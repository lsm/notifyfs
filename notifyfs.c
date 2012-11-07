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

// required??

#include <ctype.h>

#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <pthread.h>


#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

#include <fuse/fuse_lowlevel.h>

#include "entry-management.h"
#include "path-resolution.h"
#include "logging.h"
#include "notifyfs.h"

#include "epoll-utils.h"

#include "handlefuseevent.h"
#include "handlemountinfoevent.h"

#include "workerthreads.h"

#include "utils.h"
#include "options.h"
#include "xattr.h"
#include "message.h"
#include "client-io.h"
#include "client.h"
#include "socket.h"
#include "access.h"

#include "mountinfo.h"
#include "mountinfo-monitor.h"

struct notifyfs_options_struct notifyfs_options;
struct fuse_args global_fuse_args = FUSE_ARGS_INIT(0, NULL);

struct fuse_chan *notifyfs_chan;
struct notifyfs_entry_struct *root_entry;
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

    entry=find_entry(parentino, name);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    inode=entry->inode;

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

    //  if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    if ( dostat==1 ) {

	if ( call_info.mount_entry ) {

    	    if ( mount_is_up(call_info.mount_entry)==0 ) {

		/* mount is down, so do not stat
            	   what with DFS like constructions ?? */

        	dostat=0;

        	/* here something with times and mount times?? */

	    }

        }

    }

    if ( dostat==1 ) {

        /* check entry on underlying fs */

        nreturn=lstat(call_info.path, &(e.attr));

        /* here copy e.attr to inode->st */

        if (nreturn!=-1) {

	    copy_stat(&inode->st, &(e.attr));


	} else {

	    logoutput("lookup: stat on %s gives error %i", call_info.path, errno);

	}

    } else {

        /* here copy the stat from inode->st */

        copy_stat(&(e.attr), &inode->st);
        nreturn=0;

    }

    out:

    if ( nreturn==-ENOENT) {

	logoutput("lookup: entry does not exist (ENOENT)");

	e.ino = 0;
	e.entry_timeout = notifyfs_options.negative_timeout;

    } else if ( nreturn<0 ) {

	logoutput("do_lookup: error (%i)", nreturn);

    } else {

	// no error

	entry->inode->nlookup++;
	e.ino = entry->inode->ino;
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

    //if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

	/* here change the action to DOWN and leave it to changestate what to do?
	   REMOVE of SLEEP */

        //if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

    //}

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

    entry=inode->alias;

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

    /* if watch attached then not stat */

    // if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    if ( dostat==1 ) {

	if ( call_info.mount_entry ) {

    	    if ( mount_is_up(call_info.mount_entry)==0 ) {

        	/* what with DFS like constructions ?? */
        	dostat=0;

        	/* here something with times and mount times?? */

    	    }

	}

    }


    if ( dostat==1 ) {

        /* get the stat from the underlying fs */

        nreturn=lstat(call_info.path, &st);

        /* copy the st -> inode->st */

        if ( nreturn!=-1 ) copy_stat(&inode->st, &st);

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);
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

    //if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        //if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

    //}

    if ( call_info.freepath==1 ) free(call_info.path);

}

