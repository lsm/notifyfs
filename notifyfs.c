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
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>
#include <sys/time.h>
#include <assert.h>
#include <syslog.h>
#include <time.h>

#include <inttypes.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>



#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

#include "notifyfs.h"

#include "utils.h"
#include "workerthreads.h"

#include "notifyfs-io.h"
#include "client-io.h"

#include "entry-management.h"
#include "path-resolution.h"
#include "logging.h"
#include "epoll-utils.h"
#include "message.h"
#include "handlefuseevent.h"
#include "handlemountinfoevent.h"
#include "handlelockinfoevent.h"
#include "handlemessage.h"
#include "options.h"
#include "xattr.h"
#include "socket.h"
#include "client.h"
#include "handleclientmessage.h"
#include "socket.h"
#include "networkutils.h"
#include "access.h"
#include "mountinfo.h"
#include "mountinfo-monitor.h"
#include "lock-monitor.h"
#include "watches.h"
#include "changestate.h"


struct notifyfs_options_struct notifyfs_options;

struct fuse_chan *notifyfs_chan;
extern struct notifyfs_entry_struct *rootentry;
extern struct mount_entry_struct *root_mount;
unsigned char loglevel=0;
int logarea=0;

unsigned char test_access_fsuser(struct call_info_struct *call_info)
{
    unsigned char accessdeny=1;

    /* check access */

    if ( notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT ) {

	/* when accessmode allows root, and user is root, allow */

    	if ( call_info->ctx->uid==0 ) {

    	    accessdeny=0;
    	    goto out;

	}

    }


    if ( notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) {
	struct client_struct *client=lookup_client(call_info->ctx->pid, 0);

	if ( client ) {

	    accessdeny=0;
	    goto out;

	}

    }


    logoutput("access denied for pid %i", (int) call_info->ctx->pid);

    out:

    return accessdeny;

}

static void notifyfs_lookup(fuse_req_t req, fuse_ino_t parentino, const char *name)
{
    struct fuse_entry_param e;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    int nreturn=0;
    unsigned char entryexists=0, dostat=0;
    struct call_info_struct call_info;

    logoutput("LOOKUP, name: %s", name);

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    entry=find_entry_by_ino(parentino, name);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    inode=get_inode(entry->inode);

    call_info.entry=entry;

    entryexists=1;

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;


    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    dostat=1;

    /* if watch attached then not stat , this action will cause a notify action (inotify: IN_ACCESS) */

    if (inode->attr>=0) {

	if ( call_info.mount>=0 ) {
	    struct notifyfs_mount_struct *mount=get_mount(call_info.mount);

    	    if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

		/* mount is down, so do not stat
            	   what with DFS like constructions ?? */

        	dostat=0;

        	/* here something with times and mount times?? */

	    }

        }

    }

    if ( dostat==1 ) {

        nreturn=lstat(call_info.path, &(e.attr));

        if (nreturn==0) {
	    struct notifyfs_attr_struct *attr;

	    if (inode->attr>=0) {

		attr=get_attr(inode->attr);
		copy_stat(&attr->cached_st, &(e.attr));
		copy_stat_times(&attr->cached_st, &(e.attr));

		/* here compare the attr->mtim/ctim with e.attr.mtim/ctim 
		    and if necessary create an fsevent */

		if (attr->ctim.tv_sec<attr->cached_st.st_ctim.tv_sec || (attr->ctim.tv_sec==attr->cached_st.st_ctim.tv_sec && attr->ctim.tv_nsec<attr->cached_st.st_ctim.tv_nsec)) {

		    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		    logoutput("notifyfs_lookup: difference in ctim");

		}

		if (attr->mtim.tv_sec<attr->cached_st.st_mtim.tv_sec || (attr->mtim.tv_sec==attr->cached_st.st_mtim.tv_sec && attr->mtim.tv_nsec<attr->cached_st.st_mtim.tv_nsec)) {

		    logoutput("notifyfs_lookup: difference in mtim");

		}

	    } else {

		attr=assign_attr(&(e.attr), inode);

		attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		if ( S_ISDIR(e.attr.st_mode)) {

		    /* directory no access yet */

		    attr->mtim.tv_sec=0;
		    attr->mtim.tv_nsec=0;

		} else {

		    attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
		    attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

		}

	    }

	    /* other fs values */

	    e.attr.st_dev=0;
	    e.attr.st_blksize=512;
	    e.attr.st_blocks=0;

	} else {

	    logoutput("notifyfs_lookup: stat on %s gives error %i", call_info.path, errno);

	}

    } else {

	/* get the stat from the cache */

        /* no stat: here copy the stat from inode */

	get_stat_from_notifyfs(&(e.attr), entry);

        nreturn=0;

    }

    out:

    if ( nreturn==-ENOENT) {

	logoutput("lookup: entry does not exist (ENOENT)");

	e.ino=0;
	e.entry_timeout=notifyfs_options.negative_timeout;

    } else if ( nreturn<0 ) {

	logoutput("do_lookup: error (%i)", nreturn);

    } else {

	// no error

	inode->nlookup++;
	e.ino = inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

	logoutput("lookup return size: %li", (long) e.attr.st_size);

    }

    logoutput("lookup: return %i", nreturn);

    if ( nreturn<0 ) {

	fuse_reply_err(req, -nreturn);

    } else {

        fuse_reply_entry(req, &e);

    }

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

	/* here change the action to DOWN and leave it to changestate what to do?
	   REMOVE of SLEEP */

        if ( entryexists==1 ) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (call_info.path) {

		    fsevent->path=call_info.path;
		    fsevent->pathallocated=1;
		    call_info.freepath=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    if ( call_info.freepath==1 ) free(call_info.path);

}


static void notifyfs_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
    struct notifyfs_inode_struct *inode;

    inode = find_inode(ino);

    if ( ! inode ) goto out;

    logoutput("FORGET");

    if ( inode->nlookup < nlookup ) {

	logoutput("internal error: forget ino=%llu %llu from %llu", (unsigned long long) ino, (unsigned long long) nlookup, (unsigned long long) inode->nlookup);
	inode->nlookup=0;

	/* here check the attr and entry are removed ... */

    } else {

        inode->nlookup -= nlookup;

    }

    logoutput("forget, current nlookup value %llu", (unsigned long long) inode->nlookup);

    out:

    fuse_reply_none(req);

}

