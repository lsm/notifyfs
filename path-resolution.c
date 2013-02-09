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


#include "logging.h"
#include "notifyfs-io.h"

#include "entry-management.h"
#include "path-resolution.h"
#include "utils.h"

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
	int mount;
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

    /* add a slash or not */

    if ( pathinfo->len>0 && *(path+len-1) != '/' ) {

	pathinfo->len+=len+1;
	addslash=1;

    } else {

	pathinfo->len+=len;

    }

    /* see it fits */

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


    /* second: check it's root */

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
    char *pathstart=NULL, *name;
    int nreturn=0;
    struct notifyfs_entry_struct *entry=call_info->entry;
    pathstring path;
    struct pathinfo_struct pathinfo;

    pathinfo.path=path;
    pathinfo.maxsize=sizeof(pathstring);
    pathinfo.len=0;
    pathinfo.pathstart = pathinfo.path + pathinfo.maxsize - 1;
    *(pathinfo.pathstart) = '\0';
    pathinfo.mount=-1;

    if ( isrootentry(entry) ) {

        pathinfo.pathstart--;
        *(pathinfo.pathstart)='/';
	pathinfo.len++;
	nreturn=1;
	goto out;

    }

    if (entry->mount>0) pathinfo.mount=entry->mount;

    name=get_data(entry->name);

    nreturn=addtopath(&pathinfo, name);

    while (entry->parent>=0) {

	entry=get_entry(entry->parent);

	if ( isrootentry(entry) ) {

    	    pathinfo.pathstart--;
    	    *(pathinfo.pathstart)='/';
	    pathinfo.len++;
	    break;

	}

	if (entry->mount>0 && pathinfo.mount==-1) pathinfo.mount=entry->mount;

	name=get_data(entry->name);
	nreturn=addtopath(&pathinfo, name);
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

	call_info->mount=pathinfo.mount;

	/* create a path just big enough */

	call_info->path=malloc(pathinfo.len+1);

	if ( call_info->path ) {

    	    memset(call_info->path, '\0', pathinfo.len+1);
    	    memcpy(call_info->path, pathinfo.pathstart, pathinfo.len);

	    call_info->freepath=1;

	    if (call_info->mount>=0) {
		struct notifyfs_mount_struct *mount=get_mount(call_info->mount);

    		logoutput("determine_path: at mount %s, result after memcpy: %s", mount->filesystem, call_info->path);

	    } else {

    		logoutput("determine_path: at rootmount, result after memcpy: %s", call_info->path);

	    }


	} else {

    	    nreturn=-ENOMEM;

	}

    }

    error:

    if ( nreturn<0 ) logoutput1("determine_path, error: %i", nreturn);

    return nreturn;

}

void init_call_info(struct call_info_struct *call_info, struct notifyfs_entry_struct *entry)
{

    call_info->threadid=pthread_self();
    call_info->entry=entry;
    call_info->watch=NULL;
    call_info->path=NULL;
    call_info->freepath=0;
    call_info->mount=-1;
    call_info->ctx=NULL;

}


/* function which creates a path in notifyfs 

    it does this by testing every subpath and create that in notifyfs
    typically used at startup when creating the different mountpoints

*/

void create_notifyfs_path(struct call_info_struct *call_info, struct stat *buff_st)
{
    char *path, *slash;
    struct notifyfs_entry_struct *entry, *parent;
    struct notifyfs_inode_struct *inode;
    struct notifyfs_attr_struct *attr;
    int res;
    char tmppath[strlen(call_info->path)+1];
    struct stat st;
    struct timespec current_time;
    unsigned char entrycreated=0;

    get_current_time(&current_time);

    parent=get_rootentry();
    entry=parent;

    call_info->entry=NULL;
    call_info->watch=NULL;

    call_info->mount=parent->mount;

    call_info->ctx=NULL;

    strcpy(tmppath, call_info->path);
    path=tmppath;

    logoutput("create_notifyfs_path: creating %s", tmppath);

    /*  translate path into entry 
        suppose here safe that entry is a subdir of root entry...*/

    while(1) {

        /*  walk through path from begin to end and 
            check every part */

        slash=strchr(path, '/');

        if ( slash==path ) {

            /* ignore the starting slash*/

            path++;

            /* if nothing more (==only a slash) stop here */

            if (strlen(path)==0) break;

            continue;

        }

        if ( slash ) *slash='\0';

	entrycreated=0;

	inode=get_inode(parent->inode);

        entry=find_entry_by_entry(parent, path);

        if ( ! entry ) {

    	    /* check the stat */

    	    if (lstat(tmppath, &st)==-1) {

		logoutput("create_notifyfs_path: error %i at %s", errno, tmppath);

		/* additional action.. */

            	/* what to do here?? the path does not exist */

            	entry=NULL;
            	break;

    	    }

	    entry=create_entry(parent, path);

            if (entry) {

                assign_inode(entry);

		inode=get_inode(entry->inode);

		if (inode) {

            	    add_to_name_hash_table(entry);

		    attr=assign_attr(&st, inode);

		    if (attr) {

			attr->ctim.tv_sec=st.st_ctim.tv_sec;
			attr->ctim.tv_nsec=st.st_ctim.tv_nsec;

			if (S_ISDIR(attr->cached_st.st_mode)) {

			    attr->mtim.tv_sec=0;
			    attr->mtim.tv_nsec=0;

			} else {

			    attr->mtim.tv_sec=st.st_mtim.tv_sec;
			    attr->mtim.tv_nsec=st.st_mtim.tv_nsec;

			}

			attr->atim.tv_sec=current_time.tv_sec;
			attr->atim.tv_nsec=current_time.tv_nsec;

		    }

		    entrycreated=1;

		} else {

		    remove_entry(entry);
		    entry=NULL;
		    break;

		}

            } else {

                break;

            }

        }

	if (entry->mount>=0) {

	    logoutput("create_notifyfs_path: got mountindex %i on %s", entry->mount, tmppath);

	    call_info->mount=entry->mount;

	}

        if ( ! slash ) {

            break;

        } else {

            /* make slash a slash again (was turned into a \0) */

            *slash='/';
            path=slash+1;
            parent=entry;

        }

    }

    if (entry) {

	if (buff_st) {

	    if (entrycreated==0) {

		if (lstat(tmppath, &st)==-1) {

		    if (errno==ENOENT) {

			logoutput("create_notifyfs_path: error %i at %s", errno, tmppath);

			/* additional action.. */

		    }

            	    /* what to do here?? the path does not exist */

            	    entry=NULL;
		    goto out;

		}

	    }

	    copy_stat(buff_st, &st);
	    copy_stat_times(buff_st, &st);

	}

    }

    out:

    call_info->entry=entry;

}
