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

#ifdef HAVE_INOTIFY
#include <sys/inotify.h>
#endif

#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_WATCHES

#define WATCHES_HASHTABLE1_SIZE          1024

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "epoll-utils.h"
#include "notifyfs.h"
#include "entry-management.h"
#include "path-resolution.h"
#include "mountinfo.h"
#include "client.h"
#include "watches.h"
#include "utils.h"

#ifdef HAVE_INOTIFY
#include "watches-backend-inotify.c"
#else
#include "watches-backend-inotify-notsup.c"
#endif

struct eff_watch_list_struct {
    struct effective_watch_struct *first;
    struct effective_watch_struct *last;
};

struct effective_watch_struct **eff_watch_hashtable1;

unsigned long long watchctr = 1;
pthread_mutex_t watchctr_mutex=PTHREAD_MUTEX_INITIALIZER;

struct eff_watch_list_struct eff_watches_list;
pthread_mutex_t effective_watches_mutex=PTHREAD_MUTEX_INITIALIZER;

struct effective_watch_struct *effective_watches_unused=NULL;
pthread_mutex_t effective_watches_unused_mutex=PTHREAD_MUTEX_INITIALIZER;

int init_watch_hashtables()
{
    int nreturn=0;
    int i;

    eff_watch_hashtable1=calloc(WATCHES_HASHTABLE1_SIZE, sizeof(struct effective_watch_struct *));

    if ( ! eff_watch_hashtable1 ) {

	nreturn=-ENOMEM;

    } else {

	for (i=0;i<WATCHES_HASHTABLE1_SIZE;i++) {

	    eff_watch_hashtable1[i]=NULL;

	}

    }

    return nreturn;

}

/* here some function to lookup the eff watch, given the mount entry */

void add_watch_to_hashtable1(struct effective_watch_struct *eff_watch, unsigned long long id)
{
    int hash=id%WATCHES_HASHTABLE1_SIZE;

    if ( eff_watch_hashtable1[hash] ) eff_watch_hashtable1[hash]->prev_hash1=eff_watch;
    eff_watch->next_hash1=eff_watch_hashtable1[hash];
    eff_watch_hashtable1[hash]=eff_watch;

}

void remove_watch_from_hashtable1(struct effective_watch_struct *eff_watch, unsigned long long id)
{
    int hash=id%WATCHES_HASHTABLE1_SIZE;

    if ( eff_watch_hashtable1[hash]==eff_watch ) {

	eff_watch_hashtable1[hash]=eff_watch->next_hash1;

    }

    if ( eff_watch->next_hash1 ) eff_watch->next_hash1->prev_hash1=eff_watch->prev_hash1;
    if ( eff_watch->prev_hash1 ) eff_watch->prev_hash1->next_hash1=eff_watch->next_hash1;

}

struct effective_watch_struct *get_next_eff_watch_hash1(struct effective_watch_struct *effective_watch, unsigned long long id)
{
    if ( effective_watch ) {
	int hash=id%WATCHES_HASHTABLE1_SIZE;

	effective_watch=eff_watch_hashtable1[hash];

    } else {

	effective_watch=effective_watch->next_hash1;

    }

    return effective_watch;

}

struct effective_watch_struct *get_next_effective_watch(struct effective_watch_struct *effective_watch)
{
    if ( ! effective_watch ) {

	effective_watch=eff_watches_list.first;

    } else {

	effective_watch=effective_watch->next;

    }

    return effective_watch;

}

/* function to set the path for the effective watch 
   this is the path relative to the mount point of the 
   fs it's on
*/