static void notifyfs_access (fuse_req_t req, fuse_ino_t ino, int mask)
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

    entry=inode->alias;

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

    /* if watch attached then not stat */

    // if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( call_info.mount_entry ) {

        if ( mount_is_up(call_info.mount_entry)==0 ) {

            /* what with DFS like constructions ?? */
            dostat=0;

            /* here something with times and mount times?? */

        }

    }


    if ( dostat==1 ) {

        /* get the stat from the root fs */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if (res!=-1) {

	    copy_stat(&inode->st, &st);

	}

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);

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

    //if ( nreturn==-ENOENT && call_info.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        //if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

    //}

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

    entry=find_entry(parentino, name);

    if ( ! entry ) {
        struct notifyfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct notifyfs_entry_struct *pentry;

	    pentry=pinode->alias;
	    entry=create_entry(pentry, name, NULL);
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

    if (call_info.mount_entry) {

	if ( mount_is_up(call_info.mount_entry)==0 ) {

	    /* underlying mount is not mounted: creating is not permitted */

	    nreturn=-EACCES;
	    goto error;

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

	// no error

        assign_inode(entry);

        if ( ! entry->inode ) {

            nreturn=-ENOMEM;
            goto error;

        }

	entry->inode->nlookup++;

	copy_stat(&entry->inode->st, &(e.attr));

	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

        add_to_inode_hash_table(entry->inode);
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

    entry=find_entry(parentino, name);

    if ( ! entry ) {
        struct notifyfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct notifyfs_entry_struct *pentry;

	    pentry=pinode->alias;
	    entry=create_entry(pentry, name, NULL);
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

    if (call_info.mount_entry) {

	if ( mount_is_up(call_info.mount_entry)==0 ) {

	    /* underlying mount is not mounted: creating is not permitted */

	    nreturn=-EACCES;
	    goto error;

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

	// no error

        assign_inode(entry);

        if ( ! entry->inode ) {

            nreturn=-ENOMEM;
            goto error;

        }

	entry->inode->nlookup++;

	copy_stat(&entry->inode->st, &(e.attr));

	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

        add_to_inode_hash_table(entry->inode);
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
    struct notifyfs_entry_struct *entry;;
    struct notifyfs_inode_struct *inode;
    struct call_info_struct *call_info=NULL;

    logoutput("OPENDIR");

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    /* register call */

    call_info=get_call_info(entry);

    if ( ! call_info ) {

        nreturn=-ENOMEM;
        goto out;

    }

    call_info->ctx=fuse_req_ctx(req);

    /* check client access */

    if ( notifyfs_options.accessmode!=0 ) {

        if ( test_access_fsuser(call_info)==1 ) {

            nreturn=-EACCES;
            goto out;

        }

    }

    /* translate entry into path */

    nreturn=determine_path(call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    dirp = malloc(sizeof(struct notifyfs_generic_dirp_struct));

    if ( ! dirp ) {

	nreturn=-ENOMEM;
	goto out;

    }

    memset(dirp, 0, sizeof(struct notifyfs_generic_dirp_struct));

    dirp->entry=NULL;
    dirp->upperfs_offset=0;
    dirp->underfs_offset=1;
    dirp->call_info=call_info;

    dirp->generic_fh.entry=entry;

    // assign this object to fi->fh

    fi->fh = (unsigned long) dirp;


    out:

    if ( nreturn<0 ) {

        if ( call_info ) remove_call_info(call_info);
	if ( dirp ) free_dirp(dirp);

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_open(req, fi);

    }

    logoutput("opendir, nreturn %i", nreturn);

}



static int do_readdir_localhost(fuse_req_t req, char *buf, size_t size, off_t upperfs_offset, struct notifyfs_generic_dirp_struct *dirp)
{
    size_t bufpos = 0;
    int res, nreturn=0;
    size_t entsize;
    bool direntryfrompreviousbatch=false;
    char *entryname=NULL;
    unsigned char namecreated=0;

    logoutput("DO_READDIR, offset: %"PRId64, upperfs_offset);

    dirp->upperfs_offset=upperfs_offset;
    if ( dirp->upperfs_offset==0 ) dirp->upperfs_offset=1;

    if ( dirp->entry ) direntryfrompreviousbatch=true;


    while (bufpos < size ) {

        namecreated=0;

        if ( dirp->underfs_offset == 1 ) {

            /* the . entry */

            dirp->st.st_ino = dirp->generic_fh.entry->inode->ino;
            entryname=malloc(2);

            if ( entryname ) {

                sprintf(entryname, ".");
                namecreated=1;

            } else {

                nreturn=-ENOMEM;
                goto out;

            }

        } else if ( dirp->underfs_offset == 2 ) {

            /* the .. entry */

	    if (isrootinode(dirp->generic_fh.entry->inode) == 1 ) {

	        dirp->st.st_ino = FUSE_ROOT_ID;

	    } else {

	        dirp->st.st_ino = dirp->generic_fh.entry->parent->inode->ino;

	    }

            entryname=malloc(3);

            if ( entryname ) {

                sprintf(entryname, "..");
                namecreated=1;

            } else {

                nreturn=-ENOMEM;
                goto out;

            }

        } else {

            if ( dirp->underfs_offset == 3 ) {

                /* first "normal" entry */

                dirp->entry=get_next_entry(dirp->generic_fh.entry, NULL);

            } else {

                /* every next entry */

                if ( ! direntryfrompreviousbatch ) {

                    if ( dirp->entry ) dirp->entry=get_next_entry(dirp->generic_fh.entry, dirp->entry);

                }

            }

            if ( dirp->entry ) {

                /* valid entry */

                dirp->st.st_ino = dirp->entry->inode->ino;
                dirp->st.st_mode = dirp->entry->inode->st.st_mode;

                entryname=dirp->entry->name;

            } else {

                /* no valid entry: end of stream */

                break;

            }

        }

	// break when buffer is not large enough
	// function fuse_add_direntry has not added it when buffer is too small to hold direntry, 
	// only returns the requested size

        entsize = fuse_add_direntry(req, buf + bufpos, size - bufpos, entryname, &dirp->st, dirp->upperfs_offset);

        if ( namecreated==1 ) {

            free(entryname);

        }

	if (entsize > size - bufpos) {

	    // the direntry does not fit in buffer
	    // (keep the current direntry and entry attached)
	    break;

	} else {

            dirp->upperfs_offset+=1;
            dirp->underfs_offset+=1;

        }

	bufpos += entsize;

    }


    out:

    if (nreturn<0) {

	logoutput1("do_readdir, return: %i", nreturn);

	return nreturn;

    } else {

	logoutput1("do_readdir, return: %zu", bufpos);

	return bufpos;

    }

}


static void notifyfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct notifyfs_generic_dirp_struct *dirp = get_dirp(fi);
    char *buf;
    int nreturn=0;

    logoutput("READDIR, size: %zi", size);

    // look what readdir has to be called

    buf = malloc(size);

    if (buf == NULL) {

	nreturn=-ENOMEM;
	goto out;

    }

    nreturn=do_readdir_localhost(req, buf, size, offset, dirp);

    out:

    if (nreturn < 0 ) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_buf(req, buf, nreturn);

    }

    logoutput1("readdir, nreturn %i", nreturn);
    if ( buf ) free(buf);

}


static void notifyfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct notifyfs_generic_dirp_struct *dirp = get_dirp(fi);

    (void) ino;

    logoutput("RELEASEDIR");

    fuse_reply_err(req, 0);

    if ( dirp->call_info ) remove_call_info(dirp->call_info);

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

    entry=inode->alias;

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

	// st.f_bsize=4096; /* good?? */
	// st.f_frsize=st.f_bsize; /* no fragmentation on this fs */
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

    entry=inode->alias;

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

    // if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( dostat==1 ) {

	if ( call_info.mount_entry ) {

	    if ( mount_is_up(call_info.mount_entry)==0 ) {

		/* when dealing with a sleeping mount, and it's not mounted no stat*/

		dostat=0;

	    }

	}

    }


    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if ( res!=-1 ) {

	    copy_stat(&inode->st, &st);

	}

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);

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

    //if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

    //    if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

    //}

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

    entry=inode->alias;

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

    // if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( dostat==1 ) {

	if ( call_info.mount_entry ) {

    	    if ( mount_is_up(call_info.mount_entry)==0 ) {

        	/* what with DFS like constructions ?? */
        	dostat=0;

    	    }

	}

    }


    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if ( res!=-1) {

	    copy_stat(&inode->st, &st);

	}

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);

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

    //if ( nreturn==-ENOENT && call_info.path ) {

        /* entry in this fs exists but underlying entry not anymore */

    //    if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

    //}

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

    entry=inode->alias;

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

    // if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( dostat==1 ) {

	if ( call_info.mount_entry ) {

    	    if ( mount_is_up(call_info.mount_entry)==0 ) {

        	/* what with DFS like constructions ?? */
        	dostat=0;

    	    }

	}

    }


    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if (res!=-1) {

	    copy_stat(&inode->st, &st);

	}

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);

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

    //if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

    //    if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

    //}

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


