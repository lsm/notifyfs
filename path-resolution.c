/*
  2010, 2011, 2012, 2013 Stef Bon <stefbon@gmail.com>

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

#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"

#include "entry-management.h"
#include "access.h"
#include "path-resolution.h"
#include "utils.h"

unsigned char call_info_lock;
pthread_mutex_t call_info_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t call_info_condition;
struct call_info_struct *call_info_list=NULL;

struct call_info_struct *call_info_unused=NULL;
pthread_mutex_t call_info_unused_mutex=PTHREAD_MUTEX_INITIALIZER;

static const char *rootpath="/";

int check_access_path(char *path, pid_t pid, uid_t uid, gid_t gid, struct stat *st, unsigned char mask)
{
    char *name=path, *slash;
    int nreturn=0, res;
    struct gidlist_struct gidlist={0, 0, NULL};

    if (strcmp(path, "/")==0) {

	res=lstat(path, st);

	if (res==-1) {

	    /* something weird... */

	    nreturn=-errno;

	}

	/* no futher permissions checking, asume everybody has access to root */

	goto out;

    }

    while(1) {

        slash=strchr(name, '/');

        if ( slash==name ) {

            name++;

            if (strlen(name)==0) break;

            continue;

        }

        if (slash) {

	    *slash='\0';

    	    if (lstat(path, st)==-1) {

		nreturn=-errno;
		*slash='/';
		break;

	    }

	    *slash='/';

	    res=check_access_process(pid, uid, gid, st, R_OK|X_OK, &gidlist);

	    if (res==0) {

		nreturn=-EACCES;
		break;

	    } else if (res<0) {

		nreturn=res;
		break;

	    }

	} else {

    	    if (lstat(path, st)==-1) {

		nreturn=-errno;

	    } else {

		res=check_access_process(pid, uid, gid, st, mask, &gidlist);

		if (res==0) {

		    nreturn=-EACCES;

		} else if (res<0) {

		    nreturn=res;

		}

	    }

	    break;

	}

        name=slash+1;

    }

    out:

    if (gidlist.list) {

	free(gidlist.list);
	gidlist.list=NULL;

    }

    return nreturn;

}

static int check_pathlen(struct notifyfs_entry_struct *entry)
{
    int len=0;
    char *name;

    while(entry) {

	/* add the lenght of entry->name plus a slash for every name */

	name=get_data(entry->name);

	len+=strlen(name)+1;

	entry=get_entry(entry->parent);

	if (isrootentry(entry)==1) break;

    }

    return len;

}

int determine_path_entry(struct notifyfs_entry_struct *entry, struct pathinfo_struct *pathinfo)
{
    char *pos;
    int nreturn=0, len;
    char *name;

    pathinfo->len=check_pathlen(entry);
    pathinfo->path=malloc(pathinfo->len+1);

    if ( ! pathinfo->path ) {

	nreturn=-ENOMEM;
	goto out;

    }

    pathinfo->flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

    pos=pathinfo->path+pathinfo->len;
    *pos='\0';

    while (entry) {

	name=get_data(entry->name);

	len=strlen(name);
	pos-=len;

	memcpy(pos, name, len);

	pos--;
	*pos='/';

	entry=get_entry(entry->parent);

	if (! entry || isrootentry(entry)==1) break;

    }

    out:

    return nreturn;

}


/*

    construct the path from the entry

*/

int determine_path(struct notifyfs_entry_struct *entry, struct pathinfo_struct *pathinfo)
{
    int nreturn=0;

    if (isrootentry(entry)) {

	pathinfo->path=(char *) rootpath;
	pathinfo->flags=0;
	pathinfo->len=strlen(rootpath);

    } else {
	struct notifyfs_entry_struct *parent=get_entry(entry->parent);

	if (isrootentry(parent)) {
	    char *name=get_data(entry->name);
	    int len=strlen(name);

	    pathinfo->path=malloc(len+2);

	    if (pathinfo->path) {

		*(pathinfo->path)='/';
		memcpy(pathinfo->path+1, name, len);
		*(pathinfo->path+1+len)='\0';

		pathinfo->len=len+1;

	    } else {

		nreturn=-ENOMEM;
		goto out;

	    }

	} else {

	    nreturn=determine_path_entry(entry, pathinfo);

	}

    }

    out:

    return nreturn;

}

