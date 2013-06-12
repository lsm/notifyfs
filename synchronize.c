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
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/epoll.h>
#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_WATCHES

#include "logging.h"
#include "workerthreads.h"
#include "epoll-utils.h"

#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"
#include "notifyfs.h"

#include "socket.h"
#include "entry-management.h"
#include "backend.h"
#include "networkservers.h"

#include "path-resolution.h"
#include "options.h"
#include "mountinfo.h"
#include "client.h"
#include "watches.h"
#include "changestate.h"
#include "utils.h"

	if (*createfsevent==1) {
	    struct watch_struct *watch=lookup_watch_inode(parent_inode);

	    /*
		when directory is not watched (= no watch)
		or when no one cares (no view on watch) do not create fsevents
		no one is interested...
	    */

	    if (! watch || directory_is_viewed(watch)==0 || ) *createfsevent=0;

	}





/*

    sync a directory by calling opendir/readdir/closedir on the underlying filesystem
    do a stat for every entry found

    TODO: add action when an entry is created or removed

*/

int sync_directory_full(char *path, struct notifyfs_entry_struct *parent, struct timespec *sync_time, unsigned char createfsevent)
{
    DIR *dp=NULL;
    int nreturn=0;

    logoutput("sync_directory_full");

    dp=opendir(path);

    if ( dp ) {
	int lenpath=strlen(path);
	struct notifyfs_entry_struct *entry, *next_entry;
	struct notifyfs_inode_struct *parent_inode, *inode;
	struct notifyfs_attr_struct *attr;
	struct dirent *de;
	char *name;
	int res, lenname=0;
	struct stat st;
	char tmppath[lenpath+256];
	unsigned char attrcreated=0, entrycreated=0;

	memcpy(tmppath, path, lenpath);
	*(tmppath+lenpath)='/';

	parent_inode=get_inode(parent->inode);

	while((de=readdir(dp))) {

	    name=de->d_name;

	    if ( strcmp(name, ".")==0 || strcmp(name, "..")==0 ) {

		continue;

	    }

	    lenname=strlen(name);

	    /* add this entry to the base path */

	    memcpy(tmppath+lenpath+1, name, lenname);
	    *(tmppath+lenpath+1+lenname)='\0';

	    /* read the stat */

	    res=lstat(tmppath, &st);

	    if ( res==-1 ) {

		/*
		    huh??
		    readdir gives this entry, but stat not??
		*/

		continue;

	    }

	    /*
		find the entry (and create it when not found)
	    */

	    entry=find_entry_raw(parent, parent_inode, name, 1, create_entry);

	    if ( entry ) {

		entrycreated=0;

		if (entry->inode==-1) {

		    /* when no inode: just created */

		    assign_inode(entry);
		    entrycreated=1;

		}

		if (entry->inode>=0) {

		    nreturn++;

		    inode=get_inode(entry->inode);

		    if (inode->attr>=0) {

			attr=get_attr(inode->attr);
			attrcreated=0;

			if (createfsevent==1) {
			    struct fseventmask_struct fseventmask;

			    if (compare_attributes(&&attr->cached_st, &st, &fseventmask)==1) {
				struct notifyfs_fsevent_struct *fsevent=NULL;

				fsevent=create_fsevent(entry);

				if (fsevent) {

				    replace_fseventmask(&fsevent->fseventmask, &fseventmask);

				    /*
					here also create the path (it's known here) ??
				    */

				    queue_fsevent(fsevent);

				}

			    }

			} else {

			    copy_stat(&attr->cached_st, &st);
			    copy_stat_times(&attr->cached_st, &st);

			}

		    } else {

			attr=assign_attr(&st, inode);
			attrcreated=1;

		    }

		    if (attr) {

			attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
			attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

			if (attrcreated==1) {

			    if ( S_ISDIR(st.st_mode)) {

				/* directory no access yet */

				attr->mtim.tv_sec=0;
				attr->mtim.tv_nsec=0;

			    } else {

				attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
				attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

			    }

			}

			attr->atim.tv_sec=sync_time->tv_sec;
			attr->atim.tv_nsec=sync_time->tv_nsec;

		    } else {

			logoutput("sync_directory_full: error: attr not found. ");

			remove_entry(entry);

			/* ignore this (memory) error */

			continue;

		    }

		    if (*createfsevent==1 && entrycreated==1) {
			struct notifyfs_fsevent_struct *fsevent=NULL;

			/* here create an fsevent */

			fsevent=create_fsevent(entry);

			if (fsevent) {

			    fsevent->fseventmask.cache_event=NOTIFYFS_FSEVENT_CACHE_ADDED;
			    queue_fsevent(fsevent);

			}

		    }

		} else {

		    continue;

		}

	    }

	}

	closedir(dp);

    } else {

	nreturn=-errno;
	logoutput("sync_directory: error %i opening directory..", errno);

    }

    return nreturn;

}