static void notifyfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct stat st;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    int nreturn=0;
    unsigned char entryexists=0, dostat;
    struct call_info_struct call_info;

    logoutput("GETATTR");

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    // get the inode and the entry, they have to exist

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=get_entry(inode->alias);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    entryexists=1;
    call_info.entry=entry;

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if (nreturn<0) goto out;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    dostat=1;

    if (inode->attr>=0) {

	if ( call_info.mount>=0 ) {
	    struct notifyfs_mount_struct *mount=get_mount(call_info.mount);

    	    if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

		/* mount is down, so do not stat
            	   what with DFS like constructions ?? */

        	dostat=0;

        	/* here something with times and mount times?? */

	    }

        }

    }



    if ( dostat==1 ) {

        /* get the stat from the underlying fs */

        nreturn=lstat(call_info.path, &st);

        /* copy the st -> inode->st */

        if ( nreturn==0 ) {
	    struct notifyfs_attr_struct *attr;

	    if (inode->attr>=0) {

		attr=get_attr(inode->attr);
		copy_stat(&attr->cached_st, &st);
		copy_stat_times(&attr->cached_st, &st);

		/* here compare the attr->mtim/ctim with e.attr.mtim/ctim 
		    and if necessary create an fsevent */

		if (attr->ctim.tv_sec<attr->cached_st.st_ctim.tv_sec || (attr->ctim.tv_sec==attr->cached_st.st_ctim.tv_sec && attr->ctim.tv_nsec<attr->cached_st.st_ctim.tv_nsec)) {

		    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		    logoutput("notifyfs_getattr: difference in ctim");

		}

		if (attr->mtim.tv_sec<attr->cached_st.st_mtim.tv_sec || (attr->mtim.tv_sec==attr->cached_st.st_mtim.tv_sec && attr->mtim.tv_nsec<attr->cached_st.st_mtim.tv_nsec)) {

		    logoutput("notifyfs_getattr: difference in mtim");

		}

	    } else {

		attr=assign_attr(&st, inode);

		attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		if ( S_ISDIR(st.st_mode)) {

		    /* directory no access yet */

		    attr->mtim.tv_sec=0;
		    attr->mtim.tv_nsec=0;

		} else {

		    attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
		    attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

		}

	    }

	}

    } else {

        /* copy inode->st to st */

        get_stat_from_notifyfs(&st, entry);
        nreturn=0;

    }

    out:

    logoutput("getattr, return: %i", nreturn);

    if (nreturn < 0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_attr(req, &st, notifyfs_options.attr_timeout);

    }

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (call_info.path) {

		    fsevent->path=call_info.path;
		    fsevent->pathallocated=1;
		    call_info.freepath=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    if ( call_info.freepath==1 ) free(call_info.path);

}

static void notifyfs_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
    struct stat st;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    int nreturn=0,  res;
    unsigned char entryexists=0, dostat;
    struct call_info_struct call_info;

    logoutput("ACCESS, mask: %i", mask);

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    // get the inode and the entry, they have to exist

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=get_entry(inode->alias);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    call_info.entry=entry;
    entryexists=1;

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if (nreturn<0) goto out;

    dostat=1;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if (inode->attr>=0) {

	if ( call_info.mount>=0 ) {
	    struct notifyfs_mount_struct *mount=get_mount(call_info.mount);

    	    if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

		/* mount is down, so do not stat
            	   what with DFS like constructions ?? */

        	dostat=0;

        	/* here something with times and mount times?? */

	    }

        }

    }

    if ( dostat==1 ) {

        /* get the stat from the root fs */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if (res==0) {
	    struct notifyfs_attr_struct *attr;

	    if (inode->attr>=0) {

		attr=get_attr(inode->attr);
		copy_stat(&attr->cached_st, &st);
		copy_stat_times(&attr->cached_st, &st);

		/* here compare the attr->mtim/ctim with e.attr.mtim/ctim 
		    and if necessary create an fsevent */

		if (attr->ctim.tv_sec<attr->cached_st.st_ctim.tv_sec || (attr->ctim.tv_sec==attr->cached_st.st_ctim.tv_sec && attr->ctim.tv_nsec<attr->cached_st.st_ctim.tv_nsec)) {

		    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		    logoutput("notifyfs_access: difference in ctim");

		}

		if (attr->mtim.tv_sec<attr->cached_st.st_mtim.tv_sec || (attr->mtim.tv_sec==attr->cached_st.st_mtim.tv_sec && attr->mtim.tv_nsec<attr->cached_st.st_mtim.tv_nsec)) {

		    logoutput("notifyfs_access: difference in mtim");

		}

	    } else {

		attr=assign_attr(&st, inode);

		attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		if ( S_ISDIR(st.st_mode)) {

		    /* directory no access yet */

		    attr->mtim.tv_sec=0;
		    attr->mtim.tv_nsec=0;

		} else {

		    attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
		    attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

		}

	    }

	}

    } else {

	get_stat_from_notifyfs(&st, entry);
        res=0;

    }

    if ( mask == F_OK ) {

        // check for existence

        if ( res == -1 ) {

            nreturn=-ENOENT;

        } else {

            nreturn=1;

        }

    } else {

        if ( res == -1 ) {

            nreturn=-ENOENT;

        } else {

    	    nreturn=check_access(req, &st, mask);

    	    if ( nreturn==1 ) {

        	nreturn=0; /* grant access */

    	    } else if ( nreturn==0 ) {

        	nreturn=-EACCES; /* access denied */

    	    }

	}

    }

    out:

    logoutput("access, return: %i", nreturn);

    fuse_reply_err(req, abs(nreturn));

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (call_info.path) {

		    fsevent->path=call_info.path;
		    fsevent->pathallocated=1;
		    call_info.freepath=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    if ( call_info.freepath==1 ) free(call_info.path);

}

/* create a directory in notifyfs, only allowed when the directory does exist in the underlying fs */

static void notifyfs_mkdir(fuse_req_t req, fuse_ino_t parentino, const char *name, mode_t mode)
{
    struct fuse_entry_param e;
    struct notifyfs_entry_struct *entry;
    int nreturn=0;
    unsigned char entrycreated=0;
    struct call_info_struct call_info;

    logoutput("MKDIR, name: %s", name);

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    /* check client access */

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    entry=find_entry_by_ino(parentino, name);

    if ( ! entry ) {
        struct notifyfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct notifyfs_entry_struct *pentry;

	    pentry=get_entry(pinode->alias);
	    entry=create_entry(pentry, name);
	    entrycreated=1;

	    if ( !entry ) {

		nreturn=-ENOMEM; /* not able to create due to memory problems */
		entrycreated=0;
		goto error;

	    } 

	} else { 

	    nreturn=-EIO; /* parent inode not found !!?? some strange error */
	    goto error;

	}

    } else {

	/* here an error, the entry does exist already */

	nreturn=-EEXIST;
	goto error;

    }

    call_info.entry=entry;

    /* check r permissions of entry */

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    if ( call_info.mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(call_info.mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

	    nreturn=-EACCES;
	    goto error;

    	    /* here something with times and mount times?? */

	}

    }



    /* only create directory here when it does exist in the underlying fs */

    nreturn=lstat(call_info.path, &(e.attr));

    if ( nreturn==-1 ) {

        /* TODO additional action */

        nreturn=-EACCES; /* does not exist: not permitted */

    } else {

        if ( ! S_ISDIR(e.attr.st_mode) ) {

            /* TODO additional action */

            nreturn=-ENOTDIR; /* not a directory */ 

        } else {

    	    /* check read access : sufficient to create a directory in this fs ... */

    	    nreturn=check_access(req, &(e.attr), R_OK);

    	    if ( nreturn==1 ) {

        	nreturn=0; /* grant access */

    	    } else if ( nreturn==0 ) {

        	nreturn=-EACCES; /* access denied */

    	    }

	}

    }


    out:

    if ( nreturn==0 ) {
	struct notifyfs_inode_struct *inode;
	struct notifyfs_attr_struct *attr;

        assign_inode(entry);

	inode=get_inode(entry->inode);

        if ( ! inode ) {

            nreturn=-ENOMEM;
            goto error;

        }

	attr=assign_attr(&(e.attr), inode);

	attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
	attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

	/* directory no access yet */

	attr->mtim.tv_sec=0;
	attr->mtim.tv_nsec=0;

	inode->nlookup++;

	e.ino = inode->ino;
	e.attr.st_ino = e.ino;
	e.attr.st_dev=0;
	e.attr.st_blksize=512;
	e.attr.st_blocks=0;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

        add_to_name_hash_table(entry);

        logoutput("mkdir successfull");

        fuse_reply_entry(req, &e);
        if ( call_info.path ) free(call_info.path);

        return;

    }


    error:

    logoutput("mkdir: error %i", nreturn);

    if ( entrycreated==1 ) remove_entry(entry);

    e.ino = 0;
    e.entry_timeout = notifyfs_options.negative_timeout;

    fuse_reply_err(req, abs(nreturn));

    if ( call_info.freepath==1 ) free(call_info.path);

}