int set_mount_entry_effective_watch(struct call_info_struct *call_info, struct effective_watch_struct *effective_watch)
{
    struct mount_entry_struct *mount_entry=NULL;
    char *path;
    int len, nreturn=0;

    if ( call_info->mount_entry ) {
	int len, res;

	mount_entry=call_info->mount_entry;

    } else {

	if ( call_info->path ) {
	    struct notifyfs_entry_struct *entry=call_info->entry;

	    /* walk back to root */

	    checkentry:

	    if ( isrootentry(entry) ) {

		mount_entry=get_rootmount();

	    } else {

		if ( entry->mount_entry ) {

		    mount_entry=entry->mount_entry;

		} else {

		    entry=entry->parent;
		    goto checkentry;

		}

	    }

	} else {
	    int res=determine_path(call_info, NOTIFYFS_PATH_FORCE);

	    if (res<0) {

		nreturn=res;
		goto out;

	    }

	    mount_entry=call_info->mount_entry;

	}

    }

    effective_watch->mount_entry=mount_entry;
    len=strlen(mount_entry->mountpoint);

    if ( strlen(call_info->path)>len ) {

	/* here call_info->path is a real subdirectory of mount_entry->mountpoint */

	if ( *(call_info->path+len)=='/' ) len++;

	effective_watch->path=strdup(call_info->path+len);

	if ( ! effective_watch->path ) {

	    nreturn=-ENOMEM;

	}

    }

    out:

    return nreturn;

}



/* function to get a new (global) watch id 
   by just increasing the global watch counter 
   this watch counter is used to identify effective_watches,
   and to communicate with backends, other than inotify (inotify supplies a id itself)
*/

unsigned long new_watchid()
{
    int res;

    res=pthread_mutex_lock(&watchctr_mutex);

    watchctr++;

    res=pthread_mutex_unlock(&watchctr_mutex);

    return watchctr;
}

struct effective_watch_struct *get_effective_watch()
{
    struct effective_watch_struct *effective_watch=NULL;
    int res;
    unsigned char fromunused=0;

    /* try pthread_mutex_trylock here... no waiting */

    res=pthread_mutex_trylock(&effective_watches_unused_mutex);

    if ( res==0 ) {

	fromunused=1;

	if ( effective_watches_unused ) {

	    effective_watch=effective_watches_unused;
	    effective_watches_unused=effective_watch->next;

	} else {

	    fromunused=0;

	}

	res=pthread_mutex_unlock(&effective_watches_unused_mutex);

    }

    if ( fromunused==0 ) effective_watch=malloc(sizeof(struct effective_watch_struct));

    if ( effective_watch ) {

	if ( fromunused==1 ) res=pthread_mutex_lock(&effective_watch->lock_mutex);

        effective_watch->mask=0;
        effective_watch->inode=NULL;
        effective_watch->next=NULL;
        effective_watch->prev=NULL;

	if ( fromunused==0 ) {

    	    pthread_mutex_init(&effective_watch->lock_mutex, NULL);
    	    pthread_cond_init(&effective_watch->lock_condition, NULL);

	}

        effective_watch->lock=0;
        effective_watch->nrwatches=0;
        effective_watch->watches=NULL;
        effective_watch->backend=NULL;
        effective_watch->typebackend=0;
        effective_watch->id=0;
        effective_watch->backend_id=0;
        effective_watch->inotify_id=0;
        effective_watch->backendset=0;
        effective_watch->path=NULL;
	effective_watch->laststat=0;

	if ( fromunused==1 ) res=pthread_mutex_unlock(&effective_watch->lock_mutex);

    }

    return effective_watch;

}

int lock_effective_watches()
{
    return pthread_mutex_lock(&effective_watches_mutex);

}

int unlock_effective_watches()
{
    return pthread_mutex_unlock(&effective_watches_mutex);

}

/* add effective watch to list
   */

void add_effective_watch_to_list(struct effective_watch_struct *effective_watch)
{
    int res;
    struct effective_watch_struct *tmp_effective_watch;

    if ( effective_watch->path ) {

	logoutput("add_effective_watch_to_list: %s", effective_watch->path);

    } else {

	logoutput("add_effective_watch_to_list: unknown path");

    }

    res=pthread_mutex_lock(&effective_watches_mutex);

    if ( ! eff_watches_list.first ) {

	eff_watches_list.first=effective_watch;
	effective_watch->prev=NULL;

    }

    if ( ! eff_watches_list.last ) {

	eff_watches_list.last=effective_watch;
	effective_watch->next=NULL;

    } else {

	/* add it at tail */

	eff_watches_list.last->next=effective_watch;
	effective_watch->prev=eff_watches_list.last;
	eff_watches_list.last=effective_watch;

    }

    res=pthread_mutex_unlock(&effective_watches_mutex);

}


