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
#include <sys/epoll.h>
#include <sys/inotify.h>

#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_INODES

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "notifyfs.h"
#include "entry-management.h"
#include "mountinfo.h"
#include "client.h"

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

struct notifyfs_inode_struct **inode_hash_table;
struct notifyfs_entry_struct **name_hash_table;

pthread_mutex_t inodectrmutex=PTHREAD_MUTEX_INITIALIZER;
unsigned long long inoctr = FUSE_ROOT_ID;

struct notifyfs_inode_struct rootinode;
struct notifyfs_entry_struct rootentry;
unsigned char rootcreated=0;

//
// basic functions for managing inodes and entries
//

int init_hashtables()
{
    int nreturn=0;

    inode_hash_table = calloc(id_table_size, sizeof(struct notifyfs_inode_struct *));

    if ( ! inode_hash_table ) {

	nreturn=-ENOMEM;
	goto error;

    }

    name_hash_table = calloc(name_table_size, sizeof(struct notifyfs_entry_struct *));

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

static size_t inode_2_hash(fuse_ino_t ino)
{
	return ino % id_table_size;
}

static size_t name_2_hash(fuse_ino_t parent_ino, const char *name)
{
	uint64_t hash = parent_ino;

	for (; *name; name++) hash = hash * 31 + (unsigned char) *name;

	return hash % name_table_size;
}

void add_to_inode_hash_table(struct notifyfs_inode_struct *inode)
{
	size_t idh = inode_2_hash(inode->ino);

	inode->id_next = inode_hash_table[idh];
	inode_hash_table[idh] = inode;
}


void add_to_name_hash_table(struct notifyfs_entry_struct *entry)
{
	size_t tmphash = name_2_hash(entry->parent->inode->ino, entry->name);
	struct notifyfs_entry_struct *next_entry = name_hash_table[tmphash];

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

void remove_entry_from_name_hash(struct notifyfs_entry_struct *entry)
{
    struct notifyfs_entry_struct *next_entry = entry->name_next;
    struct notifyfs_entry_struct *prev_entry = entry->name_prev;

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


struct notifyfs_inode_struct *find_inode(fuse_ino_t ino)
{
    struct notifyfs_inode_struct *tmpinode = inode_hash_table[inode_2_hash(ino)];

    while (tmpinode && tmpinode->ino != ino) tmpinode = tmpinode->id_next;

    return tmpinode;

}


struct notifyfs_entry_struct *find_entry(fuse_ino_t parent, const char *name)
{
    struct notifyfs_entry_struct *tmpentry = name_hash_table[name_2_hash(parent, name)];

    while (tmpentry) {

	if (tmpentry->parent && tmpentry->parent->inode->ino == parent && strcmp(tmpentry->name, name) == 0) break;

	tmpentry = tmpentry->name_next;

    }

    return tmpentry;
}


static struct notifyfs_inode_struct *create_inode()
{
    struct notifyfs_inode_struct *inode=NULL;

    // make threadsafe

    pthread_mutex_lock(&inodectrmutex);

    inode = malloc(sizeof(struct notifyfs_inode_struct));

    if (inode) {

	memset(inode, 0, sizeof(struct notifyfs_inode_struct));
	inode->ino = inoctr;
	inode->nlookup = 0;
	inode->status=FSEVENT_INODE_STATUS_OK;
	inode->lastaction=0;
	inode->method=0;

	inoctr++;

    }

    pthread_mutex_unlock(&inodectrmutex);

    return inode;

}

static void init_entry(struct notifyfs_entry_struct *entry)
{
    entry->inode=NULL;
    entry->name=NULL;

    entry->parent=NULL;
    entry->mount_entry=NULL;

    // to maintain a table overall
    entry->name_next=NULL;
    entry->name_prev=NULL;

    /* in case of a directory pointer to the first child (which on his turn points to another entry in the same dir*/

    entry->child=NULL;
    entry->status=0;

}


struct notifyfs_entry_struct *create_entry(struct notifyfs_entry_struct *parent, const char *name, struct notifyfs_inode_struct *inode)
{
    struct notifyfs_entry_struct *entry;

    entry = malloc(sizeof(struct notifyfs_entry_struct));

    if (entry) {

	memset(entry, 0, sizeof(struct notifyfs_entry_struct));
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

void remove_entry(struct notifyfs_entry_struct *entry)
{

    free(entry->name);

    if ( entry->inode ) {

	entry->inode->alias=NULL;

    }

    free(entry);

}

void assign_inode(struct notifyfs_entry_struct *entry)
{

    entry->inode=create_inode();

    if ( entry->inode ) {

	entry->inode->alias=entry;

    }

}

struct notifyfs_entry_struct *new_entry(fuse_ino_t parent, const char *name)
{
    struct notifyfs_entry_struct *entry;

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

unsigned long long get_inoctr()
{
    return inoctr;
}


/* create the root inode and entry */

int create_root()
{
    int nreturn=0;

    if ( rootcreated!=0 ) goto out;

    /* rootentry */

    init_entry(&rootentry);

    rootentry.name=strdup(".");

    if ( ! rootentry.name ) {

	nreturn=-ENOMEM;
	goto out;

    }

    /* rootinode */

    pthread_mutex_lock(&inodectrmutex);

    rootinode.ino = inoctr;
    rootinode.nlookup = 0;
    rootinode.status=FSEVENT_INODE_STATUS_OK;

    inoctr++;

    pthread_mutex_unlock(&inodectrmutex);

    /* create the mutual links */

    rootentry.inode=&rootinode;
    rootinode.alias=&rootentry;

    add_to_inode_hash_table(&rootinode);
    rootcreated=1;

    out:

    return nreturn;

}

/* get the root inode */

struct notifyfs_inode_struct *get_rootinode()
{
    return &rootinode;
}

struct notifyfs_entry_struct *get_rootentry()
{
    return &rootentry;
}

unsigned char isrootinode(struct notifyfs_inode_struct *inode)
{

    return (inode->ino==FUSE_ROOT_ID) ? 1 : 0;

}

unsigned char isrootentry(struct notifyfs_entry_struct *entry)
{

    return (entry==&rootentry) ? 1 : 0;

}