/* create a node in notifyfs, only allowed when the node does exist in the underlying fs (with the same porperties)*/

static void notifyfs_mknod(fuse_req_t req, fuse_ino_t parentino, const char *name, mode_t mode, dev_t dev)
{
    struct fuse_entry_param e;
    struct notifyfs_entry_struct *entry;
    int nreturn=0;
    unsigned char entrycreated=0;
    struct client_struct *client=NULL;
    struct call_info_struct call_info;

    logoutput("MKNOD, name: %s", name);

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    /* check client access */

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    entry=find_entry_by_ino(parentino, name);

    if ( ! entry ) {
        struct notifyfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct notifyfs_entry_struct *pentry;

	    pentry=get_entry(pinode->alias);
	    entry=create_entry(pentry, name);
	    entrycreated=1;

	    if ( !entry ) {

		nreturn=-ENOMEM; /* not able to create due to memory problems */
		entrycreated=0;
		goto error;

	    } 

	} else { 

	    nreturn=-EIO; /* parent inode not found !!?? some strange error */
	    goto error;

	}

    } else {

	/* here an error, the entry does exist already */

	nreturn=-EEXIST;
	goto error;

    }

    call_info.entry=entry;

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    /* TODO: check when underlying fs is "sleeping": no access allowed */

    if ( call_info.mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(call_info.mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

	    nreturn=-EACCES;
	    goto error;

    	    /* here something with times and mount times?? */

        }

    }


    /* only create something here when it does exist in the underlying fs */

    nreturn=lstat(call_info.path, &(e.attr));

    if ( nreturn==-1 ) {

        nreturn=-ENOENT; /* does not exist */

    } else {

        if ( (e.attr.st_mode & S_IFMT ) != (mode & S_IFMT ) ) {

            nreturn=-ENOENT; /* not the same mode */ 
            goto out;

        }

        /* check read access in underlying fs: sufficient to create something in this fs ... */

        nreturn=check_access(req, &(e.attr), R_OK);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */

        }

    }

    out:

    if ( nreturn==0 ) {
	struct notifyfs_inode_struct *inode;
	struct notifyfs_attr_struct *attr;

        assign_inode(entry);

	inode=get_inode(entry->inode);

        if ( ! inode ) {

            nreturn=-ENOMEM;
            goto error;

        }

	attr=assign_attr(&(e.attr), inode);

	attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
	attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

	attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
	attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

	inode->nlookup++;

	e.ino = inode->ino;
	e.attr.st_ino = e.ino;
	e.attr.st_dev=0;
	e.attr.st_blksize=512;
	e.attr.st_blocks=0;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

        add_to_name_hash_table(entry);

        logoutput("mknod succesfull");

        fuse_reply_entry(req, &e);
        if ( call_info.path ) free(call_info.path);

        return;

    }

    error:

    logoutput("mknod: error %i", nreturn);

    if ( entrycreated==1 ) remove_entry(entry);

    e.ino = 0;
    e.entry_timeout = notifyfs_options.negative_timeout;

    fuse_reply_err(req, abs(nreturn));

    if ( call_info.freepath==1 ) free(call_info.path);

}


static inline struct notifyfs_generic_dirp_struct *get_dirp(struct fuse_file_info *fi)
{
    return (struct notifyfs_generic_dirp_struct *) (uintptr_t) fi->fh;
}

static void free_dirp(struct notifyfs_generic_dirp_struct *dirp)
{

    free(dirp);

}

//
// open a directory to read the contents
// here the backend is an audio cdrom, with only one root directory, and
// no subdirectories
//
// so in practice this will be called only for the root
// it's not required to build an extra check we're in the root, the
// VFS will never allow this function to be called on something else than a directory
//

/* here also add the contents of the underlying fs when there is a watch set */

static void notifyfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct notifyfs_generic_dirp_struct *dirp=NULL;
    int nreturn=0;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    struct call_info_struct call_info;

    logoutput("OPENDIR");

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=get_entry(inode->alias);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    /* register call */

    init_call_info(&call_info, entry);
    call_info.ctx=fuse_req_ctx(req);

    /* check client access */

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    dirp = malloc(sizeof(struct notifyfs_generic_dirp_struct));

    if ( ! dirp ) {

	nreturn=-ENOMEM;
	goto out;

    }

    memset(dirp, 0, sizeof(struct notifyfs_generic_dirp_struct));

    dirp->entry=NULL;
    dirp->upperfs_offset=0;

    dirp->generic_fh.entry=entry;

    // assign this object to fi->fh

    fi->fh = (unsigned long) dirp;


    out:

    if ( nreturn<0 ) {

	if ( dirp ) free_dirp(dirp);

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_open(req, fi);

    }

    logoutput("opendir, nreturn %i", nreturn);

}

static const char *dotname=".";
static const char *dotdotname="..";

static void notifyfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t upperfs_offset, struct fuse_file_info *fi)
{
    char *buff=NULL;
    size_t buffpos=0;
    int res, nreturn=0;
    size_t entsize;
    char *entryname=NULL;
    struct notifyfs_generic_dirp_struct *dirp = get_dirp(fi);
    struct notifyfs_entry_struct *entry=NULL;

    logoutput("READDIR, offset: %"PRId64, upperfs_offset);

    dirp->upperfs_offset=upperfs_offset;

    entry=dirp->entry;

    if ( dirp->upperfs_offset>0 && ! entry ) goto out;

    buff=malloc(size);

    if (! buff) {

	nreturn=-ENOMEM;
	goto out;

    }

    while (buffpos < size ) {

        if ( dirp->upperfs_offset == 0 ) {
	    struct notifyfs_inode_struct *inode=get_inode(dirp->generic_fh.entry->inode);

	    logoutput("notifyfs_readdir: got %s", entryname);

            /* the . entry */

            dirp->st.st_ino = inode->ino;
	    dirp->st.st_mode=S_IFDIR;
	    entryname=(char *) dotname;

        } else if ( dirp->upperfs_offset == 1 ) {
	    struct notifyfs_inode_struct *inode=get_inode(dirp->generic_fh.entry->inode);

            /* the .. entry */

	    logoutput("notifyfs_readdir: got %s", entryname);

	    if (isrootinode(inode) == 1 ) {

	        dirp->st.st_ino = FUSE_ROOT_ID;

	    } else {
		struct notifyfs_entry_struct *parent;

		parent=get_entry(dirp->generic_fh.entry->parent);
		inode=get_inode(parent->inode);

	        dirp->st.st_ino = inode->ino;

	    }

	    entryname=(char *) dotdotname;
	    dirp->st.st_mode=S_IFDIR;

        } else {

	    if (buffpos>0) {

        	if ( dirp->upperfs_offset == 2 ) {

            	    entry=get_next_entry(dirp->generic_fh.entry, NULL);

        	} else {

                    entry=get_next_entry(dirp->generic_fh.entry, entry);

                }

            }

            if ( entry ) {
		struct notifyfs_inode_struct *inode=get_inode(entry->inode);
		struct notifyfs_attr_struct *attr=get_attr(inode->attr);

                entryname=get_data(entry->name);

		logoutput("notifyfs_readdir: got %s", entryname);

                /* valid entry */

                dirp->st.st_ino = inode->ino;
                dirp->st.st_mode = attr->cached_st.st_mode;

            } else {

                /* no valid entry: end of stream */

		dirp->entry=NULL;
                break;

            }

        }

        entsize = fuse_add_direntry(req, buff + buffpos, size - buffpos, entryname, &dirp->st, dirp->upperfs_offset);

	if (buffpos+entsize>size) {

	    if (dirp->upperfs_offset>1) dirp->entry=entry;
	    break;

	}

        dirp->upperfs_offset+=1;
	dirp->st.st_ino=0;
	dirp->st.st_mode=0;
	buffpos+=entsize;
	dirp->entry=NULL;

    }

    out:

    if (nreturn < 0 ) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_buf(req, buff, buffpos);

    }

    if (buff) {

	free(buff);
	buff=NULL;

    }

}