void remove_effective_watch_from_list(struct effective_watch_struct *effective_watch, unsigned char lockset)
{
    int res;

    if ( lockset==0 ) res=pthread_mutex_lock(&effective_watches_mutex);

    if ( eff_watches_list.first==effective_watch ) eff_watches_list.first=effective_watch->next;
    if ( eff_watches_list.last==effective_watch ) eff_watches_list.last=effective_watch->prev;

    if ( effective_watch->next ) effective_watch->next->prev=effective_watch->prev;
    if ( effective_watch->prev ) effective_watch->prev->next=effective_watch->next;

    if ( lockset==0 ) res=pthread_mutex_unlock(&effective_watches_mutex);

}

void move_effective_watch_to_unused(struct effective_watch_struct *effective_watch)
{

    int res;

    res=pthread_mutex_lock(&effective_watches_unused_mutex);

    effective_watch->prev=NULL;
    effective_watch->next=NULL;

    if ( effective_watches_unused ) effective_watches_unused->prev=effective_watch;

    effective_watch->next=effective_watches_unused;
    effective_watches_unused=effective_watch;

    res=pthread_mutex_unlock(&effective_watches_unused_mutex);

}

struct effective_watch_struct *lookup_watch(unsigned char type, unsigned long id)
{
    struct effective_watch_struct *effective_watch=NULL;
    int res;

    res=pthread_mutex_lock(&effective_watches_mutex);

    effective_watch=eff_watches_list.first;

    if ( type==FSEVENT_BACKEND_METHOD_INOTIFY ) {

	logoutput("lookup_watch id %i for inotify", id);

	while (effective_watch) {

    	    if ( effective_watch->inotify_id==id ) break;

    	    effective_watch=effective_watch->next;

	}

    } else if ( type!=FSEVENT_BACKEND_METHOD_NOTSET ) {

	logoutput("lookup_watch id %i for backend %i", type, id);

	while (effective_watch) {

    	    if ( effective_watch->backend_id==id && effective_watch->typebackend==type ) break;

    	    effective_watch=effective_watch->next;

	}

    } else {

	while (effective_watch) {

    	    if ( effective_watch->id==id ) break;

    	    effective_watch=effective_watch->next;

	}

    }

    res=pthread_mutex_unlock(&effective_watches_mutex);

    if ( effective_watch ) {

	logoutput("lookup_watch: watch found");

    } else {

	logoutput("lookup_watch: watch not found");

    }

    return effective_watch;

}

int lock_effective_watch(struct effective_watch_struct *effective_watch)
{
    int res;

    res=pthread_mutex_lock(&effective_watch->lock_mutex);

    if ( effective_watch->lock==1 ) {

    	while (effective_watch->lock==1) {

    	    res=pthread_cond_wait(&effective_watch->lock_condition, &effective_watch->lock_mutex);

    	}

    }

    effective_watch->lock=1;

    res=pthread_mutex_unlock(&effective_watch->lock_mutex);

    return res;

}

int unlock_effective_watch(struct effective_watch_struct *effective_watch)
{
    int res;

    res=pthread_mutex_lock(&effective_watch->lock_mutex);
    effective_watch->lock=0;
    res=pthread_cond_broadcast(&effective_watch->lock_condition);
    res=pthread_mutex_unlock(&effective_watch->lock_mutex);

    return res;

}

void init_effective_watches()
{

    eff_watches_list.first=NULL;
    eff_watches_list.last=NULL;

}

struct watch_struct *get_watch()
{
    struct watch_struct *watch=NULL;

    watch=malloc(sizeof(struct watch_struct));

    if ( watch ) {

        watch->mask=0;
        watch->effective_watch=NULL;
        watch->client=NULL;
        watch->next_per_watch=NULL;
        watch->prev_per_watch=NULL;
        watch->next_per_client=NULL;
        watch->prev_per_client=NULL;

    }

    return watch;

}

/* calculate the effective mask by "adding" all individual masks of watches 
   as a by product set the number of watches */

