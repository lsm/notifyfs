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

#define DIRECTORYINDEX_SIZE			100

#define NAMEINDEX_ROOT1				91
#define NAMEINDEX_ROOT2				8281
#define NAMEINDEX_ROOT3				753571

struct nameindex_struct {
    struct notifyfs_entry_struct *entry;
    int count;
};

struct directory_struct {
    struct nameindex_struct nameindex[NAMEINDEX_ROOT1];
    int count;
};

struct notifyfs_inode_struct **inode_hash_table;
struct directory_struct directory_table[DIRECTORYINDEX_SIZE];

pthread_mutex_t inodectrmutex=PTHREAD_MUTEX_INITIALIZER;
unsigned long long inoctr = FUSE_ROOT_ID;

struct notifyfs_inode_struct rootinode;
struct notifyfs_entry_struct rootentry;
unsigned char rootcreated=0;


int init_hashtables()
{
    int nreturn=0, i, j;

    inode_hash_table = calloc(id_table_size, sizeof(struct notifyfs_inode_struct *));

    if ( ! inode_hash_table ) {

	nreturn=-ENOMEM;
	goto error;

    }

    for (i=1;i<=DIRECTORYINDEX_SIZE;i++) {

	directory_table[i-1].count=0;

	for (j=1;j<=NAMEINDEX_ROOT1;j++) {

	    directory_table[i-1].nameindex[j-1].entry=NULL;
	    directory_table[i-1].nameindex[j-1].count=0;

	}

    }

    return 0;

    error:

    if ( inode_hash_table) free(inode_hash_table);

    return nreturn;

}

static size_t inode_2_hash(fuse_ino_t ino)
{
    return ino % id_table_size;
}


void add_to_inode_hash_table(struct notifyfs_inode_struct *inode)
{
    size_t idh = inode_2_hash(inode->ino);

    inode->id_next = inode_hash_table[idh];
    inode_hash_table[idh] = inode;
}


void add_to_name_hash_table(struct notifyfs_entry_struct *entry)
{
    int nameindex_value=0, res;
    int lenname=strlen(entry->name);
    char *name=entry->name;
    unsigned char firstletter=*(name)-32;
    unsigned char secondletter=0;
    unsigned char thirdletter=0;
    unsigned char fourthletter=0;
    int inoindex=0;
    struct nameindex_struct *nameindex=NULL;
    struct notifyfs_entry_struct *next_entry, *keep_entry=NULL;

    if ( ! entry->parent ) {

	logoutput("add_to_name_hash_table: %s has no parent", entry->name);

	return;

    }

    inoindex=entry->parent->inode->ino % DIRECTORYINDEX_SIZE;

    nameindex=&(directory_table[inoindex].nameindex[firstletter]);

    /* get the first four letters 

    it's possible that the name is not that long 
    in any case the first letter is always defined 
    */

    if (lenname>=4) {

	secondletter=*(name+1)-32;
	thirdletter=*(name+2)-32;
	fourthletter=*(name+3)-32;

    } else if (lenname==3) {

	secondletter=*(name+1)-32;
	thirdletter=*(name+2)-32;

    } else if (lenname==2) {

	secondletter=*(name+1)-32;

    }

    nameindex_value=secondletter * NAMEINDEX_ROOT2 + thirdletter * NAMEINDEX_ROOT1 + fourthletter;
    entry->nameindex_value=nameindex_value;

    next_entry=nameindex->entry;

    while (next_entry) {

	keep_entry=next_entry;

	if (nameindex_value==next_entry->nameindex_value) {

	    /* look futher, indexvalue is the same, but the name may differ */

	    while (next_entry) {

		keep_entry=next_entry;

		res=strcmp(next_entry->name, entry->name);

		if (res>=0) {

		    if (res==0) {

			logoutput("add_to_name_hash_table: %s already present!", entry->name);
			return;

		    }

		    goto insert;

		}

		next_entry=next_entry->name_next;

	    }

	    break;

	} else if (nameindex_value<next_entry->nameindex_value) {

	    /* index value bigger, so the name is also "bigger": the right next value is found */

	    break;

	}

	next_entry=next_entry->name_next;

    }

    insert:

    if (next_entry) {

	/* a next entry is found */

	nameindex->count++;
	directory_table[inoindex].count++;

	entry->name_next=next_entry;

	if (next_entry==nameindex->entry) {

	    nameindex->entry=entry;
	    entry->name_prev=NULL;

	} else {

	    entry->name_prev=next_entry->name_prev;
	    next_entry->name_prev->name_next=entry;

	}

	next_entry->name_prev=entry;

    } else if (keep_entry) {

	/* next entry is empty, but a "prev" entry is found 
	probably at end of list
	*/

	nameindex->count++;
	directory_table[inoindex].count++;

	keep_entry->name_next=entry;
	entry->name_prev=keep_entry;

	entry->name_next=NULL;

    } else {

	/* no next and prev, probably empty */

	nameindex->count++;
	directory_table[inoindex].count++;

	nameindex->entry=entry;

	entry->name_next=NULL;
	entry->name_prev=NULL;

    }

}

void remove_entry_from_name_hash(struct notifyfs_entry_struct *entry)
{
    struct notifyfs_entry_struct *next=entry->name_next;
    struct notifyfs_entry_struct *prev=entry->name_prev;
    unsigned char firstletter=*(entry->name)-32;
    int inoindex=0;
    struct nameindex_struct *nameindex=NULL;

    if ( ! entry->parent ) return;

    inoindex=entry->parent->inode->ino % DIRECTORYINDEX_SIZE;

    nameindex=&(directory_table[inoindex].nameindex[firstletter]);

    if (entry==nameindex->entry) nameindex->entry=next;
    if (next) next->name_prev=prev;
    if (prev) prev->name_next=next;

    entry->name_prev=NULL;
    entry->name_next=NULL;

    nameindex->count--;
    directory_table[inoindex].count--;

}