static void notifyfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct notifyfs_generic_dirp_struct *dirp = get_dirp(fi);

    (void) ino;

    logoutput("RELEASEDIR");

    fuse_reply_err(req, 0);

    free_dirp(dirp);
    fi->fh=0;

}

static void notifyfs_statfs(fuse_req_t req, fuse_ino_t ino)
{
    struct statvfs st;
    int nreturn=0, res;
    struct notifyfs_entry_struct *entry; 
    struct notifyfs_inode_struct *inode;
    struct call_info_struct call_info;


    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    logoutput("STATFS");

    /* check client access */

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=get_entry(inode->alias);

    if ( ! entry ){

	nreturn=-ENOENT;
	goto out;

    }

    memset(&st, 0, sizeof(statvfs));

    /* should the statvfs be taken of the path or the root ?? */

    res=statvfs("/", &st);

    if ( res==0 ) {

	// take some values from the default

	/* note the fs does not provide opening/reading/writing of files, so info about blocksize etc
	   is useless, so do not override the default from the root */ 

	st.f_bsize=512;
	st.f_frsize=st.f_bsize; /* no fragmentation on this fs */
	st.f_blocks=0;
	st.f_bfree=0;
	st.f_bavail=0;

	st.f_files=get_inoctr();
	st.f_ffree=UINT32_MAX - st.f_files ; /* inodes are of unsigned long int, 4 bytes:32 */
	st.f_favail=st.f_ffree;

	// do not know what to put here... just some default values... no fsid.... just zero

	st.f_fsid=0;
	st.f_flag=0;
	st.f_namemax=255;

    } else {

	nreturn=-errno;

    }

    out:

    if (nreturn==0) {

	fuse_reply_statfs(req, &st);

    } else {

        fuse_reply_err(req, nreturn);

    }

    logoutput("statfs,nreturn: %i", nreturn);

}

static void notifyfs_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size, int flags)
{
    int nreturn=0, res;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    char *basexattr=NULL;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0, dostat;

    logoutput("SETXATTR");

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    /* check client access */

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    /* find the inode and entry */

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=get_entry(inode->alias);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    call_info.entry=entry;
    entryexists=1;

    /* translate entry to path..... and try to determine the backend */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    dostat=1;

    /* if watch attached then not stat */

    /* here test the inode is watched */

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if (inode->attr>=0) {

	if ( call_info.mount>=0 ) {
	    struct notifyfs_mount_struct *mount=get_mount(call_info.mount);

    	    if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

		/* mount is down, so do not stat
            	   what with DFS like constructions ?? */

        	dostat=0;

        	/* here something with times and mount times?? */

	    }

        }

    }


    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        if (res==0) {
	    struct notifyfs_attr_struct *attr;

	    if (inode->attr>=0) {

		attr=get_attr(inode->attr);
		copy_stat(&attr->cached_st, &st);
		copy_stat_times(&attr->cached_st, &st);

		/* here compare the attr->mtim/ctim with e.attr.mtim/ctim 
		    and if necessary create an fsevent */

		if (attr->ctim.tv_sec<attr->cached_st.st_ctim.tv_sec || (attr->ctim.tv_sec==attr->cached_st.st_ctim.tv_sec && attr->ctim.tv_nsec<attr->cached_st.st_ctim.tv_nsec)) {

		    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		    logoutput("notifyfs_setxattr: difference in ctim");

		}

		if (attr->mtim.tv_sec<attr->cached_st.st_mtim.tv_sec || (attr->mtim.tv_sec==attr->cached_st.st_mtim.tv_sec && attr->mtim.tv_nsec<attr->cached_st.st_mtim.tv_nsec)) {

		    logoutput("notifyfs_setxattr: difference in mtim");

		}

	    } else {

		attr=assign_attr(&st, inode);

		attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		if ( S_ISDIR(st.st_mode)) {

		    /* directory no access yet */

		    attr->mtim.tv_sec=0;
		    attr->mtim.tv_nsec=0;

		} else {

		    attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
		    attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

		}

	    }

	} else {

	    logoutput("notifyfs_setxattr: error %i stat %s", errno, call_info.path);

	    /* here some action */

	}

    } else {

        get_stat_from_notifyfs(&st, entry);

        res=0;

    }

    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

    } else {

        /* check read access */

        nreturn=check_access(req, &st, R_OK);

	logoutput("setxattr: got %i from check_access", nreturn);

        if ( nreturn>=1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */
            goto out;

        }

    }

    // make this global....

    basexattr=malloc(strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3); /* plus _ and terminator */

    if ( ! basexattr ) {

        nreturn=-ENOMEM;
        goto out;

    }

    memset(basexattr, '\0', strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3);

    sprintf(basexattr, "system.%s_", XATTR_SYSTEM_NAME);

    if ( strlen(name) > strlen(basexattr) && strncmp(name, basexattr, strlen(basexattr))==0 ) {

	nreturn=setxattr4workspace(&call_info, name + strlen(basexattr), value);

    } else {

	nreturn=-ENOATTR;

    }

    out:

    if (nreturn<0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_err(req, 0);

    }

    logoutput("setxattr, nreturn %i", nreturn);

    if ( basexattr ) free(basexattr);

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (call_info.path) {

		    fsevent->path=call_info.path;
		    fsevent->pathallocated=1;
		    call_info.freepath=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    if ( call_info.freepath==1 ) free(call_info.path);

}