unsigned int remove_old_entries(struct notifyfs_entry_struct *parent, struct timespec *sync_time, unsigned char createfsevent)
{
    struct notifyfs_entry_struct *entry, *next_entry;
    struct notifyfs_inode_struct *inode, *parent_inode;
    struct notifyfs_attr_struct *attr;
    unsigned char removeentry=0;
    unsigned int count=0;

    logoutput("remove_old_entries");

    parent_inode=get_inode(parent->inode);

    entry=get_next_entry(parent, NULL);

    while (entry) {

	removeentry=0;
	inode=get_inode(entry->inode);
	attr=get_attr(inode->attr);

	next_entry=get_next_entry(parent, entry);

	if (attr) {

	    /* if stime is less then parent entry access it's gone */

	    if (attr->atim.tv_sec<sync_time->tv_sec ||(attr->atim.tv_sec==sync_time->tv_sec && attr->atim.tv_nsec<sync_time->tv_nsec)) removeentry=1;

	} else {

	    removeentry=1;

	}

	if (removeentry==1) {
	    unsigned char dofsevent=0;

	    if (*createfsevent==0) {
		struct watch_struct *watch=lookup_watch_inode(inode);

		if (watch && directory_is_viewed(watch)==1) dofsevent=1;

	    } else {

		dofsevent=1;

	    }

	    if (dofsevent==1) {
		struct notifyfs_fsevent_struct *fsevent=NULL;

		/* here create an fsevent */

		fsevent=create_fsevent(entry);

		if (fsevent) {

		    /*
			it unknown here how this entry is removed, so use the fsevent_cache_removed
		    */

		    fsevent->fseventmask.cache_event=NOTIFYFS_FSEVENT_CACHE_REMOVED;

		    /* also create here the path?? */

		    queue_fsevent(fsevent);

		}

	    }

	    remove_entry_from_name_hash(entry);
	    remove_entry(entry);

	} else {

	    count++;

	}

	entry=next_entry;

    }

    return count;

}

unsigned int sync_directory_simple(char *path, struct notifyfs_entry_struct *parent, struct timespec *sync_time, unsigned char createfsevent)
{
    struct notifyfs_entry_struct *entry, *next_entry;
    struct notifyfs_inode_struct *inode, *parent_inode;
    struct notifyfs_attr_struct *attr;
    char *name;
    int lenpath=strlen(path), lenname;
    char tmppath[lenpath+256];
    struct stat st;
    unsigned int count=0;

    memcpy(tmppath, path, lenpath);
    *(tmppath+lenpath)='/';

    logoutput("sync_directory_simple");

    parent_inode=get_inode(parent->inode);

    entry=get_next_entry(parent, NULL);

    while (entry) {

	inode=get_inode(entry->inode);
	attr=get_attr(inode->attr);

	name=get_data(entry->name);

	lenname=strlen(name);

	/* add this entry to the base path */

	memcpy(tmppath+lenpath+1, name, lenname);
	*(tmppath+lenpath+1+lenname)='\0';

	/* read the stat */

	if (stat(tmppath, &st)==-1) {

	    /* when entry does not exist, take action.. */

	    if (errno==ENOENT) {

		next_entry=get_next_entry(parent, entry);

		if (createfsevent==1) {
		    struct notifyfs_fsevent_struct *fsevent=NULL;

		    /* here create an fsevent */

		    fsevent=create_fsevent(entry);

		    if (fsevent) {

			/*
			    it unknown here how this entry is removed, so use the fsevent_cache_removed

			    (TODO: add the path here??)

			*/

			fsevent->fseventmask.cache_event=NOTIFYFS_FSEVENT_CACHE_REMOVED;
			queue_fsevent(fsevent);

		    }

		}

		remove_entry_from_name_hash(entry);
		remove_entry(entry);

		entry=next_entry;

	    }

	} else {

	    attr=get_attr(inode->attr);

	    if (!attr) {

		attr=assign_attr(&st, inode);

		if (attr) {

		    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		    attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
		    attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

		    attr->atim.tv_sec=sync_time->tv_sec;
		    attr->atim.tv_nsec=sync_time->tv_nsec;

		}

	    } else {

		if (createfsevent==1) {
		    struct fseventmask_struct fseventmask;

		    if (compare_attributes(&&attr->cached_st, &st, &fseventmask)==1) {
			struct notifyfs_fsevent_struct *fsevent=NULL;

			fsevent=create_fsevent(entry);

			if (fsevent) {

			    replace_fseventmask(&fsevent->fseventmask, &fseventmask);

			    /*
				here also create the path (it's known here) ??
			    */

			    queue_fsevent(fsevent);

			}

		    }

		} else {

		    copy_stat(&attr->cached_st, &st);
		    copy_stat_times(&attr->cached_st, &st);

		}

		attr->atim.tv_sec=sync_time->tv_sec;
		attr->atim.tv_nsec=sync_time->tv_nsec;

	    }

	    count++;

	}

	entry=get_next_entry(parent, entry);

    }

    return count;

}

int sync_directory(char *path, struct notifyfs_inode_struct *inode, struct timespec *current_time, struct stat *st, unsigned char createfsevent)
{
    struct notifyfs_attr_struct *attr;
    int res=0;

    attr=get_attr(inode->attr);
    if (! attr) attr=assign_attr(st, inode);

    if (attr) {

	/*
	    check the directory needs a full sync or a simple one
	    change this: get the directory tree and get the change time...

	*/

	if (attr->mtim.tv_sec<st->st_mtim.tv_sec || (attr->mtim.tv_sec==st->st_mtim.tv_sec && attr->mtim.tv_nsec<st->st_mtim.tv_nsec) ) {
	    struct notifyfs_entry_struct *entry=get_entry(inode->alias);

	    res=sync_directory_full(path, entry, &current_time, 1);

	    if (res>=0) {
		unsigned int count=0;

		count=remove_old_entries(entry, &current_time, 1);
		update_directory_count(watch, count);

	    } else {

		logoutput("process_command_view: error %i sync directory %s", res, command_setwatch->pathinfo.path);

	    }

	} else {
	    unsigned int count=0;

	    count=sync_directory_simple(command_setwatch->pathinfo.path, entry, &current_time, 1);
	    update_directory_count(watch, count);

	}

    } else {

	res=-ENOMEM;

    }

    return res;

}
