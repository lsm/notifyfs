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
#include <dirent.h>

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

#define LOG_LOGAREA LOG_LOGAREA_WATCHES

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "testfs.h"
#include "entry-management.h"
#include "path-resolution.h"
#include "watches.h"

struct eff_watch_list_struct {
    struct effective_watch_struct *first;
    struct effective_watch_struct *last;
};

unsigned long long watchctr = 1;
pthread_mutex_t watchctr_mutex=PTHREAD_MUTEX_INITIALIZER;

struct eff_watch_list_struct eff_watches_list={NULL, NULL};
pthread_mutex_t effective_watches_mutex=PTHREAD_MUTEX_INITIALIZER;;

struct effective_watch_struct *effective_watches_unused=NULL;
pthread_mutex_t effective_watches_unused_mutex=PTHREAD_MUTEX_INITIALIZER;;

extern struct testfs_options_struct testfs_options;


struct effective_watch_struct *get_next_effective_watch(struct effective_watch_struct *effective_watch)
{
    if ( ! effective_watch ) {

	effective_watch=eff_watches_list.first;

    } else {

	effective_watch=effective_watch->next;

    }

    return effective_watch;

}

/* function to set the backend */

void set_backend(struct call_info_struct *call_info, struct effective_watch_struct *effective_watch)
{
    effective_watch->typebackend=FSEVENT_BACKEND_METHOD_INOTIFY; /* default */

}

/* function to get a new (global) watch id 
   by just increasing the global watch counter 
   this watch counter is used to identify effective_watches,
   and to communicate with backends, other than inotify (inotify supplies a id itself)
*/

unsigned long new_watchid()
{

    pthread_mutex_lock(&watchctr_mutex);

    watchctr++;

    pthread_mutex_unlock(&watchctr_mutex);

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
        effective_watch->backend=NULL;
        effective_watch->typebackend=0;
        effective_watch->id=0;
        effective_watch->backend_id=0;
        effective_watch->backendset=0;
        effective_watch->path=NULL;

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
   important is to keep the list sorted (by path) */

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

	tmp_effective_watch=eff_watches_list.first;
	res=0;

	while ( tmp_effective_watch ) {

	    res=strcmp(effective_watch->path, tmp_effective_watch->path);

	    if ( res<=0 ) break;

	    tmp_effective_watch=tmp_effective_watch->next;

	}

	if ( res>0 ) {

	    /* there is no bigger: add it at tail */

	    eff_watches_list.last->next=effective_watch;
	    effective_watch->prev=eff_watches_list.last;
	    eff_watches_list.last=effective_watch;

	} else {

	    /* tmp_effective_watch is "bigger" : insert before tmp_effective_watch */

	    if ( tmp_effective_watch->prev ) tmp_effective_watch->prev->next=effective_watch;
	    effective_watch->prev=tmp_effective_watch->prev;

	    effective_watch->next=tmp_effective_watch;
	    tmp_effective_watch->prev=effective_watch;

	    if ( eff_watches_list.first==tmp_effective_watch ) eff_watches_list.first=effective_watch;

	}

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

    if ( type==FSEVENT_BACKEND_METHOD_NOTSET ) {

	logoutput("lookup_watch for id %li", id);

	pthread_mutex_lock(&effective_watches_mutex);

	effective_watch=eff_watches_list.first;

	while (effective_watch) {

    	    if ( effective_watch->id==id ) break;

    	    effective_watch=effective_watch->next;

	}

	pthread_mutex_unlock(&effective_watches_mutex);

    } else {

	logoutput("lookup_watch for backend type %i id %li", type, id);

	pthread_mutex_lock(&effective_watches_mutex);

	effective_watch=eff_watches_list.first;

	while (effective_watch) {

    	    if ( effective_watch->backend_id==id && effective_watch->typebackend==type ) break;

    	    effective_watch=effective_watch->next;

	}

	pthread_mutex_unlock(&effective_watches_mutex);

    }

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


void del_watch_at_backend(struct effective_watch_struct *effective_watch)
{
    int res;

    if ( effective_watch->backendset==0 ) {

	logoutput("del_watch_backend: backend watch id %li is not set, not required to take action", effective_watch->backend_id);
	return;

    }

    if ( effective_watch->typebackend==FSEVENT_BACKEND_METHOD_INOTIFY ) {

	res=inotify_rm_watch(testfs_options.inotify_fd, (int) effective_watch->backend_id);

        if ( res==-1 ) {

            logoutput("del_watch_backend: deleting inotify watch %li gives error: %i", effective_watch->backend_id, errno);

        } else {

            effective_watch->backend_id=0;
            effective_watch->backendset=0;

        }

    } else {

        logoutput("del_watch_backend: error: backend %i not reckognized.", effective_watch->typebackend);

    }

}

/* 	function to call backend notify methods like:
	- inotify_add_watch
	- send a forward message to client fs

	called when setting a watch via xattr, but also
	when a fs is unmounted normally or via autofs

*/

void set_watch_at_backend(struct effective_watch_struct *effective_watch, int newmask)
{
    int res, modifiedmask=newmask;

    if ( effective_watch->typebackend==FSEVENT_BACKEND_METHOD_INOTIFY ) {

        logoutput("set_watch_backend: setting inotify watch on %s with mask %i", effective_watch->path, newmask);

	/* don't follow symlinks */

	modifiedmask|=IN_DONT_FOLLOW;

	/* don not process watches unlinked inodes */

	modifiedmask|=IN_EXCL_UNLINK;

        res=inotify_add_watch(testfs_options.inotify_fd, effective_watch->path, modifiedmask);

        if ( res==-1 ) {

            logoutput("set_watch_backend: setting inotify watch gives error: %i", errno);

        } else {

	    if ( effective_watch->backendset==1 ) {

		/* when inotify_add_watch is called on a path where a watch has already been set, 
		   the watch id should be the same, it's an update... 
		   only log when this is not the case */

		if ( res != effective_watch->backend_id ) {

		    logoutput("set_watch_backend: inotify watch returns a different id: %i versus %li", res, effective_watch->backend_id);

		}

	    }

	    logoutput("set_watch_backend: set backend id %i", res);

            effective_watch->backend_id=(unsigned long) res;
            effective_watch->backendset=1;

        }

    } else {

        logoutput("Error: backend %i not reckognized.", effective_watch->typebackend);

    }

}