static void notifyfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
    int nreturn=0, nlen=0, res;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    void *value=NULL;
    struct xattr_workspace_struct *xattr_workspace;
    char *basexattr=NULL;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0, dostat;

    logoutput("GETXATTR, name: %s, size: %i", name, size);

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    /* check client access */

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    /* find inode and entry */

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=get_entry(inode->alias);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    call_info.entry=entry;

    entryexists=1;

    /* translate entry to path..... */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    dostat=1;

    /* if watch attached then not stat */

    /* test the inode is watched */

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */


    if (inode->attr>=0) {

	if ( call_info.mount>=0 ) {
	    struct notifyfs_mount_struct *mount=get_mount(call_info.mount);

    	    if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

		/* mount is down, so do not stat
            	   what with DFS like constructions ?? */

        	dostat=0;

        	/* here something with times and mount times?? */

	    }

        }

    }


    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if ( res==0) {
	    struct notifyfs_attr_struct *attr;

	    if (inode->attr>=0) {

		attr=get_attr(inode->attr);
		copy_stat(&attr->cached_st, &st);
		copy_stat_times(&attr->cached_st, &st);

		/* here compare the attr->mtim/ctim with e.attr.mtim/ctim 
		    and if necessary create an fsevent */

		if (attr->ctim.tv_sec<attr->cached_st.st_ctim.tv_sec || (attr->ctim.tv_sec==attr->cached_st.st_ctim.tv_sec && attr->ctim.tv_nsec<attr->cached_st.st_ctim.tv_nsec)) {

		    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		    logoutput("notifyfs_getxattr: difference in ctim");

		}

		if (attr->mtim.tv_sec<attr->cached_st.st_mtim.tv_sec || (attr->mtim.tv_sec==attr->cached_st.st_mtim.tv_sec && attr->mtim.tv_nsec<attr->cached_st.st_mtim.tv_nsec)) {

		    logoutput("notifyfs_getxattr: difference in mtim");

		}

	    } else {

		attr=assign_attr(&st, inode);

		attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		if ( S_ISDIR(st.st_mode)) {

		    /* directory no access yet */

		    attr->mtim.tv_sec=0;
		    attr->mtim.tv_nsec=0;

		} else {

		    attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
		    attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

		}

	    }

	} else {

	    logoutput("notifyfs_getxattr: error %i stat %s", errno, call_info.path);

	    /* here some action */

	}

    } else {

        get_stat_from_notifyfs(&st, entry);

        res=0;

    }


    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

    } else {

        /* check read access */

        nreturn=check_access(req, &st, R_OK);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */
            goto out;

        }

    }

    basexattr=malloc(strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3); /* plus _ and terminator */

    if ( ! basexattr ) {

        nreturn=-ENOMEM;
        goto out;

    }

    memset(basexattr, '\0', strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3);

    sprintf(basexattr, "system.%s_", XATTR_SYSTEM_NAME);

    if ( strlen(name) > strlen(basexattr) && strncmp(name, basexattr, strlen(basexattr))==0 ) {

	// workspace related xattrs 
	// (they begin with system. and follow the value of XATTR_SYSTEM_NAME and the a _)

	xattr_workspace=malloc(sizeof(struct xattr_workspace_struct));

	if ( ! xattr_workspace ) {

	    nreturn=-ENOMEM;
	    goto out;

	}

	memset(xattr_workspace, 0, sizeof(struct xattr_workspace_struct));

        // here pass only the relevant part? 

	xattr_workspace->name=NULL;
	xattr_workspace->size=size;
	xattr_workspace->nerror=0;
	xattr_workspace->value=NULL;
	xattr_workspace->nlen=0;

	getxattr4workspace(&call_info, name + strlen(basexattr), xattr_workspace);

	if ( xattr_workspace->nerror<0 ) {

	    nreturn=xattr_workspace->nerror;

	    if ( xattr_workspace->value) {

	        free(xattr_workspace->value);
		xattr_workspace->value=NULL;

            }

	} else {

	    nlen=xattr_workspace->nlen;
	    if ( xattr_workspace->value ) value=xattr_workspace->value;

	}

	// free the tmp struct xattr_workspace
	// note this will not free value, which is just a good thing
	// it is used as reply overall

	free(xattr_workspace);

    } else {

	nreturn=-ENOATTR;

    }

    out:

    if ( nreturn < 0 ) { 

	fuse_reply_err(req, -nreturn);

    } else {

	if ( size == 0 ) {

	    // reply with the requested bytes

	    fuse_reply_xattr(req, nlen);

            logoutput("getxattr, fuse_reply_xattr %i", nlen);

	} else if ( nlen > size ) {

	    fuse_reply_err(req, ERANGE);

            logoutput("getxattr, fuse_reply_err ERANGE");

	} else {

	    // reply with the value

	    fuse_reply_buf(req, value, strlen(value));

            logoutput("getxattr, fuse_reply_buf value %s", (char *) value);

	}

    }

    logoutput("getxattr, nreturn: %i, nlen: %i", nreturn, nlen);

    if ( value ) free(value);
    if ( basexattr ) free(basexattr);

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (call_info.path) {

		    fsevent->path=call_info.path;
		    fsevent->pathallocated=0;
		    call_info.freepath=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    if ( call_info.freepath==1 ) free(call_info.path);

}

static void notifyfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    ssize_t nlenlist=0;
    int nreturn=0, res;
    char *list=NULL;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0, dostat;

    logoutput("LISTXATTR, size: %li", (long) size);

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    /* check client access */

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(&call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    /* find inode and entry */

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=get_entry(inode->alias);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    call_info.entry=entry;
    entryexists=1;

    /* translate entry to path..... */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    dostat=1;

    /* if watch attached then not stat */

    /* test the inode s watched */

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if (inode->attr>=0) {

	if ( call_info.mount>=0 ) {
	    struct notifyfs_mount_struct *mount=get_mount(call_info.mount);

    	    if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

		/* mount is down, so do not stat
            	   what with DFS like constructions ?? */

        	dostat=0;

        	/* here something with times and mount times?? */

	    }

        }

    }

    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if (res==0) {
	    struct notifyfs_attr_struct *attr;

	    if (inode->attr>=0) {

		attr=get_attr(inode->attr);
		copy_stat(&attr->cached_st, &st);
		copy_stat_times(&attr->cached_st, &st);

		/* here compare the attr->mtim/ctim with e.attr.mtim/ctim 
		    and if necessary create an fsevent */

		if (attr->ctim.tv_sec<attr->cached_st.st_ctim.tv_sec || (attr->ctim.tv_sec==attr->cached_st.st_ctim.tv_sec && attr->ctim.tv_nsec<attr->cached_st.st_ctim.tv_nsec)) {

		    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		    logoutput("notifyfs_setxattr: difference in ctim");

		}

		if (attr->mtim.tv_sec<attr->cached_st.st_mtim.tv_sec || (attr->mtim.tv_sec==attr->cached_st.st_mtim.tv_sec && attr->mtim.tv_nsec<attr->cached_st.st_mtim.tv_nsec)) {

		    logoutput("notifyfs_setxattr: difference in mtim");

		}

	    } else {

		attr=assign_attr(&st, inode);

		attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		if ( S_ISDIR(st.st_mode)) {

		    /* directory no access yet */

		    attr->mtim.tv_sec=0;
		    attr->mtim.tv_nsec=0;

		} else {

		    attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
		    attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

		}

	    }

	} else {

	    logoutput("notifyfs_listxattr: error %i stat %s", errno, call_info.path);

	    /* here some action */

	}

    } else {

        get_stat_from_notifyfs(&st, entry);

        res=0;

    }

    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

    } else {

        /* check read access */

        nreturn=check_access(req, &st, R_OK);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */
            goto out;

        }

    }

    if ( nreturn==0 ) {

	if ( size>0 ) {

	    // just create a list with the overall size

	    list=malloc(size);

	    if ( ! list ) {

		nreturn=-ENOMEM;
		goto out;

	    } else {

                memset(list, '\0', size);

            }

	}

	nlenlist=listxattr4workspace(&call_info, list, size);

	if ( nlenlist<0 ) {

	    // some error
	    nreturn=nlenlist;
	    goto out;

	}

    }

    out:

    // some checking

    if ( nreturn==0 ) {

        if ( size>0 ) {

            if ( nlenlist > size ) {

                nreturn=-ERANGE;

            }

        }

    }

    if ( nreturn != 0) {

	fuse_reply_err(req, abs(nreturn));

    } else {

	if ( size == 0 ) {

	    // reply with the requested size

	    fuse_reply_xattr(req, nlenlist);

	} else {

	    // here a security check the list exists??

	    fuse_reply_buf(req, list, size);

	    // should the list be freed ???

	}

    }

    // if ( list ) free(list);
    logoutput("listxattr, nreturn: %i, nlenlist: %i", nreturn, nlenlist);

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (call_info.path) {

		    fsevent->path=call_info.path;
		    fsevent->pathallocated=1;
		    call_info.freepath=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    if ( call_info.freepath==1 ) free(call_info.path);

}