void init_call_info(struct call_info_struct *call_info, struct notifyfs_entry_struct *entry)
{
    call_info->threadid=pthread_self();
    call_info->entry=entry;
    call_info->entrycreated=0;
    call_info->strict=0;
    call_info->pathinfo.path=NULL;
    call_info->pathinfo.len=0;
    call_info->pathinfo.flags=0;
    call_info->mount=-1;
    call_info->pid=0;
    call_info->uid=0;
    call_info->gid=0;
    call_info->mask=0;
    call_info->st=NULL;
    call_info->error=0;
}

void free_path_pathinfo(struct pathinfo_struct *pathinfo)
{
    if ((pathinfo->flags & NOTIFYFS_PATHINFOFLAGS_ALLOCATED) && ! (pathinfo->flags & NOTIFYFS_PATHINFOFLAGS_INUSE)) {

	if (pathinfo->path) {

	    free(pathinfo->path);
	    pathinfo->path=NULL;

	}

	pathinfo->flags-=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

    }

}

/* function which creates a path in notifyfs 

    it does this by testing every subpath and create that in notifyfs
    typically used at startup when creating the different mountpoints

*/

struct notifyfs_entry_struct *create_notifyfs_path(struct pathinfo_struct *pathinfo, struct stat *st, unsigned char strict, int mask, int *error, pid_t pid, uid_t uid, gid_t gid)
{
    char *name, *slash, *path;
    struct notifyfs_entry_struct *entry, *parent;
    struct notifyfs_inode_struct *inode;
    struct notifyfs_attr_struct *attr;
    int res;
    struct timespec current_time;
    struct gidlist_struct gidlist={0, 0, NULL};
    unsigned char statdone=0;

    current_time.tv_sec=0;
    current_time.tv_nsec=0;

    parent=get_rootentry();
    entry=parent;

    entry=NULL;

    path=pathinfo->path;
    name=path;

    logoutput("create_notifyfs_path: trying to create %s", path);

    while(1) {

        /*  walk through path from begin to end and 
            check every part */

        slash=strchr(name, '/');

        if (slash==name) {

            name++;
            if (strlen(name)==0) break;
            continue;

        }

        if (slash) *slash='\0';

	inode=get_inode(parent->inode);
        entry=find_entry_by_entry(parent, name);
	statdone=0;

        if (! entry || strict>0) {

	    /* only do a stat call when no entry found or strict mode */

    	    res=lstat(path, st);
	    statdone=1;

	} else {

	    st->st_mode=0;
	    res=0;

	}

	if (res==-1) {

	    /* error: (part of ) path does not exist */

            entry=NULL;
	    if (error) *error=errno;
	    if (slash) *slash='/';
            goto out;

    	}

	if (strict==2) {
	    unsigned char lmask=mask;

	    if (slash) lmask |= (R_OK | X_OK); /* directory: read and access bits must be set */

	    /* check the access to this object */

	    if (check_access_process(pid, uid, gid, st, lmask, &gidlist)==0) {

		if (error) *error=EACCES;
		entry=NULL;
		if (slash) *slash='/';
		goto out;

	    }

	}

	if ( ! entry) {

	    entry=create_entry(parent, name);

	    if (entry) {

		add_to_name_hash_table(entry);

	    } else {

		if (error) *error=ENOMEM;
		if (slash) *slash='/';
		goto out;

	    }

	}

	if (slash) *slash='/';

        if (entry->inode<0) assign_inode(entry);

	inode=get_inode(entry->inode);

	if (inode) {

	    attr=get_attr(inode->attr);

	    if ( ! attr) {

		attr=assign_attr(st, inode);

		if (attr) {

		    attr->ctim.tv_sec=st->st_ctim.tv_sec;
		    attr->ctim.tv_nsec=st->st_ctim.tv_nsec;

		    if (S_ISDIR(attr->cached_st.st_mode)) {

			attr->mtim.tv_sec=0;
			attr->mtim.tv_nsec=0;

		    } else {

			attr->mtim.tv_sec=st->st_mtim.tv_sec;
			attr->mtim.tv_nsec=st->st_mtim.tv_nsec;

		    }

		    if (current_time.tv_sec==0) get_current_time(&current_time);

		    attr->atim.tv_sec=current_time.tv_sec;
		    attr->atim.tv_nsec=current_time.tv_nsec;

		}

	    } else {

		/* update, possibly analyse differences

		*/

		copy_stat(&attr->cached_st, st);
		copy_stat_times(&attr->cached_st, st);

	    }

	} else {

	    if (error) *error=ENOMEM;
	    goto out;

	}

        if ( ! slash ) {

            break;

        } else {

            name=slash+1;
            parent=entry;

        }

    }

    out:

    if (gidlist.list) {

	free(gidlist.list);
	gidlist.list=NULL;

    }

    return entry;

}


