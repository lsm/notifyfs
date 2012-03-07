/*
  2010, 2011 Stef Bon <stefbon@gmail.com>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "global-defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>

#include <inttypes.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>

#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_PATH_RESOLUTION

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "testfs.h"
#include "entry-management.h"

pthread_mutex_t inodectrmutex=PTHREAD_MUTEX_INITIALIZER;

#ifdef SIZE_INODE_HASHTABLE
static size_t id_table_size=SIZE_INODE_HASHTABLE;
#else
static size_t id_table_size=32768;
#endif

#ifdef SIZE_NAME_HASHTABLE
static size_t name_table_size=SIZE_NAME_HASHTABLE;
#else
static size_t name_table_size=32768;
#endif

struct testfs_inode_struct **inode_hash_table;
struct testfs_entry_struct **name_hash_table;

unsigned long long inoctr = FUSE_ROOT_ID;

unsigned char call_info_lock;
pthread_mutex_t call_info_mutex;
pthread_cond_t call_info_condition;
struct call_info_struct *call_info_list=NULL;

struct call_info_struct *call_info_unused=NULL;
pthread_mutex_t call_info_unused_mutex;


struct effective_watch_struct *effective_watches_list=NULL;
pthread_mutex_t effective_watches_mutex;

//
// basic functions for managing inodes and entries
//

int init_hashtables()
{
    int nreturn=0;

    inode_hash_table = calloc(id_table_size, sizeof(struct testfs_entry_struct *));

    if ( ! inode_hash_table ) {

	nreturn=-ENOMEM;
	goto error;

    }

    name_hash_table = calloc(name_table_size, sizeof(struct testfs_entry_struct *));

    if ( ! name_hash_table ) {

	nreturn=-ENOMEM;
	goto error;

    }

    return 0;

    error:

    if ( inode_hash_table) free(inode_hash_table);
    if ( name_hash_table) free(name_hash_table);

    return nreturn;

}

static size_t inode_2_hash(fuse_ino_t inode)
{
	return inode % id_table_size;
}

static size_t name_2_hash(fuse_ino_t parent_inode, const char *name)
{
	uint64_t hash = parent_inode;

	for (; *name; name++) hash = hash * 31 + (unsigned char) *name;

	return hash % name_table_size;
}

void add_to_inode_hash_table(struct testfs_inode_struct *inode)
{
	size_t idh = inode_2_hash(inode->ino);

	inode->id_next = inode_hash_table[idh];
	inode_hash_table[idh] = inode;
}


void add_to_name_hash_table(struct testfs_entry_struct *entry)
{
	size_t tmphash = name_2_hash(entry->parent->inode->ino, entry->name);
	struct testfs_entry_struct *next_entry = name_hash_table[tmphash];

	if (next_entry) {

	    next_entry->name_prev = entry;

	    entry->name_next = next_entry;
	    entry->name_prev = NULL;


	} else {

	    // there is no first, so this becomes the first, a circular list of one element

	    entry->name_next=NULL;
	    entry->name_prev=NULL;

	}

	// point to the new inserted entry.. well the pointer to it

	name_hash_table[tmphash] = entry;
	entry->namehash=tmphash;

}

void remove_entry_from_name_hash(struct testfs_entry_struct *entry)
{
    struct testfs_entry_struct *next_entry = entry->name_next;
    struct testfs_entry_struct *prev_entry = entry->name_prev;

    if ( next_entry == NULL && prev_entry == NULL ) {

	// entry is the last one...no next and no prev

	name_hash_table[entry->namehash]=NULL;

    } else {

	// if removing the first one, shift the list pointer 

	if (name_hash_table[entry->namehash]==entry) {

	    if (next_entry) {

		name_hash_table[entry->namehash]=next_entry;

	    } else {

		name_hash_table[entry->namehash]=prev_entry;

	    }

	}

	if (next_entry) next_entry->name_prev=prev_entry;
	if (prev_entry) prev_entry->name_next=next_entry;

    }

    entry->name_prev = NULL;
    entry->name_next = NULL;
    entry->namehash = 0;

}


struct testfs_inode_struct *find_inode(fuse_ino_t inode)
{
    struct testfs_inode_struct *tmpinode = inode_hash_table[inode_2_hash(inode)];

    while (tmpinode && tmpinode->ino != inode) tmpinode = tmpinode->id_next;

    return tmpinode;

}


struct testfs_entry_struct *find_entry(fuse_ino_t parent, const char *name)
{
    struct testfs_entry_struct *tmpentry = name_hash_table[name_2_hash(parent, name)];

    while (tmpentry) {

	if (tmpentry->parent && tmpentry->parent->inode->ino == parent && strcmp(tmpentry->name, name) == 0) break;

	tmpentry = tmpentry->name_next;

    }

    return tmpentry;
}


static struct testfs_inode_struct *create_inode()
{
    struct testfs_inode_struct *inode=NULL;

    // make threadsafe

    pthread_mutex_lock(&inodectrmutex);

    inode = malloc(sizeof(struct testfs_inode_struct));

    if (inode) {

	memset(inode, 0, sizeof(struct testfs_inode_struct));
	inode->ino = inoctr;
	inode->nlookup = 0;

	inoctr++;

    }

    pthread_mutex_unlock(&inodectrmutex);

    return inode;

}

static void init_entry(struct testfs_entry_struct *entry)
{
    entry->inode=NULL;
    entry->name=NULL;

    entry->parent=NULL;
    entry->ptr=NULL;

    // to maintain a table overall
    entry->name_next=NULL;
    entry->name_prev=NULL;

    /* in case of a directory pointer to the first child (which on his turn points to another entry in the same dir*/

    entry->child=NULL;
    entry->status=0;

}