int calculate_effmask(struct effective_watch_struct *effective_watch, unsigned char lockset)
{
    int effmask=0, res;

    if ( effective_watch ) {
        struct watch_struct *watch=NULL;
        int nrwatches=0;

        if ( lockset==0 ) res=lock_effective_watch(effective_watch);

        watch=effective_watch->watches;

        while(watch) {

            effmask=effmask | watch->mask;
            nrwatches++;
            watch=watch->next_per_watch;

        }

        effective_watch->mask=effmask;
        effective_watch->nrwatches=nrwatches;

        if ( lockset==0 ) res=unlock_effective_watch(effective_watch);;

    }

    return effmask;

}

/* function to look for effective watches somewhere in a subdirectory of entry

*/

int check_for_effective_watch(char *path)
{
    int nreturn=0, res, nlen=strlen(path);
    struct effective_watch_struct *effective_watch;

    res=pthread_mutex_lock(&effective_watches_mutex);

    effective_watch=eff_watches_list.first;

    while (effective_watch) {

	if ( effective_watch->path ) {

	    if ( strlen(effective_watch->path)>nlen && strncmp(effective_watch->path, path, nlen)==0 && strncmp(effective_watch->path+nlen,"/",1)==0 ) {

		/* there is a watch in a subdirectory */

		nreturn=1;
		break;

	    }

	}

	effective_watch=effective_watch->next;

    }

    res=pthread_mutex_unlock(&effective_watches_mutex);

    return nreturn;

}

/* add a new client watch to the effective watch */

int add_new_client_watch(struct effective_watch_struct *effective_watch, int mask, int client_watch_id, struct client_struct *client)
{
    int nreturn=0;
    struct watch_struct *watch;

    watch=get_watch();

    if ( ! watch ) {

        nreturn=-ENOMEM;
        goto out;

    }

    /* add to effective watch */

    watch->effective_watch=effective_watch;
    if ( effective_watch->watches ) effective_watch->watches->prev_per_watch=watch;
    watch->next_per_watch=effective_watch->watches;
    effective_watch->watches=watch;

    /* add to client */

    watch->client=client;
    if ( client->watches ) client->watches->prev_per_client=watch;
    watch->next_per_client=client->watches;
    client->watches=watch;

    watch->mask=mask;
    watch->client_watch_id=client_watch_id;

    out:

    return nreturn;

}

/* remove a client watch from the effective watch */

void remove_client_watch_from_inode(struct watch_struct *watch)
{
    struct effective_watch_struct *effective_watch=watch->effective_watch;

    if ( effective_watch ) {

	if ( effective_watch->watches==watch ) effective_watch->watches=watch->next_per_watch;

    }

    if ( watch->prev_per_watch ) watch->prev_per_watch->next_per_watch=watch->next_per_watch;
    if ( watch->next_per_watch ) watch->next_per_watch->prev_per_watch=watch->prev_per_watch;

    watch->prev_per_watch=NULL;
    watch->next_per_watch=NULL;
    watch->effective_watch=NULL;

    effective_watch->nrwatches--;

}

void remove_client_watch_from_client(struct watch_struct *watch)
{
    struct client_struct *client=watch->client;

    /* from client */

    if ( client ) {

	if ( client->watches==watch ) client->watches=watch->next_per_client;

    }

    if ( watch->prev_per_client ) watch->prev_per_client->next_per_client=watch->next_per_client;
    if ( watch->next_per_client ) watch->next_per_client->prev_per_client=watch->prev_per_client;

    watch->prev_per_client=NULL;
    watch->next_per_client=NULL;
    watch->client=NULL;

}

void set_watch_backend_os_specific(struct effective_watch_struct *effective_watch, char *path, int mask)
{
    int res=set_watch_backend_inotify(effective_watch, path, mask);
}

void change_watch_backend_os_specific(struct effective_watch_struct *effective_watch, char *path, int mask)
{
    int res=change_watch_backend_inotify(effective_watch, path, mask);
}


void remove_watch_backend_os_specific(struct effective_watch_struct *effective_watch)
{
    remove_watch_backend_inotify(effective_watch);
}

void initialize_fsnotify_backends()
{
    initialize_inotify();
}

void close_fsnotify_backends()
{
    close_inotify();
}
