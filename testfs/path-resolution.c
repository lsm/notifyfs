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

#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_PATH_RESOLUTION

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "testfs.h"
#include "entry-management.h"
#include "path-resolution.h"
#include "watches.h"

unsigned char call_info_lock;
pthread_mutex_t call_info_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t call_info_condition;
struct call_info_struct *call_info_list=NULL;

struct call_info_struct *call_info_unused=NULL;
pthread_mutex_t call_info_unused_mutex=PTHREAD_MUTEX_INITIALIZER;

struct pathinfo_struct {
	char *path;
	size_t size;
	char *pathstart;
};


static int addtopath(struct pathinfo_struct *pathinfo, char *path, unsigned char addslash)
{
    int len=strlen(path), nreturn=0;

    /* see it fits */

    if ( addslash==1 ) {

	if ( pathinfo->pathstart - pathinfo->path < 1 + (long) len ) {

	    nreturn=-ENAMETOOLONG;
	    goto out;

	}

	pathinfo->pathstart--;
	*(pathinfo->pathstart)='/';

    } else {

	if ( pathinfo->pathstart - pathinfo->path < (long) len ) {

	    nreturn=-ENAMETOOLONG;
	    goto out;

	}

    }

    pathinfo->pathstart-=len;
    memcpy(pathinfo->pathstart, path, len);

    out:

    return nreturn;

}

int determine_path(struct call_info_struct *call_info, unsigned char flags)
{
    char *pathstart=NULL;
    int nreturn=0;
    struct testfs_entry_struct *tmpentry=call_info->entry;
    pathstring tmppath;
    size_t maxpathsize=sizeof(pathstring);
    struct pathinfo_struct pathinfo;

    logoutput("determine_path, name: %s", tmpentry->name);

    pathinfo.path=tmppath;
    pathinfo.size=sizeof(pathstring);

    /* start of path */

    pathinfo.pathstart = pathinfo.path + pathinfo.size - 1;
    *(pathinfo.pathstart) = '\0';

    if ( tmpentry->status==ENTRY_STATUS_REMOVED && ! (flags & TESTFS_PATH_FORCE) ) {

        nreturn=-ENOENT;
        goto error;

    }

    if ( tmpentry->inode ) {

	if ( ! call_info->effective_watch ) call_info->effective_watch=tmpentry->inode->effective_watch;

	if ( call_info->effective_watch ) {

	    if ( call_info->effective_watch->path ) {

		nreturn=addtopath(&pathinfo, call_info->effective_watch->path, 0);
		goto out;

	    }

	}

        if ( isrootinode(tmpentry->inode)==1 ) {

            pathinfo.pathstart--;
            *(pathinfo.pathstart)='.';
	    goto out;

        }

    }

    nreturn=addtopath(&pathinfo, tmpentry->name, 0);

    while (tmpentry->parent) {

	tmpentry=tmpentry->parent;

        if ( tmpentry->status==ENTRY_STATUS_REMOVED && ! (flags & TESTFS_PATH_FORCE) ) {

            nreturn=-ENOENT;
            goto error;

        }

	if ( tmpentry->inode ) {

	    /* get the first upstream effective watch */

	    if ( ! call_info->effective_watch ) call_info->effective_watch=tmpentry->inode->effective_watch;

	    if ( call_info->effective_watch ) {

		if ( call_info->effective_watch->path ) {

		    nreturn=addtopath(&pathinfo, call_info->effective_watch->path, 1);
		    goto out;

		}

	    }

	    if ( isrootinode(tmpentry->inode)==1 ) break;

	} else {

	    nreturn=-EIO;
	    goto error;

	}

	nreturn=addtopath(&pathinfo, tmpentry->name, 1);

    }

    out:

    if (nreturn==0) {
	size_t pathsize=pathinfo.path+pathinfo.size-pathinfo.pathstart+1;

	/* create a path just big enough */

	call_info->path=malloc(pathsize);

	if ( call_info->path ) {

    	    memset(call_info->path, '\0', pathsize);
    	    memcpy(call_info->path, pathinfo.pathstart, pathsize-1);

    	    logoutput("result after memcpy: %s", call_info->path);

	} else {

    	    nreturn=-ENOMEM;

	}

    }

    error:

    if ( nreturn<0 ) logoutput1("determine_path, error: %i", nreturn);

    return nreturn;

}

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
    call_info->effective_watch=NULL;
    call_info->path=NULL;
    call_info->backend=NULL;
    call_info->typebackend=0;
    call_info->next=NULL;
    call_info->prev=NULL;
    call_info->ctx=NULL;

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