struct testfs_entry_struct *create_entry(struct testfs_entry_struct *parent, const char *name, struct testfs_inode_struct *inode)
{
    struct testfs_entry_struct *entry;

    entry = malloc(sizeof(struct testfs_entry_struct));

    if (entry) {

	memset(entry, 0, sizeof(struct testfs_entry_struct));
	init_entry(entry);

	entry->name = strdup(name);

	if (!entry->name) {

	    free(entry);
	    entry = NULL;

	} else {

	    entry->parent = parent;

	    if (inode != NULL) {

		entry->inode = inode;
		inode->alias=entry;

	    }

	}

    }

    return entry;

}

void remove_entry(struct testfs_entry_struct *entry)
{

    free(entry->name);

    if ( entry->inode ) {

	entry->inode->alias=NULL;

    }

    free(entry);

}

void assign_inode(struct testfs_entry_struct *entry)
{

    entry->inode=create_inode();

    if ( entry->inode ) {

	entry->inode->alias=entry;

    }

}

struct testfs_entry_struct *new_entry(fuse_ino_t parent, const char *name)
{
    struct testfs_entry_struct *entry;

    entry = create_entry(find_inode(parent)->alias, name, NULL);

    if ( entry ) {

	assign_inode(entry);

	if ( ! entry->inode ) {

	    remove_entry(entry);
	    entry=NULL;

	}

    }

    return entry;
}

unsigned char rootinode(struct testfs_inode_struct *inode)
{

    return (inode->ino==FUSE_ROOT_ID) ? 1 : 0;

}

unsigned long long get_inoctr()
{
    return inoctr;
}

