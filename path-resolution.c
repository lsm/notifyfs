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

#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_PATH_RESOLUTION

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "notifyfs.h"
#include "entry-management.h"
#include "path-resolution.h"
#include "watches.h"
#include "mountinfo.h"
#include "client.h"

unsigned char call_info_lock;
pthread_mutex_t call_info_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t call_info_condition;
struct call_info_struct *call_info_list=NULL;

struct call_info_struct *call_info_unused=NULL;
pthread_mutex_t call_info_unused_mutex=PTHREAD_MUTEX_INITIALIZER;

struct pathinfo_struct {
	char *path;
	size_t maxsize;
	char *pathstart;
	size_t len;
	struct mount_entry_struct *mount_entry;
};



/* function which adds a part of a path BEFORE another part of path 
   the part to be added may be a single name or a subpath
   return:
   <0 : error
   >0 : number of bytes added
*/


static int addtopath(struct pathinfo_struct *pathinfo, char *path)
{
    int len=strlen(path), nreturn=0;
    unsigned char addslash=0;

    logoutput("addtopath: add path %s", path);

    /* see it fits */

    if ( pathinfo->len>0 && *(path+len-1) != '/' ) {

	pathinfo->len+=len+1;
	addslash=1;

    } else {

	pathinfo->len+=len;

    }

    if ( pathinfo->len>pathinfo->maxsize ) {

	nreturn=-ENAMETOOLONG;
	goto out;

    }

    if ( addslash ) {

	pathinfo->pathstart--;
	*(pathinfo->pathstart)='/';
	pathinfo->len++;
	nreturn=1;

    }

    pathinfo->pathstart-=len;
    pathinfo->len+=len;
    memcpy(pathinfo->pathstart, path, len);
    nreturn+=len;

    out:

    return nreturn;

}

/* function to add extra pathinfo, from effective_watch and mount_entry 
   it checks this extra pathinfo is available 
   return:
   <0: error
   0 : no data available
   1 : ready */

static int processextrapathinfo(struct notifyfs_entry_struct *entry, struct pathinfo_struct *pathinfo)
{
    int nreturn=0;
    /* check for an effective watch attached, this will speed things up */

    if ( entry->inode ) {

	/* first: look for an effective watch, is has a path set relative to the mount entry, and 
           this mount entry has a mountpoint  */

	if (entry->inode->effective_watch ) {
	    struct effective_watch_struct *effective_watch=entry->inode->effective_watch;

	    if ( effective_watch->mount_entry) {
		struct mount_entry_struct *mount_entry=effective_watch->mount_entry;

		pathinfo->mount_entry=mount_entry;

		if ( entry==(struct notifyfs_entry_struct *) mount_entry->data0 ) {

		    /* watch is on mountpoint */

		    logoutput("determine_path: mount_entry found, add path %s", mount_entry->mountpoint);

		    nreturn=addtopath(pathinfo, mount_entry->mountpoint);
		    goto out;

		} else {

		    logoutput("determine_path: effective_watch and mount_entry found, add path %s/%s", mount_entry->mountpoint, effective_watch->path);

		    nreturn=addtopath(pathinfo, effective_watch->path);
		    if ( nreturn<0 ) goto out;

		    nreturn=addtopath(pathinfo, mount_entry->mountpoint);
		    goto out;


		}

	    }

	}

    }

    /* second: check there is a mount entry */

    if ( entry->mount_entry ) {
	struct mount_entry_struct *mount_entry=entry->mount_entry;

	pathinfo->mount_entry=mount_entry;

	logoutput("determine_path: mount_entry found, add path %s", mount_entry->mountpoint);

	nreturn=addtopath(pathinfo, mount_entry->mountpoint);
	goto out;

    }

    /* third: check it's root */

    if ( isrootentry(entry) ) {

        pathinfo->pathstart--;
        *(pathinfo->pathstart)='/';
	pathinfo->len++;
	nreturn=1;

    }

    out:

    return nreturn;

}



int determine_path(struct call_info_struct *call_info, unsigned char flags)
{
    char *pathstart=NULL;
    int nreturn=0;
    struct notifyfs_entry_struct *entry=call_info->entry;
    pathstring path;
    struct pathinfo_struct pathinfo;

    logoutput("determine_path, name: %s", entry->name);

    pathinfo.path=path;
    pathinfo.maxsize=sizeof(pathstring);
    pathinfo.len=0;
    pathinfo.pathstart = pathinfo.path + pathinfo.maxsize - 1;
    *(pathinfo.pathstart) = '\0';
    pathinfo.mount_entry=NULL;

    if ( entry->status==ENTRY_STATUS_REMOVED && ! (flags & NOTIFYFS_PATH_FORCE) ) {

        nreturn=-ENOENT;
        goto error;

    }

    /* check for extra pathinfo available */

    nreturn=processextrapathinfo(entry, &pathinfo);
    if ( nreturn!=0 ) goto out;

    nreturn=addtopath(&pathinfo, entry->name);

    while (entry->parent) {

	entry=entry->parent;

        if ( entry->status==ENTRY_STATUS_REMOVED && ! (flags & NOTIFYFS_PATH_FORCE) ) {

            nreturn=-ENOENT;
            goto error;

        }

	/* check for extra pathinfo available */

	nreturn=processextrapathinfo(entry, &pathinfo);
	if ( nreturn!=0 ) goto out;

	nreturn=addtopath(&pathinfo, entry->name);
	if ( nreturn<0 ) goto error;

    }

    nreturn=1;

    out:

    if (nreturn>=0) {

	if ( *(pathinfo.pathstart)!='/' ) {

	    logoutput("determine_path, slash missing at start %s, adding it", pathinfo.pathstart);

	    pathinfo.pathstart--;
    	    *(pathinfo.pathstart)='/';
	    pathinfo.len++;

	}

	if ( pathinfo.mount_entry ) {

	    call_info->mount_entry=pathinfo.mount_entry;

	} else {

	    call_info->mount_entry=get_rootmount();

	}

	/* create a path just big enough */

	call_info->path=malloc(pathinfo.len+1);

	if ( call_info->path ) {

    	    memset(call_info->path, '\0', pathinfo.len+1);
    	    memcpy(call_info->path, pathinfo.pathstart, pathinfo.len);

	    call_info->freepath=1;

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

void init_call_info(struct call_info_struct *call_info, struct notifyfs_entry_struct *entry)
{

    call_info->threadid=pthread_self();
    call_info->entry=entry;
    call_info->entry2remove=entry;
    call_info->effective_watch=NULL;
    call_info->path=NULL;
    call_info->freepath=0;
    call_info->backend=NULL;
    call_info->typebackend=0;
    call_info->next=NULL;
    call_info->prev=NULL;
    call_info->mount_entry=NULL;
    call_info->ctx=NULL;
    call_info->client=NULL;

}

struct call_info_struct *get_call_info(struct notifyfs_entry_struct *entry)
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

    if (call_info->freepath==1) free(call_info->path);
    call_info->freepath=0;

    /* move to unused list */

    res=pthread_mutex_lock(&call_info_unused_mutex);

    if (call_info_unused) call_info_unused->prev=call_info;
    call_info->next=call_info_unused;
    call_info_unused=call_info;
    call_info->prev=NULL;

    res=pthread_mutex_unlock(&call_info_unused_mutex);

}