void create_pid_file()
{
    char *tmpchar=getenv("RUNDIR");

    if ( tmpchar ) {
        char *buf;

        snprintf(notifyfs_options.pidfile, PATH_MAX, "%s/notifyfs.pid", tmpchar);

        buf=malloc(20);

        if ( buf ) {
            struct stat st;
            int fd=0,res;

	    memset(buf, '\0', 20);
	    sprintf(buf, "%d", getpid()); 

	    logoutput1("storing pid: %s in %s", buf, notifyfs_options.pidfile);

	    res=stat(notifyfs_options.pidfile, &st);

	    if ( S_ISREG(st.st_mode) ) {

	        fd = open(notifyfs_options.pidfile, O_RDWR | O_EXCL | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	    } else {

	        fd = open(notifyfs_options.pidfile, O_RDWR | O_EXCL | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	    }

	    if ( fd>0 ) {

	        res=write(fd, buf, strlen(buf));

	        close(fd);

	    }

	    free(buf);

        }

    } else {

	logoutput1("RUNDIR not found, not creating a pidfile");

    }

}


void remove_pid_file()
{

    if ( strlen(notifyfs_options.pidfile)>0 ) {
        struct stat st;
        int res;

	res=stat(notifyfs_options.pidfile, &st);

	if ( res!=-1 && S_ISREG(st.st_mode) ) {

	    logoutput1("Pid file %s found, removing it.", notifyfs_options.pidfile);

	    res=unlink(notifyfs_options.pidfile);

	}

    }
}



static void notifyfs_init (void *userdata, struct fuse_conn_info *conn)
{

    logoutput("INIT");

    /* create a pid file */

    create_pid_file();

    /* log the capabilities */

    /*

    if ( conn->capable & FUSE_CAP_ASYNC_READ ) {

	logoutput("async read supported");

    } else {

	logoutput("async read not supported");

    }

    if ( conn->capable & FUSE_CAP_POSIX_LOCKS ) {

	logoutput("posix locks supported");

    } else {

	logoutput("posix locks not supported");

    }

    if ( conn->capable & FUSE_CAP_ATOMIC_O_TRUNC ) {

	logoutput("atomic_o_trunc supported");

    } else {

	logoutput("atomic_o_trunc not supported");

    }

    if ( conn->capable & FUSE_CAP_EXPORT_SUPPORT ) {

	logoutput("export support supported");

    } else {

	logoutput("export support not supported");

    }

    if ( conn->capable & FUSE_CAP_BIG_WRITES ) {

	logoutput("big writes supported");

    } else {

	logoutput("big writes not supported");

    }

    if ( conn->capable & FUSE_CAP_SPLICE_WRITE ) {

	logoutput("splice write supported");

    } else {

	logoutput("splice write not supported");

    }

    if ( conn->capable & FUSE_CAP_SPLICE_MOVE ) {

	logoutput("splice move supported");

    } else {

	logoutput("splice move not supported");

    }

    if ( conn->capable & FUSE_CAP_SPLICE_READ ) {

	logoutput("splice read supported");

    } else {

	logoutput("splice read not supported");

    }

    if ( conn->capable & FUSE_CAP_FLOCK_LOCKS ) {

	logoutput("flock locks supported");

    } else {

	logoutput("flock locks not supported");

    }

    if ( conn->capable & FUSE_CAP_IOCTL_DIR ) {

	logoutput("ioctl on directory supported");

    } else {

	logoutput("ioctl on directory not supported");

    } */


}

static void notifyfs_destroy (void *userdata)
{

    remove_pid_file();

}

/* function to update notifyfs when mounts are added and/or removed 
   some observations:
   - not every mountpoint (added or removed) is used by this fs
   - in case of an fs mounted by fs, it's possible that watches are already set on this fs has been
   set to "sleep" mode after is has been umounted before
   -   
   - note::: added and removed mounts are already sorted in the same way the main sort module works 
   in mountinfo.c:
   (mountpoint, mountsource, fstype)
   */

void update_notifyfs(unsigned char firstrun)
{
    struct mount_entry_struct *mount_entry=NULL;
    struct notifyfs_entry_struct *entry;
    struct client_struct *client;
    int nreturn=0, res;

    logoutput("update_notifyfs");

    /* walk through removed mounts to see it affects the fs */

    res=lock_mountlist();

    /* start with last: the list is sorted, bigger/longer (also submounts) mounts are first this way */

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_REMOVED);

    while (mount_entry) {

        entry=(struct notifyfs_entry_struct *) mount_entry->entry;

        /* if normal umount then remove the tree... 
               if umounted by autofs then set tree to sleep */

        if ( entry ) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (! fsevent) {

		logoutput("update_notifyfs: unable to allocate memory for unmount fsevent");

	    } else {

		fsevent->fseventmask.fs_event=NOTIFYFS_FSEVENT_FS_UNMOUNT;

		fsevent->path=mount_entry->mountpoint;
		fsevent->pathallocated=0; /* do not free it! */

		get_current_time(&fsevent->detect_time);

		fsevent->mount_entry=mount_entry;

		queue_fsevent(fsevent);

	    }

        }

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_REMOVED);

    }

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_AUTOFS_KEEP);

    while (mount_entry) {

	/* it's possible this mount entry has been processed already 
	    possible other sollution like the generation id, unique, time...???*/

	if ( mount_entry->processed==1 ) {

	    mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_AUTOFS_KEEP);
	    continue;

	}

        entry=(struct notifyfs_entry_struct *) mount_entry->entry;

        if ( entry ) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (! fsevent) {

		logoutput("update_notifyfs: unable to allocate memory for unmount fsevent (autofs)");

	    } else {

		fsevent->fseventmask.fs_event=NOTIFYFS_FSEVENT_FS_UNMOUNT;

		fsevent->path=mount_entry->mountpoint;
		fsevent->pathallocated=0; /* do not free it! */

		get_current_time(&fsevent->detect_time);

		fsevent->mount_entry=mount_entry;

		queue_fsevent(fsevent);

	    }

        }

	mount_entry->processed=1;

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_AUTOFS_KEEP);

    }

    /* in any case the watches set on an autofs managed fs, get out of "sleep" mode */

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_ADDED);

    while (mount_entry) {

	logoutput("update_notifyfs: found mount entry %s", mount_entry->mountpoint);

	if ( is_rootmount(mount_entry) ) {

	    /* link rootmount and rootentry */

	    if ( ! mount_entry->entry ) {

		/* not already assigned */

		entry=get_rootentry();

		mount_entry->entry=(void *) entry;

	    }

	}

        entry=(struct notifyfs_entry_struct *) mount_entry->entry;

	if ( strcmp(notifyfs_options.mountpoint, mount_entry->mountpoint)==0 ) {

		/* skip the mountpoint of this fs, caused deadlocks in the past... 
		   this will be not the case anymore since the threads for handling the fuse
		   events are up and running independent of this thread... needs testing */

	    goto next;

	} else {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(NULL);

	    if (! fsevent) {

		logoutput("update_notifyfs: unable to allocate memory for mount fsevent");

	    } else {

		fsevent->fseventmask.fs_event=NOTIFYFS_FSEVENT_FS_MOUNT;

		fsevent->path=mount_entry->mountpoint;
		fsevent->pathallocated=0; /* do not free it! */

		get_current_time(&fsevent->detect_time);

		fsevent->mount_entry=mount_entry;

		queue_fsevent(fsevent);

	    }

	}


	next:

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_ADDED);

    }

    unlock:

    res=unlock_mountlist();

}

/* allow or skip filesystems 
    TODO: read the supported filesystems 
*/