int determine_path(struct call_info_struct *call_info, unsigned char flags)
{
    char *pathstart=NULL;
    size_t lenname;
    int nreturn=0;
    struct testfs_entry_struct *tmpentry=call_info->entry;
    pathstring tmppath;
    size_t size=sizeof(pathstring);

    logoutput1("determine_path, name: %s", tmpentry->name);

    /* start of path */

    pathstart = tmppath + size - 1;
    *pathstart = '\0';

    if ( tmpentry->status==ENTRY_STATUS_REMOVED && ! (flags & TESTFS_PATH_FORCE) ) {

        nreturn=-ENOENT;
        goto error;

    }

    if ( ! call_info->ptr ) call_info->ptr=tmpentry->ptr;

    if ( tmpentry->inode ) {

        if ( rootinode(tmpentry->inode)==1 ) {

	    logoutput1("determine_path, testing is rootentry: yes");

            pathstart-=1;
            *pathstart = '.';
	    goto out;

        } else {

	    logoutput1("determine_path, testing is rootentry: no");

	}

    }

    logoutput2("normal entry, adding %s", tmpentry->name);

    lenname=strlen(tmpentry->name);
    pathstart -= (long) lenname;
    memcpy(pathstart, tmpentry->name, lenname);

    while (tmpentry->parent) {

	tmpentry=tmpentry->parent;

        if ( ! call_info->ptr ) call_info->ptr=tmpentry->ptr;

	if ( rootinode(tmpentry->inode)==1 ) break;

        if ( tmpentry->status==ENTRY_STATUS_REMOVED && ! (flags & TESTFS_PATH_FORCE) ) {

            nreturn=-ENOENT;
            goto error;

        }

	// add the name of this entry (at the start of path) and a slash to separate it

	lenname=strlen(tmpentry->name);

	if ( pathstart - tmppath < 1 + (long) lenname ) {

	    nreturn=-ENAMETOOLONG;
	    goto error;

	}

	pathstart--;
	*pathstart = '/';

	pathstart -= lenname;

	memcpy(pathstart, tmpentry->name, lenname);

    }

    out:

    /* create a path just big enough */

    call_info->path=malloc(tmppath+size-pathstart+1);

    if ( call_info->path ) {

        memset(call_info->path, '\0', tmppath+size-pathstart+1);
        memcpy(call_info->path, pathstart, tmppath+size-pathstart);
        logoutput2("result after memcpy: %s", call_info->path);

    } else {

        nreturn=-ENOMEM;

    }


    error:

    if ( nreturn<0 ) {

        logoutput2("determine_path, error: %i", nreturn);

    }

    return nreturn;

}

struct effective_watch_struct *get_effective_watch()
{
    struct effective_watch_struct *effective_watch=NULL;

    effective_watch=malloc(sizeof(struct effective_watch_struct));

    if ( effective_watch ) {

        effective_watch->mask=0;
        effective_watch->inode=NULL;
        effective_watch->next=NULL;
        effective_watch->prev=NULL;
        pthread_mutex_init(&effective_watch->lock_mutex, NULL);
        pthread_cond_init(&effective_watch->lock_condition, NULL);
        effective_watch->lock=0;
        effective_watch->id=0;

    }

    return effective_watch;

}


void add_effective_watch_to_list(struct effective_watch_struct *effective_watch)
{
    int res;

    res=pthread_mutex_lock(&effective_watches_mutex);

    effective_watch->next=effective_watches_list;
    if ( effective_watches_list ) effective_watches_list->prev=effective_watch;
    effective_watches_list=effective_watch;
    effective_watch->prev=NULL;

    res=pthread_mutex_unlock(&effective_watches_mutex);

}

void remove_effective_watch_from_list(struct effective_watch_struct *effective_watch)
{
    int res;

    res=pthread_mutex_lock(&effective_watches_mutex);

    if ( effective_watches_list==effective_watch ) effective_watches_list=effective_watch->next;
    if ( effective_watch->next ) effective_watch->next->prev=effective_watch->prev;
    if ( effective_watch->prev ) effective_watch->prev->next=effective_watch->next;

    res=pthread_mutex_unlock(&effective_watches_mutex);

}

struct effective_watch_struct *lookup_watch(unsigned char type, unsigned long id)
{
    struct effective_watch_struct *effective_watch=NULL;
    int res;

    /* lock */

    res=pthread_mutex_lock(&effective_watches_mutex);

    effective_watch=effective_watches_list;

    while (effective_watch) {

        if ( effective_watch->id==id ) break;

        effective_watch=effective_watch->next;

    }

