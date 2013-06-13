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

#include "epoll-utils.h"

#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"

#include "socket.h"

#include "entry-management.h"
#include "path-resolution.h"
#include "logging.h"

#include "networkutils.h"
#include "backend.h"
#include "networkservers.h"

#include "message-base.h"
#include "message-receive.h"
#include "message-send.h"

#include "handlefuseevent.h"
#include "handlemountinfoevent.h"
#include "handlelockinfoevent.h"
#include "handleclientmessage.h"

#include "options.h"
#include "xattr.h"
#include "client.h"
#include "access.h"
#include "mountinfo.h"
#include "mountinfo-monitor.h"
#include "filesystem.h"
#include "watches.h"
#include "changestate.h"

struct notifyfs_options_struct notifyfs_options;

struct fuse_chan *notifyfs_chan;
extern struct notifyfs_entry_struct *rootentry;
extern struct mountentry_struct *rootmount;
unsigned char loglevel=0;
int logarea=0;

static unsigned long long last_generation_id=0;



static void notifyfs_lookup(fuse_req_t req, fuse_ino_t parentino, const char *name)
{
    struct fuse_entry_param e;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    int nreturn=0;
    unsigned char dostat=0;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};
    struct stat st;

    logoutput("LOOKUP, name: %s", name);

    entry=find_entry_by_ino(parentino, name);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    inode=get_inode(entry->inode);

    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;


    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    dostat=1;

    if ( entry->mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

    	    dostat=0;

	    /* here something with times and mount times?? */

        }

    }

    if ( dostat==1 ) {

        nreturn=lstat(pathinfo.path, &(e.attr));

        if (nreturn==0) {
	    struct notifyfs_attr_struct *attr;

	    if (inode->attr>=0) {

		attr=get_attr(inode->attr);

		cache_stat_notifyfs(attr, &(e.attr));


	    } else {

		attr=assign_attr(&(e.attr), inode);

		if (attr) {

		    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
		    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

		]

	    }

	    /* other fs values */

	    e.attr.st_dev=0;
	    e.attr.st_blksize=512;
	    e.attr.st_blocks=0;

	} else {

	    logoutput("notifyfs_lookup: stat on %s gives error %i", pathinfo.path, errno);

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

    if ( nreturn==-ENOENT && pathinfo.path) {

        /* entry in this fs exists but underlying entry not anymore */

	/* here change the action to DOWN and leave it to changestate what to do?
	   REMOVE of SLEEP */

        if (entry) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (pathinfo.path) {

		    /* take over the path */

		    fsevent->pathinfo.path=pathinfo.path;
		    fsevent->pathinfo.flags=pathinfo.flags;
		    fsevent->pathinfo.len=pathinfo.len;
		    pathinfo.flags=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    free_path_pathinfo(&pathinfo);

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
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    int nreturn=0;
    unsigned char dostat;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};
    struct stat st;

    logoutput("GETATTR");

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

    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;


    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    dostat=1;

    if ( entry->mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
             what with DFS like constructions ?? */

    	    dostat=0;


        }

    }



    if ( dostat==1 ) {

        /* get the stat from the underlying fs */

        nreturn=lstat(pathinfo.path, &st);

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

    if ( nreturn==-ENOENT && pathinfo.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if (entry) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (pathinfo.path) {

		    fsevent->pathinfo.path=pathinfo.path;
		    fsevent->pathinfo.flags=pathinfo.flags;
		    fsevent->pathinfo.len=pathinfo.len;
		    pathinfo.flags=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    free_path_pathinfo(&pathinfo);

}

static void notifyfs_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    int nreturn=0,  res;
    unsigned char dostat;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};
    struct stat st;

    logoutput("ACCESS, mask: %i", mask);

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

    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;


    dostat=1;

    if (entry->mount>=0) {
	struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

    	    dostat=0;

    	    /* here something with times and mount times?? */


        }

    }

    if ( dostat==1 ) {

        /* get the stat from the root fs */

        res=lstat(pathinfo.path, &st);

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
	    struct gidlist_struct gidlist={0, 0, NULL};

	    res=check_access_process(ctx->pid, ctx->uid, ctx->gid, &st, mask, &gidlist);

    	    if ( res==1 ) {

        	nreturn=0; /* grant access */

    	    } else if ( res==0 ) {

        	nreturn=-EACCES; /* access denied */

    	    } else if (res<0) {

		nreturn=res;

	    }

	    if (gidlist.list) {

		free(gidlist.list);
		gidlist.list=NULL;

	    }

	}

    }

    out:

    logoutput("access, return: %i", nreturn);

    fuse_reply_err(req, abs(nreturn));

    /* post reply action */

    if ( nreturn==-ENOENT && pathinfo.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        if (entry) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (pathinfo.path) {

		    fsevent->pathinfo.path=pathinfo.path;
		    fsevent->pathinfo.flags=pathinfo.flags;
		    fsevent->pathinfo.len=pathinfo.len;
		    pathinfo.flags=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    free_path_pathinfo(&pathinfo);

}

/* create a directory in notifyfs, only allowed when the directory does exist in the underlying fs */

static void notifyfs_mkdir(fuse_req_t req, fuse_ino_t parentino, const char *name, mode_t mode)
{
    struct fuse_entry_param e;
    struct notifyfs_entry_struct *entry;
    int nreturn=0;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};
    struct stat st;

    logoutput("MKDIR, name: %s", name);

    entry=find_entry_by_ino(parentino, name);

    if ( ! entry ) {
        struct notifyfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct notifyfs_entry_struct *pentry;

	    pentry=get_entry(pinode->alias);
	    entry=create_entry(pentry, name);

	    if ( !entry ) {

		nreturn=-ENOMEM; /* not able to create due to memory problems */
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

    /* check r permissions of entry */

    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;

    if (entry->mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

	    nreturn=-EACCES;
	    goto error;

    	    /* here something with times and mount times?? */

	}

    }

    /* only create directory here when it does exist in the underlying fs */

    nreturn=lstat(pathinfo.path, &(e.attr));

    if ( nreturn==-1 ) {

        /* TODO additional action */

        nreturn=-EACCES; /* does not exist: not permitted */

    } else {

        if ( ! S_ISDIR(e.attr.st_mode) ) {

            /* TODO additional action */

            nreturn=-ENOTDIR; /* not a directory */ 

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

        fuse_reply_entry(req, &e);

	free_path_pathinfo(&pathinfo);

        return;

    }


    error:

    logoutput("MKDIR: error %i", nreturn);

    if (entry) remove_entry(entry);

    e.ino = 0;
    e.entry_timeout = notifyfs_options.negative_timeout;

    fuse_reply_err(req, abs(nreturn));

    free_path_pathinfo(&pathinfo);

}

/* create a node in notifyfs, only allowed when the node does exist in the underlying fs (with the same porperties)*/

static void notifyfs_mknod(fuse_req_t req, fuse_ino_t parentino, const char *name, mode_t mode, dev_t dev)
{
    struct fuse_entry_param e;
    struct notifyfs_entry_struct *entry;
    int nreturn=0;
    struct client_struct *client=NULL;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};
    struct stat st;

    logoutput("MKNOD, name: %s", name);

    entry=find_entry_by_ino(parentino, name);

    if ( ! entry ) {
        struct notifyfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct notifyfs_entry_struct *pentry;

	    pentry=get_entry(pinode->alias);
	    entry=create_entry(pentry, name);

	    if ( !entry ) {

		nreturn=-ENOMEM;
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


    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;

    /* TODO: check when underlying fs is "sleeping": no access allowed */

    if ( entry->mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

	    nreturn=-EACCES;
	    goto error;

    	    /* here something with times and mount times?? */

        }

    }


    /* only create something here when it does exist in the underlying fs */

    nreturn=lstat(pathinfo.path, &(e.attr));

    if ( nreturn==-1 ) {

        nreturn=-ENOENT; /* does not exist */

    } else {

        if ( (e.attr.st_mode & S_IFMT ) != (mode & S_IFMT ) ) {

            nreturn=-ENOENT; /* not the same mode */ 
            goto out;

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

        fuse_reply_entry(req, &e);
        free_path_pathinfo(&pathinfo);

        return;

    }

    error:

    logoutput("MKNOD: error %i", nreturn);

    if (entry) remove_entry(entry);

    e.ino = 0;
    e.entry_timeout = notifyfs_options.negative_timeout;

    fuse_reply_err(req, abs(nreturn));

    free_path_pathinfo(&pathinfo);

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
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};
    struct stat st;

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

    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;

    dirp = malloc(sizeof(struct notifyfs_generic_dirp_struct));

    if ( ! dirp ) {

	nreturn=-ENOMEM;
	goto out;

    }

    memset(dirp, 0, sizeof(struct notifyfs_generic_dirp_struct));

    dirp->entry=NULL;
    dirp->upperfs_offset=0;
    dirp->generic_fh.entry=entry;

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

            /* the . entry */

            dirp->st.st_ino = inode->ino;
	    dirp->st.st_mode=S_IFDIR;
	    entryname=(char *) dotname;

	    logoutput("notifyfs_readdir: got %s", entryname);

        } else if ( dirp->upperfs_offset == 1 ) {
	    struct notifyfs_inode_struct *inode=get_inode(dirp->generic_fh.entry->inode);

            /* the .. entry */

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

	    logoutput("notifyfs_readdir: got %s", entryname);

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
    const struct fuse_ctx *ctx=fuse_req_ctx(req);

    logoutput("STATFS");

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
    unsigned dostat;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};

    logoutput("SETXATTR");

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

    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;

    dostat=1;

    /* if watch attached then not stat */

    /* here test the inode is watched */

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( entry->mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

    	    dostat=0;

    	    /* here something with times and mount times?? */

	}

    }


    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(pathinfo.path, &st);

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

	    logoutput("notifyfs_setxattr: error %i stat %s", errno, pathinfo.path);

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
	struct gidlist_struct gidlist={0, 0, NULL};

    	/* check read access : sufficient to create a directory in this fs ... 
	    note this is an overlays fs, the entry does exist in the underlying fs, so the question here
	    is has the client has enough permissions to read the entry
	*/

	res=check_access_process(ctx->pid, ctx->uid, ctx->gid, &st, R_OK|W_OK, &gidlist);

    	if ( res==1 ) {

    	    nreturn=0; /* grant access */

    	} else if ( res==0 ) {

    	    nreturn=-EACCES; /* access denied */

    	} else if (res<0) {

	    nreturn=res;

	}

	if (gidlist.list) {

	    free(gidlist.list);
	    gidlist.list=NULL;

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

	nreturn=setxattr4workspace(entry, name + strlen(basexattr), value);

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

    if ( nreturn==-ENOENT && pathinfo.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if (entry) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (pathinfo.path) {

		    fsevent->pathinfo.path=pathinfo.path;
		    fsevent->pathinfo.flags=pathinfo.flags;
		    fsevent->pathinfo.len=pathinfo.len;
		    pathinfo.flags=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    free_path_pathinfo(&pathinfo);

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
    unsigned char dostat;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};

    logoutput("GETXATTR, name: %s, size: %i", name, size);

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

    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;

    dostat=1;

    /* if watch attached then not stat */

    /* test the inode is watched */

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( entry->mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

    	    dostat=0;

    	    /* here something with times and mount times?? */

	}

    }


    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(pathinfo.path, &st);

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

	    logoutput("notifyfs_getxattr: error %i stat %s", errno, pathinfo.path);

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
	struct gidlist_struct gidlist={0, 0, NULL};

    	/* check read access : sufficient to create a directory in this fs ... 
	    note this is an overlays fs, the entry does exist in the underlying fs, so the question here
	    is has the client has enough permissions to read the entry
	*/

	res=check_access_process(ctx->pid, ctx->uid, ctx->gid, &st, R_OK, &gidlist);

    	if ( res==1 ) {

    	    nreturn=0; /* grant access */

    	} else if ( res==0 ) {

    	    nreturn=-EACCES; /* access denied */

    	} else if (res<0) {

	    nreturn=res;

	}

	if (gidlist.list) {

	    free(gidlist.list);
	    gidlist.list=NULL;

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

	getxattr4workspace(entry, name + strlen(basexattr), xattr_workspace);

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

    if ( nreturn==-ENOENT && pathinfo.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        if (entry) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (pathinfo.path) {

		    fsevent->pathinfo.path=pathinfo.path;
		    fsevent->pathinfo.flags=pathinfo.flags;
		    fsevent->pathinfo.len=pathinfo.len;
		    pathinfo.flags=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    free_path_pathinfo(&pathinfo);

}

static void notifyfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    ssize_t nlenlist=0;
    int nreturn=0, res;
    char *list=NULL;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    struct stat st;
    unsigned char dostat;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct pathinfo_struct pathinfo={NULL, 0, 0};

    logoutput("LISTXATTR, size: %li", (long) size);

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

    /* translate entry into path */

    nreturn=determine_path(entry, &pathinfo);
    if (nreturn<0) goto out;

    /* test permissions */

    nreturn=check_access_path(pathinfo.path, ctx->pid, ctx->uid, ctx->gid, &st, R_OK);
    if (nreturn<0) goto out;

    dostat=1;

    /* if watch attached then not stat */

    /* test the inode s watched */

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if (entry->mount>=0 ) {
	struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    	if ( mount->status!=NOTIFYFS_MOUNTSTATUS_UP ) {

	    /* mount is down, so do not stat
            what with DFS like constructions ?? */

    	    dostat=0;

    	    /* here something with times and mount times?? */

	}

    }

    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(pathinfo.path, &st);

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

	    logoutput("notifyfs_listxattr: error %i stat %s", errno, pathinfo.path);

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
	struct gidlist_struct gidlist={0, 0, NULL};

    	/* check read access : sufficient to create a directory in this fs ... 
	    note this is an overlays fs, the entry does exist in the underlying fs, so the question here
	    is has the client has enough permissions to read the entry
	*/

	res=check_access_process(ctx->pid, ctx->uid, ctx->gid, &st, R_OK, &gidlist);

    	if ( res==1 ) {

    	    nreturn=0; /* grant access */

    	} else if ( res==0 ) {

    	    nreturn=-EACCES; /* access denied */

    	} else if (res<0) {

	    nreturn=res;

	}

	if (gidlist.list) {

	    free(gidlist.list);
	    gidlist.list=NULL;

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

	nlenlist=listxattr4workspace(entry, list, size);

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

	    fuse_reply_xattr(req, nlenlist);

	} else {

	    fuse_reply_buf(req, list, size);

	}

    }

    logoutput("listxattr, nreturn: %i, nlenlist: %i", nreturn, nlenlist);

    /* post reply action */

    if ( nreturn==-ENOENT && pathinfo.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if (entry) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->fseventmask.move_event=NOTIFYFS_FSEVENT_MOVE_DELETED;

		if (pathinfo.path) {

		    fsevent->pathinfo.path=pathinfo.path;
		    fsevent->pathinfo.flags=pathinfo.flags;
		    fsevent->pathinfo.len=pathinfo.len;
		    pathinfo.flags=0;

		}

		get_current_time(&fsevent->detect_time);
		queue_fsevent(fsevent);

	    }

	}

    }

    free_path_pathinfo(&pathinfo);

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

int update_notifyfs(unsigned long long generation_id, struct mountentry_struct *(*next) (void **index, unsigned char type))
{
    struct mountentry_struct *mountentry=NULL;
    struct notifyfs_entry_struct *entry;
    int nreturn=0, res;
    void *index=NULL;
    struct notifyfs_fsevent_struct *fsevent=NULL;
    struct supermount_struct *supermount=NULL;

    logoutput("update_notifyfs");

    index=NULL;
    mountentry=next(&index, MOUNTLIST_REMOVED);

    while (mountentry) {

	supermount=find_supermount_majorminor(mountentry->major, mountentry->minor);

	if (supermount) {

	    fsevent=create_fsevent(entry);

	    if (! fsevent) {

		logoutput("update_notifyfs: unable to allocate memory for unmount fsevent");

	    } else {

		fsevent->fseventmask.fs_event=NOTIFYFS_FSEVENT_FS_UNMOUNT;

		fsevent->pathinfo.path=strdup(mountentry->mountpoint);
		fsevent->pathinfo.flags=0;

		if (fsevent->pathinfo.path) {

		    fsevent->pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;
		    fsevent->pathinfo.len=strlen(mountentry->mountpoint);

		} else {

		    fsevent->pathinfo.flags=0;
		    fsevent->pathinfo.len=0;

		}

		get_current_time(&fsevent->detect_time);

		fsevent->data=(void *) supermount;
		fsevent->flags=mountentry->flags;

		queue_fsevent(fsevent);

	    }

	} else {

	    logoutput("update_notifyfs: unable to find the supermount %i:%i", mountentry->major, mountentry->minor);

	}

        mountentry=next(&index, MOUNTLIST_REMOVED);

    }


    index=NULL;
    mountentry=next(&index, MOUNTLIST_ADDED);

    while (mountentry) {

	logoutput("update_notifyfs: found mount entry, unique %lli, generation %lli, path %s", mountentry->unique, mountentry->generation, mountentry->mountpoint);

	supermount=find_supermount_majorminor(mountentry->major, mountentry->minor);
	if (! supermount) supermount=add_supermount(mountentry->major, mountentry->minor, mountentry->source, mountentry->options);

	if (supermount) {
	    struct notifyfs_fsevent_struct *fsevent=create_fsevent(NULL);

	    supermount->fs=lookup_filesystem(mountentry->fs);

	    if (! fsevent) {

		logoutput("update_notifyfs: unable to allocate memory for mount fsevent");

	    } else {

		fsevent->fseventmask.fs_event=NOTIFYFS_FSEVENT_FS_MOUNT;

		fsevent->pathinfo.path=strdup(mountentry->mountpoint);

		if (fsevent->pathinfo.path) {

		    fsevent->pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;
		    fsevent->pathinfo.len=strlen(mountentry->mountpoint);

		} else {

		    fsevent->pathinfo.len=0;
		    fsevent->pathinfo.flags=0;

		}

		get_current_time(&fsevent->detect_time);

		fsevent->data=(void *) supermount;
		fsevent->flags=mountentry->flags;

		queue_fsevent(fsevent);

	    }

	}

	next:

	mountentry=next(&index, MOUNTLIST_ADDED);

    }

    last_generation_id=generation_id;

    return 1;

}

/* allow or skip filesystems 
    TODO: read the supported filesystems 
*/

unsigned char skip_mountentry(char *source, char *fs, char *path)
{
    struct notifyfs_filesystem_struct *filesystem=NULL;
    int error=0;
    unsigned char doskip=0;

    /* skip the mountpoint of notifyfs self */

    if (strcmp(path, notifyfs_options.mountpoint)==0) {

	doskip=1;
	goto out;

    }

    /*
	do not skip auto mounted mountpoints
    */

    if (strcmp(fs, "autofs")==0) goto out;

    /*
	determine the filesystem, to find out it's a system fs or not
    */

    filesystem=lookup_filesystem(fs);

    if (! filesystem) {

	filesystem=read_supported_filesystems(fs, &error);

    }

    if (filesystem) {

	if (filesystem->mode & (NOTIFYFS_FILESYSTEM_KERNEL | NOTIFYFS_FILESYSTEM_NODEV)) {

	    /* test it's a system filesystem or not */

	    if (! (filesystem->mode & NOTIFYFS_FILESYSTEM_STAT)) {
		struct statvfs stvfs;

		if (statvfs(path, &stvfs)==0) {

		    if (stvfs.f_bfree==0) filesystem->mode|=NOTIFYFS_FILESYSTEM_SYSTEM;

		}

		filesystem->mode|=NOTIFYFS_FILESYSTEM_STAT;

	    }

	}

	/* skip system filesystems like proc, mqueue, sysfs, cgroup etc */

	if (filesystem->mode&NOTIFYFS_FILESYSTEM_SYSTEM) {

	    doskip=1;
	    goto out;

	}

    } else if (error>0) {

	if (error==ENOMEM) {

	    logoutput("skip_mountentry: memory allocation error");

	} else {

	    logoutput("skip_mountentry: unknown error (%i)", error);

	}

    } else {

	/*
	    not a kernel fs
	    (with linux: a fuse fs)

	*/

	filesystem=create_filesystem();

	if (filesystem) {

	    strncpy(filesystem->filesystem, fs, 32);
	    add_filesystem(filesystem, 0);

	}

    }

    out:

    return doskip;

}

int process_connection_event(struct notifyfs_connection_struct *connection, uint32_t events)
{
    logoutput("process_connection_event");

    if (events & EPOLLIN) {
	char *buffer=NULL;
	size_t lenbuffer=0;
	unsigned char isremote=0;

	if (connection->typedata==NOTIFYFS_OWNERTYPE_CLIENT) {
	    struct client_struct *client=(struct client_struct *) connection->data;

	    /* get the buffer for a client */

	    if ( ! client->buffer) {

		client->buffer=malloc(NOTIFYFS_RECVBUFFERSIZE);

		if (client->buffer) {

		    client->lenbuffer=NOTIFYFS_RECVBUFFERSIZE;
		    logoutput("process_connection_event: buffer created for client (size: %i)", client->lenbuffer);

		}

	    }

	    buffer=client->buffer;
	    lenbuffer=client->lenbuffer;

	} else if (connection->typedata==NOTIFYFS_OWNERTYPE_SERVER) {
	    struct notifyfs_server_struct *notifyfs_server=(struct notifyfs_server_struct *) connection->data;

	    if ( ! notifyfs_server->buffer) {

		notifyfs_server->buffer=malloc(NOTIFYFS_RECVBUFFERSIZE);

		if (notifyfs_server->buffer) {

		    notifyfs_server->lenbuffer=NOTIFYFS_RECVBUFFERSIZE;
		    logoutput("process_connection_event: buffer created for server (size: %i)", notifyfs_server->lenbuffer);

		}

	    }

	    buffer=notifyfs_server->buffer;
	    lenbuffer=notifyfs_server->lenbuffer;

	} else if (connection->typedata==NOTIFYFS_OWNERTYPE_BACKEND) {
	    struct notifyfs_backend_struct *notifyfs_backend=(struct notifyfs_backend_struct *) connection->data;

	    if ( ! notifyfs_backend->buffer) {

		notifyfs_backend->buffer=malloc(NOTIFYFS_RECVBUFFERSIZE);

		if (notifyfs_backend->buffer) {

		    notifyfs_backend->lenbuffer=NOTIFYFS_RECVBUFFERSIZE;
		    logoutput("process_connection_event: buffer created for backend (size: %i)", notifyfs_backend->lenbuffer);

		}

	    }

	    buffer=notifyfs_backend->buffer;
	    lenbuffer=notifyfs_backend->lenbuffer;

	}

	int res=receive_message(connection->fd, connection->data, events, connection->typedata, buffer, lenbuffer);

	if (res==0) {

	    /* 
		in case of a connection with a remote server this means a hangup 
	    */

	    if (is_remote(connection)==1) {

		events|=EPOLLHUP;

	    }

	}

    }

    if (events & ( EPOLLHUP | EPOLLRDHUP ) ) {

	if (connection->typedata==NOTIFYFS_OWNERTYPE_CLIENT) {
	    struct client_struct *client;

	    close(connection->fd);

	    client=(struct client_struct *) connection->data;

	    if (client) {
		struct view_struct *view=NULL;
		void *view_index=NULL;

		logoutput("process_connection_event: hangup of client %i, remove watches", client->pid);

		/* here clear all clients watches */

		client->connection=NULL;

		/*
		    bring all views used by this client down
		*/

		view=get_next_view(client->pid, &view_index);

		while(view) {

		    if (view->pid==client->pid) {

			pthread_mutex_lock(&view->mutex);
			view->status=NOTIFYFS_VIEWSTATUS_CLIENTDOWN;
			pthread_cond_broadcast(&view->cond);
			pthread_mutex_unlock(&view->mutex);

		    }

		    view=get_next_view(client->pid, &view_index);

		}

		remove_clientwatches_client(client);
		remove_client(client);

	    }

	    connection->fd=0;

	} else if (connection->typedata==NOTIFYFS_OWNERTYPE_SERVER) {
	    struct notifyfs_server_struct *notifyfs_server=(struct notifyfs_server_struct *) connection->data;

	    close(connection->fd);
	    connection->fd=0;

	    /* remote server hangup */

	    if (notifyfs_server) {

		logoutput("process_connection_event: hangup of remote server");

		notifyfs_server->status=NOTIFYFS_SERVERSTATUS_DOWN;
		notifyfs_server->connection=NULL;

		remove_clientwatches_server(notifyfs_server);

	    }

	} else if (connection->typedata==NOTIFYFS_OWNERTYPE_BACKEND) {
	    struct notifyfs_backend_struct *notifyfs_backend=(struct notifyfs_backend_struct *) connection->data;

	    close(connection->fd);
	    connection->fd=0;

	    /* what to do here ?? 

		with a remote server as backend?
		with a fuse fs as backend? this will also be reported when the fs is unmounted 
		with the kernel... this will not happen

		in general: there is nothing here when a watch is forwarded to the backend
	    */

	    if (notifyfs_backend) {

		logoutput("process_connection_event: hangup of backend");

		notifyfs_backend->status=NOTIFYFS_BACKENDSTATUS_DOWN;
		notifyfs_backend->connection=NULL;

	    }

	}

	free(connection);

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

	    client=register_client(connection->fd, credentials.pid, credentials.uid, credentials.gid, 0, 0);

	    if (client) {

		connection->data=(void *) client;
		connection->typedata=NOTIFYFS_OWNERTYPE_CLIENT;
		connection->process_event=process_connection_event;
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

int add_networkserver(struct notifyfs_connection_struct *new_connection, uint32_t events)
{
    int nreturn=0;

    /* what to match against the connection ?? */

    if (is_remote(new_connection)==1) {
	struct notifyfs_connection_struct *connection=NULL;

	connection=compare_notifyfs_connections(new_connection);

	if (connection) {

	    /* existing connection found */

	    if (connection->typedata==NOTIFYFS_OWNERTYPE_SERVER) {
		struct notifyfs_server_struct *notifyfs_server=NULL;

		notifyfs_server=(struct notifyfs_server_struct *) connection->data;

		if (notifyfs_server) {

		    /* there is already a connection to the same host */

		    nreturn=-1;

		}

	    }

	} else {
	    struct notifyfs_server_struct *notifyfs_server=NULL;

	    /* get a new notifyfs server */

	    notifyfs_server=create_notifyfs_server();

	    if (notifyfs_server) {

		notifyfs_server->connection=new_connection;
		notifyfs_server->status=NOTIFYFS_SERVERSTATUS_UP;
		notifyfs_server->type=NOTIFYFS_SERVERTYPE_NETWORK;

		get_current_time(&notifyfs_server->connect_time);

		new_connection->data=(void *) notifyfs_server;
		new_connection->typedata=NOTIFYFS_OWNERTYPE_SERVER;
		new_connection->process_event=process_connection_event;

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
    struct workerthreads_queue_struct notifyfs_threadsqueue=WORKERTHREADS_QUEUE_INIT;
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

    set_max_nr_workerthreads(&notifyfs_threadsqueue, 8);
    add_workerthreads(&notifyfs_threadsqueue, 8);

    init_changestate(&notifyfs_threadsqueue);

    /*
	 create and init the shared memory blocks
    */

    res=initialize_sharedmemory_notifyfs(0, notifyfs_options.shm_gid);

    if ( res<0 ) {

	fprintf(stderr, "Error, cannot intialize shared memory (error: %i).\n", abs(res));
	exit(1);

    }

    /*
        create the root inode and entry
    */

    res=init_entry_management();

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

    /* read supported filesystems */

    register_fsfunctions();
    read_supported_filesystems(NULL, NULL);

    /* read fstab */

    read_fstab();

    /* initialize watch tables */

    init_watch_hashtables();

    /* initialize the watches */

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

    init_handleclientmessage(&notifyfs_threadsqueue);

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

    if ( notifyfs_options.forwardnetwork==1) {

	init_local_backend();
	init_local_server();

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

	if (notifyfs_options.ipv6==0) {

	    inet_fd=create_inet_serversocket(AF_INET, notifyfs_options.networkport, &inetserver_socket, NULL, add_networkserver);

	} else {

	    inet_fd=create_inet_serversocket(AF_INET6, notifyfs_options.networkport, &inetserver_socket, NULL, add_networkserver);

	}

	if ( inet_fd<=0 ) {

    	    logoutput("Error creating network socket fd: error %i.", inet_fd);
    	    goto out;

	} else {

	    logoutput("Network socket fd %i created.", inet_fd);

	}

    }

    /*
	add mount monitor
    */

    init_handlemountinfoevent(&notifyfs_threadsqueue);

    res=open_mountmonitor(&xdata_mountinfo, update_notifyfs, skip_mountentry);

    if ( res<0 ) {

        logoutput("main: unable to open mountmonitor");
        goto out;

    }

    epoll_xdata=add_to_epoll(xdata_mountinfo.fd, EPOLLERR, process_mountinfo_event, NULL, &xdata_mountinfo, NULL);

    if ( ! epoll_xdata ) {

        logoutput("error adding mountinfo fd to mainloop");
        goto out;

    } else {

        logoutput("mountinfo fd %i added to epoll", xdata_mountinfo.fd);
	add_xdata_to_list(epoll_xdata);

    }

    /* read the mountinfo to initialize */

    process_mountinfo_event(0, NULL, 0);

    /* fs notify backends */

    initialize_fsnotify_backends();

    /* enable fuse or not */

    if (notifyfs_options.enablefusefs==1) {

	/* start fuse */

	res=initialize_fuse(notifyfs_options.mountpoint, "notifyfs", &notifyfs_oper, sizeof(notifyfs_oper), &global_fuse_args, &notifyfs_threadsqueue);
	if (res<0) goto out;

    }

    res=start_epoll_eventloop(NULL, NULL);

    out:

    if (notifyfs_options.enablefusefs==1) finish_fuse();

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

    destroy_workerthreads_queue(&notifyfs_threadsqueue);

    skipeverything:

    closelog();

    return res ? 1 : 0;

}