struct notifyfs_inode_struct *find_inode(fuse_ino_t ino)
{
    struct notifyfs_inode_struct *tmpinode = inode_hash_table[inode_2_hash(ino)];

    while (tmpinode && tmpinode->ino != ino) tmpinode = tmpinode->id_next;

    return tmpinode;

}

struct notifyfs_entry_struct *find_entry_raw(struct notifyfs_entry_struct *parent, const char *name, unsigned char exact)
{
    int nameindex_value=0, lenname=strlen(name);
    unsigned char firstletter=*(name)-32;
    unsigned char secondletter=0;
    unsigned char thirdletter=0;
    unsigned char fourthletter=0;
    int inoindex=parent->inode->ino % DIRECTORYINDEX_SIZE;
    struct nameindex_struct *nameindex=&(directory_table[inoindex].nameindex[firstletter]);
    struct notifyfs_entry_struct *entry=NULL;

    if (directory_table[inoindex].count==0) goto out;
    if (nameindex->count==0) goto out;

    if (lenname>=4) {

	secondletter=*(name+1)-32;
	thirdletter=*(name+2)-32;
	fourthletter=*(name+3)-32;

    } else if (lenname==3) {

	secondletter=*(name+1)-32;
	thirdletter=*(name+2)-32;

    } else if (lenname==2) {

	secondletter=*(name+1)-32;

    }

    nameindex_value=secondletter * NAMEINDEX_ROOT2 + thirdletter * NAMEINDEX_ROOT1 + fourthletter;
    entry=nameindex->entry;

    while (entry) {

	if (nameindex_value>entry->nameindex_value) {

	    /* before name */

	    entry=entry->name_next;
	    continue;

	} else if (nameindex_value==entry->nameindex_value) {

	    while (entry) {

		/* index value (first 4 letters) is the same : compare full names */

		if (entry->parent==parent) {

		    if (strcmp(entry->name, name)==0) {

			goto out;

		    } else if (strcmp(entry->name, name)>0 && exact==0) {

			goto out;

		    }

		}

		entry=entry->name_next;

		if (! entry) goto out;

		if (nameindex_value<entry->nameindex_value) {

		    if (exact==1) {

			entry=NULL;
			goto out;

		    } else {

			goto out;

		    }

		}

	    }

	} else if (nameindex_value<entry->nameindex_value) {

	    /* past name */

	    if (exact==1) {

		entry=NULL;
		break;

	    } else {

		break;

	    }

	}

    }

    out:

    if (entry) {

	logoutput("find_entry_raw: %s found", name);

    } else {

	logoutput("find_entry_raw: %s not found", name);

    }

    return entry;

}

struct notifyfs_entry_struct *find_entry(fuse_ino_t ino, const char *name)
{
    struct notifyfs_inode_struct *inode=find_inode(ino);

    if (inode) {

	if (inode->alias) return find_entry_raw(inode->alias, name, 1);

    }

    return NULL;

}

static struct notifyfs_entry_struct *lookup_first_entry(struct notifyfs_entry_struct *parent, unsigned char i)
{
    struct notifyfs_entry_struct *entry=NULL;
    int inoindex=parent->inode->ino % DIRECTORYINDEX_SIZE;

    while ( i<NAMEINDEX_ROOT1 && ! entry) {

	if (directory_table[inoindex].nameindex[i].count>0) {

	    entry=directory_table[inoindex].nameindex[i].entry;

	    while (entry) {

		if (entry->parent==parent) goto out;

		entry=entry->name_next;


	    }

	}

	i++;

    }

    out:

    return entry;

}

/* function which does a lookup of the next entry with the same parent 
    used for getting the contents of a directory */

struct notifyfs_entry_struct *get_next_entry(struct notifyfs_entry_struct *parent, struct notifyfs_entry_struct *entry)
{
    int inoindex=parent->inode->ino % DIRECTORYINDEX_SIZE;

    logoutput("get_next_entry");

    if (directory_table[inoindex].count==0) return NULL;

    if ( ! entry) {

	entry=lookup_first_entry(parent, 0);

    } else {
	unsigned char i=*(entry->name)-32; /* remember the current row */

	entry=entry->name_next;

	/* next entry in the list must have the same parent */

	while (entry) {

	    if (entry->parent==parent) break;

	    entry=entry->name_next;

	}

	if ( ! entry) {

	    /* look in the first next tabel */

	    entry=lookup_first_entry(parent, i+1);

	}

    }

    if (entry) {

	logoutput("get_next_entry: found %s", entry->name);

    } else {

	logoutput("get_next_entry: no entry found");

    }

    return entry;

}

static struct notifyfs_inode_struct *create_inode()
{
    struct notifyfs_inode_struct *inode=NULL;

    pthread_mutex_lock(&inodectrmutex);

    inode = malloc(sizeof(struct notifyfs_inode_struct));

    if (inode) {

	memset(inode, 0, sizeof(struct notifyfs_inode_struct));
	inode->ino = inoctr;
	inode->nlookup = 0;
	inode->status=FSEVENT_INODE_STATUS_OK;
	inode->lastaction=0;
	inode->method=0;
	inode->effective_watch=NULL;

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