    /* unlock */

    res=pthread_mutex_unlock(&effective_watches_mutex);

    return effective_watch;

}

/* functions to manage the calls:
   - add call_info 
   - lookup_call_info
   - remove call_info

*/

struct call_info_struct *create_call_info()
{
    struct call_info_struct *call_info=NULL;


    call_info=malloc(sizeof(struct call_info_struct));


    return call_info;

}

void add_call_info_to_list(struct call_info_struct *call_info)
{
    int res;

    /* add to list */

    res=pthread_mutex_lock(&call_info_mutex);

    if ( call_info_list ) call_info_list->prev=call_info;
    call_info->next=call_info_list;
    call_info->prev=NULL;
    call_info_list=call_info;

    res=pthread_mutex_unlock(&call_info_mutex);

}

void init_call_info(struct call_info_struct *call_info, struct testfs_entry_struct *entry)
{

    call_info->threadid=pthread_self();
    call_info->entry=entry;
    call_info->entry2remove=entry;
    call_info->path=NULL;
    call_info->backend=NULL;
    call_info->next=NULL;
    call_info->prev=NULL;
    call_info->ptr=NULL;

}



struct call_info_struct *get_call_info(struct testfs_entry_struct *entry)
{
    int res;
    struct call_info_struct *call_info=NULL;

    res=pthread_mutex_lock(&call_info_unused_mutex);

    if (call_info_unused) {

        call_info=call_info_unused;
        call_info_unused=call_info->next;

    } else {

        call_info=create_call_info();

        if ( ! call_info ) goto out;

    }

    res=pthread_mutex_unlock(&call_info_unused_mutex);

    init_call_info(call_info, entry);

    out:

    return call_info;

}

/* function to test a call is present with 
   a subdirectory/child of path

   return:
   0: not found
   1: found
*/

int lookup_call_info(char *path, unsigned char lockset)
{
    int nreturn=0, res;

    if (lockset==0) res=pthread_mutex_lock(&call_info_mutex);

    if ( call_info_list ) {
        struct call_info_struct *call_info=call_info_list;

        while(call_info) {

            if ( strlen(call_info->path) > strlen(path) && strncmp(path, call_info->path, strlen(path))==0 ) {

                nreturn=1;
                break;

            }

            call_info=call_info->next;

        }

    }

    if (lockset==0) res=pthread_mutex_unlock(&call_info_mutex);

    return nreturn;

}

int wait_for_calls(char *path)
{
    int nreturn=0, res;

    res=pthread_mutex_lock(&call_info_mutex);

    res=lookup_call_info(path, 1);

    if ( res==1 ) {

        /* there is at least one call... wait for it to finish */

        wait:

        res=pthread_cond_wait(&call_info_condition, &call_info_mutex);

        res=lookup_call_info(path, 1);

        if ( res==1 ) goto wait;

    }

    res=pthread_mutex_unlock(&call_info_mutex);

    return nreturn;

}

void remove_call_info_from_list(struct call_info_struct *call_info)
{
    int res=0;

    res=pthread_mutex_lock(&call_info_mutex);

    if ( call_info->prev ) call_info->prev->next=call_info->next;
    if ( call_info->next ) call_info->next->prev=call_info->prev;
    if ( call_info_list==call_info ) call_info_list=call_info->next;

    /* signal waiting operations call on path is removed*/

    res=pthread_cond_broadcast(&call_info_condition);

    res=pthread_mutex_unlock(&call_info_mutex);

}




void remove_call_info(struct call_info_struct *call_info)
{
    int res=0;

    if (call_info->path) free(call_info->path);

    /* move to unused list */

    res=pthread_mutex_lock(&call_info_unused_mutex);

    if (call_info_unused) call_info_unused->prev=call_info;
    call_info->next=call_info_unused;
    call_info_unused=call_info;
    call_info->prev=NULL;

    res=pthread_mutex_unlock(&call_info_unused_mutex);

}