unsigned char skip_mount_entry(char *source, char *fs, char *path)
{

    /* skip mounts in certain system directories */

    if (strncmp(path, "/proc/", 6)==0) {

	return 1;

    } else if (strncmp(path, "/sys/", 5)==0) {

	return 1;

    } else if (strncmp(path, "/dev/", 5)==0) {

	return 1;

    }

    /* skip various system filesystems */

    if (strcmp(fs, "devtmpfs")==0) {

	return 1;

    } else if (strcmp(fs, "sysfs")==0) {

	return 1;

    } else if (strcmp(fs, "proc")==0) {

	return 1;

    } else if (strcmp(fs, "cgroup")==0) {

	return 1;

    } else if (strcmp(fs, "cpuset")==0) {

	return 1;

    } else if (strcmp(fs, "binfmt_misc")==0) {

	return 1;

    } else if (strcmp(fs, "sockfs")==0) {

	return 1;

    } else if (strcmp(fs, "pipefs")==0) {

	return 1;

    } else if (strcmp(fs, "anon_inodefs")==0) {

	return 1;

    } else if (strcmp(fs, "rpc_pipefs")==0) {

	return 1;

    } else if (strcmp(fs, "configfs")==0) {

	return 1;

    } else if (strcmp(fs, "devpts")==0) {

	return 1;

    } else if (strcmp(fs, "hugetlbfs")==0) {

	return 1;

    } else if (strcmp(fs, "fusectl")==0) {

	return 1;

    } else if (strcmp(fs, "mqueue")==0) {

	return 1;

    }



    /* do not skip local filesystems */

    if (strcmp(fs, "ext2")==0 || strcmp(fs, "ext3")==0 || strncmp(fs, "ext4", 4)==0) {

	return 0;

    } else if (strcmp(fs, "reiserfs")==0) {

	return 0;

    } else if (strcmp(fs, "xfs")==0) {

	return 0;

    } else if (strcmp(fs, "vfat")==0 || strcmp(fs, "msdos")==0 || strcmp(fs, "ntfs")==0 ) {

	return 0;

    } else if (strcmp(fs, "iso9660")==0) {

	return 0;

    } else if (strcmp(fs, "udf")==0) {

	return 0;

    } else if (strcmp(fs, "btrfs")==0) {

	return 0;

    } else if (strncmp(fs, "fuse.", 5)==0) {

	return 0;

    }

    /* allow autofs */

    if (strcmp(fs, "autofs")==0) {

	return 0;

    }

    /* network filesystems */

    if (strcmp(fs, "cifs")==0) {

	return 0;

    } else if (strcmp(fs, "nfs")==0) {

	return 0;

    } else if (strcmp(fs, "afs")==0) {

	return 0;

    }



    /* all other allow ... */

    return 0;

}

int process_client_event(struct notifyfs_connection_struct *connection, uint32_t events)
{

    if (events & ( EPOLLHUP | EPOLLRDHUP ) ) {

	/* identification who owns the connection (client or server) depends on the 
	    connection being local (->client) or not (->server)
	    this is not enough when working with more type local processes
	    like a local fuse fs
	*/

	if (connection->typedata==NOTIFYFS_OWNERTYPE_CLIENT) {
	    struct client_struct *client;

	    close(connection->fd);
	    connection->fd=0;

	    client=(struct client_struct *) connection->data;

	    if (client) {

		logoutput("process_client_event: hangup of client %i, remove watches", client->pid);

		/* here clear all clients watches */

		client->connection=NULL;

		remove_clientwatches_client(client);
		remove_client(client);

	    }

	} else if (connection->typedata==NOTIFYFS_OWNERTYPE_SERVER) {
	    struct notifyfs_server_struct *notifyfs_server=(struct notifyfs_server_struct *) connection->data;

	    close(connection->fd);
	    connection->fd=0;

	    /* remote server hangup */

	    if (notifyfs_server) {

		logoutput("process_client_event: hangup of remote server");

		notifyfs_server->status=NOTIFYFS_SERVERSTATUS_DOWN;
		notifyfs_server->connection=NULL;

		remove_clientwatches_server(notifyfs_server);

	    }

	}

	free(connection);

    } else if (events & EPOLLIN) {

	int res=receive_message(connection->fd, connection->data, events, is_remote(connection));

    }

    return 0;

}


int add_localclient(struct notifyfs_connection_struct *connection, uint32_t events)
{
    struct ucred credentials;
    socklen_t socklen=sizeof(credentials);
    int nreturn=0;

    if (getsockopt(connection->fd, SOL_SOCKET, SO_PEERCRED, &credentials, &socklen)==0) {
	struct client_struct *client=NULL;

	/* lookup client(s) with the same pid */

	client=lookup_client(credentials.pid, 0);

	if (! client) {

	    client=register_client(connection->fd, credentials.pid, credentials.uid, credentials.gid, NOTIFYFS_CLIENTTYPE_NONE);

	    if (client) {

		connection->data=(void *) client;
		connection->process_event=process_client_event;
		client->connection=connection;

	    } else {

		nreturn=-ENOMEM;

	    }

	} else {

	    nreturn=-EACCES;

	}

    } else {

	nreturn=-errno;

    }

    return nreturn;

}

int add_networkserver(struct notifyfs_connection_struct *connection, uint32_t events)
{
    int nreturn=0;

    /* what to match against the connection ?? */

    if (is_remote(connection)==1) {

	/* here look for a notifyfs server */

	/* check in the existing notifyfs servers */
	/* if it exists: deny the connection */

	/* what to compare ???

	    compare the peername ??

	*/

	struct notifyfs_server_struct *notifyfs_server=compare_notifyfs_servers(connection->fd);

	if (notifyfs_server) {

	    if (notifyfs_server->connection) {

		/* there is already a connection to the same host */

		nreturn=-1;

	    } else {

		/* there is already a reference to this server, but no connection, so use the new one */

		notifyfs_server->connection=connection;
		notifyfs_server->status=NOTIFYFS_SERVERSTATUS_UP;
		get_current_time(&notifyfs_server->connect_time);
		notifyfs_server->error=0;
		connection->data=(void *) notifyfs_server;
		connection->process_event=process_client_event;

	    }

	} else {

	    /* get a new notifyfs server */

	    notifyfs_server=create_notifyfs_server();

	    if (notifyfs_server) {

		init_notifyfs_server(notifyfs_server);

		notifyfs_server->connection=connection;
		notifyfs_server->status=NOTIFYFS_SERVERSTATUS_UP;
		notifyfs_server->type=NOTIFYFS_SERVERTYPE_NETWORK;
		get_current_time(&notifyfs_server->connect_time);
		connection->data=(void *) notifyfs_server;
		connection->process_event=process_client_event;

	    }

	}

    }

    return nreturn;

}

struct fuse_lowlevel_ops notifyfs_oper = {
    .init	= notifyfs_init,
    .destroy	= notifyfs_destroy,
    .lookup	= notifyfs_lookup,
    .forget	= notifyfs_forget,
    .getattr	= notifyfs_getattr,
    .access	= notifyfs_access,
    .mkdir	= notifyfs_mkdir,
    .mknod	= notifyfs_mknod,
    .opendir	= notifyfs_opendir,
    .readdir	= notifyfs_readdir,
    .releasedir	= notifyfs_releasedir,
    .statfs	= notifyfs_statfs,
    .setxattr	= notifyfs_setxattr,
    .getxattr	= notifyfs_getxattr,
    .listxattr	= notifyfs_listxattr,
};