void create_notifyfs_mount_path(struct mount_entry_struct *mount_entry)
{
    struct call_info_struct call_info;

    init_call_info(&call_info, NULL);

    call_info.path=mount_entry->mountpoint;
    call_info.freepath=0;

    create_notifyfs_path(&call_info);

    if ( call_info.entry ) {

	mount_entry->data0=(void *) call_info.entry;
	mount_entry->typedata0=NOTIFYFS_MOUNTDATA_ENTRY;
	call_info.entry->mount_entry=mount_entry;

	mount_entry->status=MOUNT_STATUS_UP;

    } else {

	/* somehow failed to create the path */

	logoutput("create_notifyfs_mount_path: unable to create mount point %s", mount_entry->mountpoint);

	mount_entry->status=MOUNT_STATUS_NOTSET;

    }

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

    /* log based on condition */

    logoutput_list(MOUNTENTRY_ADDED, 0);
    logoutput_list(MOUNTENTRY_REMOVED, 0);
    logoutput_list(MOUNTENTRY_REMOVED_KEEP, 0);

    /* walk through removed mounts to see it affects the fs */

    res=lock_mountlist();

    /* start with last: the list is sorted, bigger/longer (also submounts) mounts are first this way */

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_REMOVED);

    while (mount_entry) {


        entry=(struct notifyfs_entry_struct *) mount_entry->data0;

        /* if normal umount then remove the tree... 
               if umounted by autofs then set tree to sleep */

        if ( entry ) {
            struct call_info_struct call_info;

	    init_call_info(&call_info, entry);

            call_info.path=mount_entry->mountpoint;
	    call_info.mount_entry=mount_entry;

            /* normal umount: remove whole tree */

            // changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE );

        }

	/* send message to inform clients.. not on startup */

	// if ( firstrun==0 ) send_mount_message_to_clients(mount_entry);

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_REMOVED);

    }

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_REMOVED_KEEP);

    while (mount_entry) {

	if ( mount_entry->processed==1 ) {

	    mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_REMOVED_KEEP);
	    continue;

	}

        entry=(struct notifyfs_entry_struct *) mount_entry->data0;

        /* if normal umount then remove the tree... 
               if umounted by autofs then set tree to sleep */

        if ( entry ) {
            struct call_info_struct call_info;

	    init_call_info(&call_info, entry);

            call_info.path=mount_entry->mountpoint;
	    call_info.mount_entry=mount_entry;

            //changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE);

        }

	mount_entry->processed=1;

	client=(struct client_struct *) mount_entry->data1;

	/* send message to inform clients.. not on startup... TODO */

	// if ( firstrun==0 ) send_mount_message_to_clients(mount_entry);

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_REMOVED_KEEP);

    }


    /* in any case the watches set on an autofs managed fs, get out of "sleep" mode */

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_ADDED);

    while (mount_entry) {

	if ( is_rootmount(mount_entry) ) {

	    /* link rootmount and rootentry */

	    if ( ! mount_entry->data0 ) {

		/* not already assigned */

		entry=get_rootentry();

		mount_entry->data0=(void *) entry;
		entry->mount_entry=mount_entry;

	    }

	    /* root is already created */

	    goto next;

	}

	/* match a client fs if there is one, not at init(TODO) */

	// determine_backend_mountpoint(mount_entry);

        entry=(struct notifyfs_entry_struct *) mount_entry->data0;

        if ( entry ) {
            struct call_info_struct call_info;

            /* entry does exist already, there is a tree already related to this mount */
            /* walk through tree and wake everything up */

	    init_call_info(&call_info, entry);

            call_info.path=mount_entry->mountpoint;
	    call_info.mount_entry=mount_entry;

            //changestate(&call_info, FSEVENT_ACTION_TREE_UP);

        } else {

	    if ( strcmp(mount_entry->mountpoint, "/") == 0 ) {

		/* skip the root... not necessary to create....*/
		goto next;

	    } else if ( strcmp(notifyfs_options.mountpoint, mount_entry->mountpoint)==0 ) {

		/* skip the mountpoint of this fs, caused deadlocks in the past... 
		   this will be not the case anymore since the threads for handling the fuse
		   events are up and running independent of this thread... needs testing */

		goto next;

	    }

            create_notifyfs_mount_path(mount_entry);

        }

	/* send message to inform clients.. not on startup */

	// if ( firstrun==0 ) send_mount_message_to_clients(mount_entry);

	next:

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_ADDED);

    }

    unlock:

    res=unlock_mountlist();

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
    int res, epoll_fd, socket_fd, mountinfo_fd;
    struct stat st;
    pthread_t threadid_mountmonitor=0;
    struct epoll_extended_data_struct *epoll_xdata=NULL;
    struct epoll_extended_data_struct xdata_socket=EPOLL_XDATA_INIT;
    struct epoll_extended_data_struct xdata_mountinfo=EPOLL_XDATA_INIT;

    umask(0);

    // set logging

    openlog("fuse.notifyfs", 0,0); 

    /* parse commandline options and initialize the fuse options */

    res=parse_arguments(argc, argv, &global_fuse_args);

    if ( res<0 ) {

	res=0;
	goto skipeverything;

    }

    //
    // init the name and inode hashtables
    //

    res=init_hashtables();

    if ( res<0 ) {

	fprintf(stderr, "Error, cannot intialize the hashtables (error: %i).\n", abs(res));
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

    socket_fd=create_socket(notifyfs_options.socket);

    if ( socket_fd<=0 ) {

        logoutput("Error creating socket fd: %i.", socket_fd);
        goto out;

    }

    notifyfs_options.socket_fd=socket_fd;
    assign_socket_callback(client_socketfd_callback);

    /*
    *     add mount info 
    */

    mountinfo_fd=open(MOUNTINFO_FILE, O_RDONLY);

    if ( mountinfo_fd==-1 ) {

        logoutput("unable to open file %s", MOUNTINFO_FILE);
        goto out;

    }

    epoll_xdata=add_to_epoll(mountinfo_fd, EPOLLERR, TYPE_FD_INOTIFY, process_mountinfo_event, NULL, &xdata_mountinfo, NULL);

    if ( ! epoll_xdata ) {

        logoutput("error adding mountinfo fd to mainloop");
        goto out;

    } else {

        logoutput("mountinfo fd %i added to epoll", mountinfo_fd);

	add_xdata_to_list(epoll_xdata, NULL);

    }

    register_mountinfo_callback(MOUNTINFO_CB_ONUPDATE, &update_notifyfs);

    process_mountinfo(NULL);

    /* add the fuse channel(=fd) to the mainloop */

    res=initialize_fuse(notifyfs_options.mountpoint);

    if (res<0) goto out;

    start_workerthreads();

    res=start_epoll_eventloop(NULL);

    out:

    finish_fuse();

    if ( xdata_socket.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_socket, 0);
	close(xdata_socket.fd);
	xdata_socket.fd=0;
	notifyfs_options.socket_fd=0;
	remove_xdata_from_list(&xdata_socket, 0, NULL);

    }

    unlink(notifyfs_options.socket);

    if ( xdata_mountinfo.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_mountinfo, 0);
	close(xdata_mountinfo.fd);
	xdata_mountinfo.fd=0;
	remove_xdata_from_list(&xdata_mountinfo, 0, NULL);

    }

    /* remove any remaining xdata from mainloop */

    destroy_eventloop(NULL);
    fuse_opt_free_args(&global_fuse_args);

    skipeverything:

    closelog();

    return res ? 1 : 0;

}