int main(int argc, char *argv[])
{
    int res, epoll_fd, socket_fd, mountinfo_fd, lockmonitor_fd, inet_fd;
    struct stat st;
    struct epoll_extended_data_struct *epoll_xdata=NULL;
    struct epoll_extended_data_struct xdata_mountinfo=EPOLL_XDATA_INIT;
    struct epoll_extended_data_struct xdata_lockinfo=EPOLL_XDATA_INIT;
    struct fuse_args global_fuse_args = FUSE_ARGS_INIT(0, NULL);
    struct workerthreads_queue_struct notifyfs_threads_queue=WORKERTHREADS_QUEUE_INIT;
    struct notifyfs_connection_struct localserver_socket;
    struct notifyfs_connection_struct inetserver_socket;

    umask(0);

    // set logging

    openlog("fuse.notifyfs", 0,0); 

    /* parse commandline options and initialize the fuse options */

    res=parse_arguments(argc, argv, &global_fuse_args);

    if ( res<0 ) {

	res=0;
	goto skipeverything;

    }

    if ( notifyfs_options.conffile ) {

	read_global_settings_from_file(notifyfs_options.conffile);

    }

    if ( ! notifyfs_options.mountpoint ) {

	fprintf(stderr, "Error, mountpoint not defined\n");
	exit(1);

    }

    /* set the maximum number of threads */

    set_max_nr_workerthreads(&notifyfs_threads_queue, 8);
    add_workerthreads(&notifyfs_threads_queue, 8);

    init_changestate(&notifyfs_threads_queue);

    //
    // init the shared memory blocks
    //

    res=initialize_sharedmemory_notifyfs();

    if ( res<0 ) {

	fprintf(stderr, "Error, cannot intialize shared memory (error: %i).\n", abs(res));
	exit(1);

    }

    /*
     create the root inode and entry
    */

    res=create_root();

    if ( res<0 ) {

	fprintf(stderr, "Error, failed to create the root entry(error: %i).\n", res);
	exit(1);

    }

    /*
     set default options
    */

    logoutput("main: taking accessmode %i", notifyfs_options.accessmode);
    logoutput("main: taking testmode %i", notifyfs_options.testmode);

    loglevel=notifyfs_options.logging;
    logarea=notifyfs_options.logarea;

    notifyfs_options.attr_timeout=1.0;
    notifyfs_options.entry_timeout=1.0;
    notifyfs_options.negative_timeout=1.0;

    res = fuse_daemonize(0);

    if ( res!=0 ) {

        logoutput("Error daemonize.");
        goto out;

    }

    /* read fstab */

    read_fstab();

    /* initialize watch tables */

    init_watch_hashtables();

    epoll_fd=init_eventloop(NULL, 1, 0);

    if ( epoll_fd<0 ) {

        logoutput("Error creating epoll fd, error: %i.", epoll_fd);
        goto out;

    } else {

	logoutput("Init mainloop, epoll fd: %i", epoll_fd);

    }

    /*
        create the socket clients can connect to
    */

    init_handleclientmessage(&notifyfs_threads_queue);

    localserver_socket.fd=0;
    init_xdata(&localserver_socket.xdata_socket);
    localserver_socket.data=NULL;
    localserver_socket.type=0;
    localserver_socket.allocated=0;
    localserver_socket.process_event=NULL;

    socket_fd=create_local_serversocket(notifyfs_options.socket, &localserver_socket, NULL, add_localclient);

    if ( socket_fd<=0 ) {

        logoutput("Error creating socket fd: error %i.", socket_fd);
        goto out;

    }

    /*
	if forwarding over network is enabled: listen to remote servers
    */

    init_networkutils();

    if ( notifyfs_options.forwardnetwork==1) {

	if ( ! notifyfs_options.remoteserversfile ) {

	    notifyfs_options.remoteserversfile=strdup(NOTIFYFS_REMOTESERVERS_FILE_DEFAULT);

	}

	if ( notifyfs_options.remoteserversfile ) {

	    read_remote_servers(notifyfs_options.remoteserversfile);

	}

    }

    inetserver_socket.fd=0;
    init_xdata(&inetserver_socket.xdata_socket);
    inetserver_socket.data=NULL;
    inetserver_socket.type=0;
    inetserver_socket.allocated=0;
    inetserver_socket.process_event=NULL;


    if (notifyfs_options.listennetwork==1) {

	logoutput("main: listen on network port requested, but not yet supported");

	inet_fd=create_inet_serversocket(notifyfs_options.networkport, &inetserver_socket, NULL, add_networkserver);

	if ( inet_fd<=0 ) {

    	    logoutput("Error creating socket fd: error %i.", inet_fd);
    	    goto out;

	}

    }

    /*
	add mount monitor
    */

    init_handlemountinfoevent(&notifyfs_threads_queue);

    mountinfo_fd=open(MOUNTINFO_FILE, O_RDONLY);

    if ( mountinfo_fd==-1 ) {

        logoutput("unable to open file %s", MOUNTINFO_FILE);
        goto out;

    }

    epoll_xdata=add_to_epoll(mountinfo_fd, EPOLLERR, process_mountinfo_event, NULL, &xdata_mountinfo, NULL);

    if ( ! epoll_xdata ) {

        logoutput("error adding mountinfo fd to mainloop");
        goto out;

    } else {

        logoutput("mountinfo fd %i added to epoll", mountinfo_fd);

	add_xdata_to_list(epoll_xdata);

    }

    /* assign a callback when something changes */

    register_mountinfo_callback(MOUNTINFO_CB_ONUPDATE, &update_notifyfs);
    register_mountinfo_callback(MOUNTINFO_CB_IGNOREENTRY, &skip_mount_entry);

    /* read the mountinfo to initialize */

    process_mountinfo_event(0, NULL, 0);

    /*
     	add lock monitor
    */

    if (0) {

	init_handlelockinfoevent(&notifyfs_threads_queue);

	lockmonitor_fd=open_locksfile();

	if ( lockmonitor_fd==-1 ) {

    	    logoutput("unable to open locks file");
    	    goto out;

	}

	epoll_xdata=add_to_epoll(lockmonitor_fd, EPOLLERR, process_lockinfo_event, NULL, &xdata_lockinfo, NULL);

	if ( ! epoll_xdata ) {

    	    logoutput("error adding lockinfo fd to mainloop");
    	    goto out;

	} else {

    	    logoutput("lockinfo fd %i added to epoll", lockmonitor_fd);

	    add_xdata_to_list(epoll_xdata);

	}

	/* process to initialize */

	process_lockinfo_event(0, NULL, 0);

    }


    /* fs notify backends */

    initialize_fsnotify_backends();


    /* start fuse */

    res=initialize_fuse(notifyfs_options.mountpoint, "notifyfs", &notifyfs_oper, sizeof(notifyfs_oper), &global_fuse_args, &notifyfs_threads_queue);
    if (res<0) goto out;

    res=start_epoll_eventloop(NULL);

    out:

    finish_fuse();

    close_fsnotify_backends();

    if ( localserver_socket.fd>0 ) {

	res=remove_xdata_from_epoll(&localserver_socket.xdata_socket);
	close(localserver_socket.fd);
	localserver_socket.fd=0;
	remove_xdata_from_list(&localserver_socket.xdata_socket, 0);

    }

    unlink(notifyfs_options.socket);

    if ( xdata_mountinfo.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_mountinfo);
	close(xdata_mountinfo.fd);
	xdata_mountinfo.fd=0;
	remove_xdata_from_list(&xdata_mountinfo, 0);

    }

    /* remove any remaining xdata from mainloop */

    destroy_eventloop(NULL);
    fuse_opt_free_args(&global_fuse_args);

    destroy_workerthreads_queue(&notifyfs_threads_queue);

    skipeverything:

    closelog();

    return res ? 1 : 0;

}
