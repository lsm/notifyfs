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

#include "utils.h"
#include "options.h"
#include "xattr.h"
#include "client.h"
#include "socket.h"
#include "access.h"
#include "mountinfo.h"
#include "mountinfo-monitor.h"

#include "watches.h"
#include "determinechanges.h"
#include "changestate.h"

#include "message.h"
#include "message-server.h"

#include "networksocket.h"

struct notifyfs_options_struct notifyfs_options;

struct fuse_chan *notifyfs_chan;
struct notifyfs_entry_struct *root_entry;
extern struct mount_entry_struct *root_mount;
unsigned char loglevel=0;
int logarea=0;

/* function to remove a client app */

void remove_client_app(struct client_struct *client)
{
    int res;

    /* remove every watch of this client */

    if ( client->watches ) {
	struct watch_struct *watch=client->watches;
	struct effective_watch_struct *effective_watch=NULL;
	int newmask,oldmask;

	/* remove the watch from the inodes, and if there no watches left there, 
	   remove the effective_watch */

	while (watch) {

	    /* here remove from effective watch */

	    effective_watch=watch->effective_watch;

	    if ( effective_watch ) {

    		/* lock the effective watch */

		res=lock_effective_watch(effective_watch);

		/* remove watch from inode/effective_watch */

		if ( effective_watch->watches==watch ) effective_watch->watches=watch->next_per_watch;

		if ( watch->next_per_watch ) {

		    watch->next_per_watch->prev_per_watch=watch->prev_per_watch;
		    watch->next_per_watch=NULL;

		}

		if ( watch->prev_per_watch ) {

		    watch->prev_per_watch->next_per_watch=watch->next_per_watch;
		    watch->prev_per_watch=NULL;

		}

		/* recalculate the effective mask */

		oldmask=effective_watch->mask;
		newmask=calculate_effmask(effective_watch, 1);

		if ( effective_watch->nrwatches==0 ) {

		    /* remove the effective watch */

		    logoutput("remove_client_app: remove effective watch, no more watches...");

		    del_watch_backend(effective_watch);

		    if (effective_watch->inode) effective_watch->inode->effective_watch=NULL;
		    effective_watch->inode=NULL;

		    if ( effective_watch->path ) {

			free(effective_watch->path);
			effective_watch->path=NULL;

		    }

		    remove_effective_watch_from_list(effective_watch, 0);
		    move_effective_watch_to_unused(effective_watch);

		    /* big to do : correct the fs, is the inode still usefull?? */


		} else {

		    newmask|=(IN_DELETE_SELF | IN_MOVE_SELF);

		    if ( newmask != oldmask ) {

			effective_watch->mask=newmask;

			/* here update the current backend watch */

			set_watch_backend(effective_watch, newmask, 1);

		    }

		}

		res=unlock_effective_watch(effective_watch);

	    }

	    client->watches=watch->next_per_client;
	    watch->client=NULL;
	    watch->prev_per_client=NULL;

	    if ( watch->next_per_client ) {

		watch=watch->next_per_client;
		free(watch->prev_per_client);
		watch->prev_per_client=NULL;

	    } else {

		free(watch);
		watch=NULL;

	    }

	}

    }

}


void remove_client_fs(struct client_struct *client)
{
    int res;
    struct effective_watch_struct *effective_watch;
    unsigned char dosleep=0;

    if ( client->mount_entry ) {

	if ( client->mount_entry->isautofs==1 ) {

	    /* here: when dealing with a autofs mounted fs, set the watches to sleep mode, otherwise remove */
	    /* possibly */

	    dosleep=1;

	}

    }

    res=lock_effective_watches();

    /* first remove any watch using this client fs as backend */

    effective_watch=get_next_effective_watch(NULL);

    while(effective_watch) {

	if ( client != (struct client_struct *) effective_watch->backend ) {

	    /* forwarding watch, but different client */

	    effective_watch=effective_watch->next;
	    continue;

	}

    	/* lock the effective watch */

	res=lock_effective_watch(effective_watch);

	/* every watch */

	if ( effective_watch->watches ) {
	    struct watch_struct *watch=effective_watch->watches;

	    while(watch) {

		if ( dosleep==0 ) {

		    effective_watch->watches=watch->next_per_watch; /* shift */
		    watch->prev_per_watch=NULL;
		    watch->next_per_watch=NULL;

		}

		if ( watch->client ) {
		    struct client_struct *client_app=watch->client;

		    /* send client a message */

		    if ( dosleep==1 ) {

			send_sleepwatch_message(client_app->fd, watch->id);

		    } else {

			send_delwatch_message(client_app->fd, watch->id);

			/*remove watch from client app */

			res=lock_client(client_app);

			if ( client_app->watches==watch ) client_app->watches=watch->next_per_client;
            		if ( watch->prev_per_client ) watch->prev_per_client->next_per_client=watch->next_per_client;
            		if ( watch->next_per_client ) watch->next_per_client->prev_per_client=watch->prev_per_client;

			res=unlock_client(client_app);

            		watch->prev_per_client=NULL;
            		watch->next_per_client=NULL;
            		watch->client=NULL;

            		free(watch);

			effective_watch->nrwatches--;

		    }

		}

		if ( dosleep==0 ) {

		    watch=effective_watch->watches;

		} else {

		    watch=watch->next_per_watch;

		}

	    }

	}

	if ( effective_watch->nrwatches==0 ) {
	    struct effective_watch_struct *tmp_effective_watch=get_next_effective_watch(effective_watch);

	    /* detach from inode */

	    if ( effective_watch->inode ) effective_watch->inode->effective_watch=NULL;
	    effective_watch->inode=NULL;

	    if ( effective_watch->path ) {

		free(effective_watch->path);
		effective_watch->path=NULL;

	    }

	    /* remove the effective watch (lock set)*/

	    remove_effective_watch_from_list(effective_watch, 1);

	    /* move to unused */

	    move_effective_watch_to_unused(effective_watch);

	    unlock_effective_watch(effective_watch);

	    effective_watch=tmp_effective_watch;

	} else {

	    unlock_effective_watch(effective_watch);

	    effective_watch=get_next_effective_watch(effective_watch);

	}

    }

    res=unlock_effective_watches();

    if ( client->path ) {

	free(client->path);
	client->path=NULL;

    }

}

void remove_client(struct client_struct *client)
{
    int res;

    /* lock client */

    res=lock_client(client);

    client->status_fs=NOTIFYFS_CLIENTSTATUS_DOWN;
    client->status_app=NOTIFYFS_CLIENTSTATUS_DOWN;

    if ( client->type & NOTIFYFS_CLIENTTYPE_FS ) {

	remove_client_fs(client);

    }

    if ( client->type & NOTIFYFS_CLIENTTYPE_APP ) {

	remove_client_app(client);

    }

    /* unlock client 
       do not destroy it yet, this is done when the client really disconnects from socket */

    res=unlock_client(client);

}

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

	    /* keep for later use */

	    call_info->client=client;

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

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

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

	    update_timespec(&inode->laststat);
	    copy_stat(&inode->st, &(e.attr));

	    inode->lastaction=FSEVENT_STAT_STAT;

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

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

	/* here change the action to DOWN and leave it to changestate what to do?
	   REMOVE of SLEEP */

        if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

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

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

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

	update_timespec(&inode->laststat);
        copy_stat(&st, &inode->st);
	inode->lastaction=FSEVENT_STAT_STAT;
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

        if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

    }

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

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

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
	    update_timespec(&inode->laststat);
	    inode->lastaction=FSEVENT_STAT_STAT;

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

    if ( nreturn==-ENOENT && call_info.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

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
	update_timespec(&entry->inode->laststat);
	entry->inode->lastaction=FSEVENT_STAT_STAT;

	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

        add_to_inode_hash_table(entry->inode);
        add_to_name_hash_table(entry);

        /* insert in directory (some lock required (TODO)????) */

        if ( entry->parent ) {

            entry->dir_next=NULL;
            entry->dir_prev=NULL;

            if (entry->parent->child) {

                entry->parent->child->dir_prev=entry;
                entry->dir_next=entry->parent->child;

            }

            entry->parent->child=entry;

        }

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
	update_timespec(&entry->inode->laststat);
	entry->inode->lastaction=FSEVENT_STAT_STAT;

	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

        add_to_inode_hash_table(entry->inode);
        add_to_name_hash_table(entry);

        /* insert in directory (some lock required (TODO)????) */

        if ( entry->parent ) {

            entry->dir_next=NULL;
            entry->dir_prev=NULL;

            if (entry->parent->child) {

                entry->parent->child->dir_prev=entry;
                entry->dir_next=entry->parent->child;

            }

            entry->parent->child=entry;

        }

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

                dirp->entry=dirp->generic_fh.entry->child;

            } else {

                /* every next entry */

                if ( ! direntryfrompreviousbatch ) {

                    if ( dirp->entry ) dirp->entry=dirp->entry->dir_next;

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

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

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
	    update_timespec(&inode->laststat);
	    inode->lastaction=FSEVENT_STAT_STAT;

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

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

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

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

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
	    update_timespec(&inode->laststat);
	    inode->lastaction=FSEVENT_STAT_STAT;

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

    if ( nreturn==-ENOENT && call_info.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

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

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

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
	    update_timespec(&inode->laststat);
	    inode->lastaction=FSEVENT_STAT_STAT;

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

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, NOTIFYFS_TREE_DOWN);

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

    // remove pid file

    remove_pid_file();

}



/* wrapper around signal_mountmonitor */

int handle_data_on_mountinfo_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo)
{

    if ( ! epoll_xdata ) {

	signal_mountmonitor(1);

    } else {

	signal_mountmonitor(0);

    }

    return 0;

}


/* function to process a fsevent reported by
   inotify or send via socket by client fs
   it's possible to use one function for both "backends" cause
   here one method to describe events is used (inotify):
   mask is in inotify format
   name is the name of the entry
   len is the len of name
   (len is zero, name is NULL when event on watch self 
    stat st - stat provided by the backend. can be NULL. by providing this an extra stat call is not required
    a client fs will be able to provide this as part of the fsevent_message
    method - an id of the backend method

*/

void evaluate_and_process_fsevent(struct effective_watch_struct *effective_watch, char *name, int len, int mask, struct stat *st, unsigned char method)
{
    int res;
    unsigned char remote=(method==FSEVENT_BACKEND_METHOD_FORWARD) ? 1 : 0;

    if (name) {

	logoutput("evaluate_and_process_fsevent: name: %s, len: %i, mask: %i, method: %i", name, len, mask, method);

    } else {

	logoutput("evaluate_and_process_fsevent: NONAME, len: %i, mask: %i, method: %i", len, mask, method);

    }

    /* if no watch then ready */

    if ( ! effective_watch ) return;

    /* test it's an event on a watch long gone 
       watches should be disabled when entry/inodes are removed,
       so receiving messages on them should 
       not happen, but to make sure */

    if ( ! effective_watch->inode ) {

	return;

    } else if ( effective_watch->inode->status!=FSEVENT_INODE_STATUS_OK ) {

	return;

    }

    if ( name && len>0 ) {

        /* something happens on an entry in the directory.. check it's in use
        by this fs, the find command will return NULL if it isn't 
	    (do nothing with close, open and access) */

	if ( mask & ( IN_DELETE | IN_MOVED_FROM | IN_ATTRIB | IN_MODIFY | IN_CREATE | IN_MOVED_TO | IN_IGNORED | IN_UNMOUNT ) ) {
	    struct notifyfs_entry_struct *entry=NULL;

	    entry=find_entry(effective_watch->inode->ino, name);

	    if ( entry ) {

		/* entry found : entry is part of this fs */

		if ( mask & IN_UNMOUNT ) {

		    logoutput("evaluate_and_process_fsevent: entry %s unmounted, ignored, handled by the mountmonitor", name);

		    return;

		}

		/* process file- and attrib changes 
                   note the create and moved_to operation are not likely to happen, since the entry already exists here...

                   TODO: with inotify a mkdir or mknod results in IN_CREATE is followed by a IN_ATTRIB
                   from inotify or VFS point of view these are different operations, but from the userspace these are related!
                   notifyfs will after the first IN_CREATE do a stat call to get the attributes...
                   when the IN_ATTRIB follows shortly after that, it again does a stat call.
                   the question is now is there some way to make/see notifyfs the second IN_ATTRIB can be ignored

                */

		if ( mask & ( IN_ATTRIB | IN_MODIFY | IN_CREATE | IN_MOVED_TO ) ) {
		    unsigned char filechanged=0, lastaction=0;
		    struct notifyfs_inode_struct *inode=entry->inode;

		    if ( mask & IN_ATTRIB ) {

			logoutput("evaluate_and_process_fsevent: attributes %s changed", name);
			lastaction=FSEVENT_STAT_IN_ATTRIB;

		    }

		    if ( mask & IN_MODIFY ) {

			logoutput("evaluate_and_process_fsevent: %s modified", name);
			lastaction=FSEVENT_STAT_IN_MODIFY;

		    }

		    if ( mask & ( IN_CREATE | IN_MOVED_TO ) ) {

			logoutput("evaluate_and_process_fsevent: entry %s created", name);
			lastaction=FSEVENT_STAT_IN_CREATE;

		    }

		    if ( ! inode ) return;

		    if ( st ) {

			/* a stat provided by the backend: use that (and trust it to be correct...) */

			filechanged=determinechanges(&(inode->st), mask, st);

		    } else {
			struct stat test_st;
			struct call_info_struct call_info;

			/* a stat is not provided: get it here */

            		init_call_info(&call_info, entry);

            		res=determine_path(&call_info, NOTIFYFS_PATH_FORCE);
            		if (res<0) return;

			res=lstat(call_info.path, &test_st);

			if ( res==-1 ) {

			    /* strange case: message that entry does exist, but stat gives error... */

			    mask|=IN_DELETE;

			    /* skip the futher actions, go direct to delete... */

			    goto entrydelete1;

			} else {

			    filechanged=determinechanges(&(inode->st), mask, &test_st);
			    update_timespec(&inode->laststat);
			    inode->method=method;
			    inode->lastaction=lastaction;

			}

		    }

		    /* only send when there has really changed something */

		    if ( filechanged != FSEVENT_FILECHANGED_NONE ) {

			logoutput("evaluate_and_process_fsevent: a real change, sending message");

			send_notify_message_clients(effective_watch, mask, len, name, &(inode->st), remote);

		    } else {

			logoutput("evaluate_and_process_fsevent: not a real change, not sending message");

		    }

		}

		entrydelete1:

		if ( mask & ( IN_DELETE | IN_MOVED_FROM | IN_IGNORED ) ) {
		    unsigned char prestatus=FSEVENT_INODE_STATUS_REMOVED;
		    struct call_info_struct call_info;
		    struct notifyfs_inode_struct *inode=entry->inode;

		    logoutput("evaluate_and_process_fsevent: entry %s removed, checking to send a message", name);

		    /* there must be an inode.... */

		    if ( ! inode ) return;

		    prestatus=inode->status;

            	    init_call_info(&call_info, entry);

            	    res=determine_path(&call_info, NOTIFYFS_PATH_FORCE);
            	    if (res<0) return;

		    /* entry deleted (here) so adjust the filesystem */

		    changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE);

		    /* forward message only when something really changed */

		    if ( prestatus!=inode->status && inode->status==FSEVENT_INODE_STATUS_REMOVED) {

			logoutput("evaluate_and_process_fsevent: a real remove, sending message");

			send_notify_message_clients(effective_watch, mask, len, name, NULL, remote);

		    } else {

			logoutput("evaluate_and_process_fsevent: not a real remove, not sending message");

		    }

		    return;

		}

	    } else {

		/* entry not part of fs, just send the message 
		maybe additional create the entry 
		*/

		if ( mask & ( IN_CREATE | IN_MOVED_TO | IN_ATTRIB | IN_MODIFY ) ) {
		    struct notifyfs_entry_struct *parent_entry=NULL;
		    unsigned char lastaction;


		    logoutput("evaluate_and_process_fsevent: entry %s new, not found in notifyfs, creating it", name);

		    if ( mask & IN_ATTRIB ) {

			logoutput("evaluate_and_process_fsevent: attributes %s changed", name);
			lastaction=FSEVENT_STAT_IN_ATTRIB;

		    }

		    if ( mask & IN_MODIFY ) {

			logoutput("evaluate_and_process_fsevent: %s modified", name);
			lastaction=FSEVENT_STAT_IN_MODIFY;

		    }

		    if ( mask & ( IN_CREATE | IN_MOVED_TO ) ) {

			logoutput("evaluate_and_process_fsevent: entry %s created", name);
			lastaction=FSEVENT_STAT_IN_CREATE;

		    }

		    /* here create the entry */

		    parent_entry=effective_watch->inode->alias;
		    entry=create_entry(parent_entry, name, NULL);

		    if ( entry ) {
			struct stat test_st;

			res=0;

			if ( ! st ) {
			    struct call_info_struct call_info;

			    /* stat not defined: get it here */

                    	    init_call_info(&call_info, entry);

                    	    res=determine_path(&call_info, NOTIFYFS_PATH_FORCE);
                    	    if (res<0) return;

			    st=&test_st;

			    res=lstat(call_info.path, st);

			}

			if ( res==0 ) {

			    /* insert at begin */

			    entry->dir_prev=NULL;

			    if ( parent_entry->child ) parent_entry->child->dir_prev=entry;
			    entry->dir_next=parent_entry->child;
			    parent_entry->child=entry;

			    assign_inode(entry);
			    add_to_name_hash_table(entry);
			    add_to_inode_hash_table(entry->inode);

			    copy_stat(&(entry->inode->st), st);
			    update_timespec(&entry->inode->laststat);
			    entry->inode->method=method;
			    entry->inode->lastaction=lastaction;

			    send_notify_message_clients(effective_watch, mask, len, name, st, remote);

			} else {

			    /* huh?? should not happen.. received a message the entry does exist
                               ignore the event futher */

			    remove_entry(entry);

			}

		    }

		} else if ( mask & IN_UNMOUNT ) {

		    /* entry not found in notifyfs, which should not happen since
		       every mount is managed here */

		    /* a config switch here to send a message or not ? */

		    logoutput("evaluate_and_process_fsevent: entry (not found in notifyfs) %s unmounted, ignored, handled by the mountmonitor", name);

		} else if ( mask & ( IN_DELETE | IN_MOVED_FROM | IN_IGNORED ) ) {

		    logoutput("evaluate_and_process_fsevent: entry %s removed, not found in notifyfs", name);

		    /* received a message an entry is deleted, what was not part of this fs, so do nothing just forward */

		    send_notify_message_clients(effective_watch, mask, len, name, st, remote);

		}

	    }

	}

    } else {

	/*  event on watch self
	    do nothing with close, open and access*/

	if ( mask & ( IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB | IN_MODIFY | IN_IGNORED | IN_UNMOUNT ) ) {
	    struct notifyfs_entry_struct *entry=NULL;
	    struct notifyfs_inode_struct *inode=NULL;

	    inode=effective_watch->inode;
	    entry=inode->alias;

	    if ( entry ) {

		/* entry found which is logical, event is on watch, which exists */

		if ( mask & ( IN_ATTRIB | IN_MODIFY ) ) {
		    unsigned char filechanged=0, lastaction=0;


		    if ( mask & IN_ATTRIB ) {

			logoutput("evaluate_and_process_fsevent: watch attributes changed");
			lastaction=FSEVENT_STAT_IN_ATTRIB;

		    }

		    if ( mask & IN_MODIFY ) {

			logoutput("evaluate_and_process_fsevent: watch modified");
			lastaction=FSEVENT_STAT_IN_MODIFY;

		    }

		    if ( st ) {

			/* a stat provided by the backend: use that (and trust it to be correct...) */

			filechanged=determinechanges(&(inode->st), mask, st);

		    } else {
			struct stat test_st;
			struct call_info_struct call_info;

			/* a stat is not provided: get it here */

            		init_call_info(&call_info, entry);

            		res=determine_path(&call_info, NOTIFYFS_PATH_FORCE);
            		if (res<0) return;

			res=lstat(call_info.path, &test_st);

			if ( res==-1 ) {

			    /* strange case: message that entry does exist, but stat gives error... */

			    mask|=IN_DELETE;
			    goto entrydelete2;

			} else {

			    filechanged=determinechanges(&(inode->st), mask, &test_st);
			    update_timespec(&inode->laststat);
			    inode->method=method;
			    inode->lastaction=lastaction;

			}

		    }

		    /* only send when there has really changed something */

		    if ( filechanged != FSEVENT_FILECHANGED_NONE ) {

			logoutput("evaluate_and_process_fsevent: changes detected, sending message");

			send_notify_message_clients(effective_watch, mask, 0, NULL, &(inode->st), remote);

		    } else {

			logoutput("evaluate_and_process_fsevent: no changes detected");

		    }

		}

		entrydelete2:

		if ( mask & ( IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED | IN_UNMOUNT ) ) {
		    unsigned char prestatus=FSEVENT_INODE_STATUS_REMOVED;
            	    struct call_info_struct call_info;

		    logoutput("evaluate_and_process_fsevent: (watch) entry %s removed", entry->name);

		    /* entry deleted (here) so adjust the filesystem */

		    prestatus=inode->status;

            	    init_call_info(&call_info, entry);

            	    res=determine_path(&call_info, NOTIFYFS_PATH_FORCE);
            	    if (res<0) return;

		    changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE);

		    /* forward message only when something really changed */

		    if ( prestatus!=inode->status && inode->status==FSEVENT_INODE_STATUS_REMOVED) {

			send_notify_message_clients(effective_watch, mask, 0, NULL, NULL, remote);

		    }

		}

	    } else {

		/* received an event on watch, but entry is not found.. */

		logoutput("evaluate_and_process_fsevent: (watch) entry not found for watch id %li", effective_watch->id);

	    }

	}

    }

}


/* INOTIFY BACKEND SPECIFIC CALLS */

/* read data from inotify fd */
#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUFF_LEN (1024 * (INOTIFY_EVENT_SIZE + 16))

void handle_data_on_inotify_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo)
{
    int nreturn=0;
    char outputstring[256];

    logoutput1("handle_data_on_inotify_fd.");

    if ( events & EPOLLIN ) {
        int lenread=0;
        char buff[INOTIFY_BUFF_LEN];

        lenread=read(epoll_xdata->fd, buff, INOTIFY_BUFF_LEN);

        if ( lenread<0 ) {

            logoutput0("Error (%i) reading inotify events (fd: %i).", errno, epoll_xdata->fd);

        } else {
            int i=0, res;
            struct inotify_event *i_event;
            struct effective_watch_struct *effective_watch;

            while(i<lenread) {

                i_event = (struct inotify_event *) &buff[i];

                /* handle overflow here */

                if ( (i_event->mask & IN_Q_OVERFLOW) && i_event->wd==-1 ) {

                    /* what to do here: read again?? go back ??*/

                    logoutput0("Error reading inotify events: buffer overflow.");
                    goto next;

                }

                /* lookup watch using this wd */

                logoutput1("Received an inotify event on wd %i.", i_event->wd);

                memset(outputstring, '\0', 256);
                res=print_mask(i_event->mask, outputstring, 256);

                if ( res>0 ) {

                    logoutput2("Mask: %i/%s", i_event->mask, outputstring);

                } else {

                    logoutput2("Mask: %i", i_event->mask);

                }

                effective_watch=lookup_watch(FSEVENT_BACKEND_METHOD_INOTIFY, i_event->wd);

		/* process the event.... is from inotify, so no stat and it's local*/

		evaluate_and_process_fsevent(effective_watch, i_event->name, i_event->len, i_event->mask, NULL, FSEVENT_BACKEND_METHOD_INOTIFY);

		next:

                i += INOTIFY_EVENT_SIZE + i_event->len;

            }

        }

    }

}




void handle_client_message(struct client_struct *client,  struct notifyfs_client_message *client_message, void *data1, int len1)
{
    unsigned char type=client_message->type;
    char *path=(char *) data1;

    logoutput("handle client message, for message %i, client pid/type %i/%i", type, client->pid, client->type);

    /* check the pid and the tid the client has send, and the tid earlier detected is a task of the mainpid */

    if ( belongtosameprocess(client_message->pid, client_message->tid)==0 ) {

	logoutput("handle client message: pid %i and tid %i send by client (%i) are not part of same process", client_message->pid, client_message->tid, client->tid);

	/* ignore message.. */

	return;

    }

    if ( belongtosameprocess(client_message->pid, client->tid)==0 ) {

	logoutput("handle client message: pid %i send by client and tid %i are not part of same process", client_message->pid, client->tid);

	/* ignore message.. */

	return;

    }

    client->pid=client_message->pid;

    if ( type&(NOTIFYFS_MESSAGE_CLIENT_REGISTERAPP|NOTIFYFS_MESSAGE_CLIENT_REGISTERFS) ) {

	/* received a register client fs message */

	if ( type&NOTIFYFS_MESSAGE_CLIENT_REGISTERFS ) {
	    unsigned char newclientfs=0;

	    /* register client as fs */

	    if ( ! (client->type&NOTIFYFS_CLIENTTYPE_FS) ) {

		newclientfs=1;

		client->type|=NOTIFYFS_CLIENTTYPE_FS;

	    }

	    if ( newclientfs==1 ) {

		/* path should be an argument */

		if ( client->path ) {

		    if ( path ) {

			logoutput("handle client message: client has already path set(%s) but replacing by %s", client->path, path);
			free(client->path);
			client->path=NULL;

		    }

		}

		if ( ! client->path ) {

		    if ( path ) {

			/* use malloc here instead?? */

			client->path=strdup(path);

		    }

		    if ( client->path ) {

			logoutput("handle_client_message: read path (len: %i) %s", strlen(client->path), client->path);

			client->status_fs=NOTIFYFS_CLIENTSTATUS_UP;
			client->mount_entry=NULL;

			find_and_assign_mount_to_clientfs(client);

		    }

		}

	    }

	}

	if ( type&NOTIFYFS_MESSAGE_CLIENT_REGISTERAPP ) {

	    /* register client as app */

	    client->type|=NOTIFYFS_CLIENTTYPE_APP;
	    client->status_app=NOTIFYFS_CLIENTSTATUS_UP;

	}

	if ( client->type&NOTIFYFS_CLIENTTYPE_FS ) {

	    logoutput("client %i is a client fs", client->pid);

	}

	if ( client->type&NOTIFYFS_CLIENTTYPE_APP ) {

	    logoutput1("client %i is a client app", client->pid);

	}

	/* use the mask the client has send as messagemask */

	client->messagemask=client_message->messagemask;


    }

    if ( type&NOTIFYFS_MESSAGE_CLIENT_SIGNOFF ) {

	/* signoff client */

	logoutput("handle client message, signoff client pid %i", client->pid);

	remove_client(client);


    }

}

void handle_fsevent_message(struct client_struct *client, struct notifyfs_fsevent_message *fsevent_message, void *data1, int len1)
{
    unsigned char type=fsevent_message->type;

    if ( type==NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYPATH ) {

	logoutput("handle_fsevent_message: setwatch_bypath");

	/* here read the data, it must be complete:
	   - path and mask 
	   then set the watch at backend
	*/


    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYINO ) {

	logoutput("handle_fsevent_message: setwatch_byino");

	/* here read the data, it must be complete:
	   - ino and mask 
	   then set the watch at backend
	*/

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_NOTIFY ) {
	struct effective_watch_struct *effective_watch;

	logoutput("handle_fsevent_message: notify");

	/* read the data from the client fs 
	   and pass it through to client apps 
	   but first filter it out as it maybe an event caused by this fs
	   and because it comes through a message it's an event on the backend
	   howto determine....
	   it's a fact that inotify events have been realised on the VFS,
	   with events on the backend this is not so
	   but first filter out the events caused by this host....*/

	/* lookup the watch the event is about using the backend_id */

	/* there must be a client interested */

	effective_watch=lookup_watch(FSEVENT_BACKEND_METHOD_FORWARD, fsevent_message->id);

	if ( effective_watch ) {

	    logoutput("handle_fsevent_message: watch found.");

	    if ( fsevent_message->statset==1 ) {

		evaluate_and_process_fsevent(effective_watch, (char *) data1, len1, fsevent_message->mask, &(fsevent_message->st), FSEVENT_BACKEND_METHOD_FORWARD);

	    } else {

		evaluate_and_process_fsevent(effective_watch, (char *) data1, len1, fsevent_message->mask, NULL, FSEVENT_BACKEND_METHOD_FORWARD);

	    }

	} else {

	    logoutput("handle_fsevent_message: watch not found for id %li.", fsevent_message->id);

	}


    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_DELWATCH ) {

	logoutput("handle_fsevent_message: delwatch");

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_SLEEPWATCH ) {

	logoutput("handle_fsevent_message: sleepwatch");

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_WAKEWATCH ) {

	logoutput("handle_fsevent_message: wakewatch");

    } else {

	logoutput("handle_fsevent_message: unknown message");

    }

}

void handle_reply_message(struct client_struct *client,  struct notifyfs_reply_message *reply_message, char *path)
{
    unsigned char type=reply_message->type;

    if ( type==NOTIFYFS_MESSAGE_REPLY_OK ) {

	logoutput("handle_reply_message: reply_ok");

    } else if ( type==NOTIFYFS_MESSAGE_REPLY_ERROR ) {

	logoutput("handle_reply_message: reply_error");

    } else if ( type==NOTIFYFS_MESSAGE_REPLY_REPLACE ) {

	logoutput("handle_reply_message: reply_error");

    } else {

	logoutput("handle_reply_message: unknown message");

    }

}

void handle_mountinfo_request(struct client_struct *client,  struct notifyfs_mount_message *mount_message, void *data1, void *data2)
{
    int res=0;
    struct mount_entry_struct *mount_entry;
    char *path=(char *) data1;

    if ( strlen(mount_message->fstype)>0 ) {

	logoutput("handle_mountinfo_request, for fstype %s", mount_message->fstype);

    } else {

	logoutput("handle_mountinfo_request, no fs filter");

    }

    if ( path && strlen(path)>0 ) {

	logoutput("handle_mountinfo_request, a subdirectory of %s", path);

    } else {

	logoutput("handle_mountinfo_request, no path filter");

    }


    res=lock_mountlist();

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_CURRENT);

    while (mount_entry) {


	if ( strlen(mount_message->fstype)>0 ) {

	    logoutput("handle_mountinfo_request, compare %s with %s", mount_entry->fstype, mount_message->fstype);

	    /* filter on filesystem */

	    if ( strcmp(mount_message->fstype, mount_entry->fstype)!=0 ) goto next;

	}

	if ( path && strlen(path)>0 ) {

	    logoutput("handle_mountinfo_request, compare %s with %s", mount_entry->mountpoint, path);

	    /* filter on directory */

	    if ( issubdirectory(mount_entry->mountpoint, path, 1)==0 ) goto next;

	}

	res=send_mount_message(client->fd, mount_entry, mount_message->unique);

	next:

	mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_CURRENT);

    }

    res=unlock_mountlist();

    /* a reply message to terminate */

    res=reply_message(client->fd, mount_message->unique, 0);

}



static struct fuse_lowlevel_ops notifyfs_oper = {
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
    struct fuse_args notifyfs_fuse_args = FUSE_ARGS_INIT(0, NULL);
    struct fuse_session *notifyfs_session;
    int res, epoll_fd, socket_fd, inotify_fd, mountinfo_fd, networksocket_fd;
    struct stat st;
    pthread_t threadid_mountmonitor=0;
    struct epoll_extended_data_struct *epoll_xdata=NULL;
    struct epoll_extended_data_struct xdata_inotify={0, 0, NULL, NULL, NULL, NULL};
    struct epoll_extended_data_struct xdata_socket={0, 0, NULL, NULL, NULL, NULL};
    struct epoll_extended_data_struct xdata_mountinfo={0, 0, NULL, NULL, NULL, NULL};
    struct epoll_extended_data_struct xdata_network={0, 0, NULL, NULL, NULL, NULL};


    umask(0);

    // set logging

    openlog("fuse.notifyfs", 0,0); 

    /* parse commandline options and initialize the fuse options */

    res=parse_arguments(argc, argv, &notifyfs_fuse_args);

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

    //
    // create the root inode and entry
    //
    res=create_root();

    if ( res<0 ) {

	fprintf(stderr, "Error, failed to create the root entry(error: %i).\n", res);
	exit(1);

    }

    //
    // set default options
    //

    logoutput("main: taking accessmode %i", notifyfs_options.accessmode);
    logoutput("main: taking filesystems %i", notifyfs_options.filesystems);
    logoutput("main: taking testmode %i", notifyfs_options.testmode);

    loglevel=notifyfs_options.logging;
    logarea=notifyfs_options.logarea;

    notifyfs_options.attr_timeout=1.0;
    notifyfs_options.entry_timeout=1.0;
    notifyfs_options.negative_timeout=1.0;

    if ( (notifyfs_chan = fuse_mount(notifyfs_options.mountpoint, &notifyfs_fuse_args)) == NULL) {

        logoutput("Error mounting and setting up a channel.");
        goto out;

    }

    notifyfs_session=fuse_lowlevel_new(&notifyfs_fuse_args, &notifyfs_oper, sizeof(notifyfs_oper), NULL);

    if ( notifyfs_session == NULL ) {

        logoutput("Error starting a new session.");
        goto out;

    }

    res = fuse_daemonize(0);

    if ( res!=0 ) {

        logoutput("Error daemonize.");
        goto out;

    }

    fuse_session_add_chan(notifyfs_session, notifyfs_chan);

    epoll_fd=init_mainloop();

    if ( epoll_fd<=0 ) {

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

    /* add socket to epoll for reading */

    epoll_xdata=add_to_epoll(socket_fd, EPOLLIN, TYPE_FD_SOCKET, &handle_data_on_socket_fd, NULL, &xdata_socket);

    if ( ! epoll_xdata ) {

        logoutput("Error adding socket fd to mainloop.");
        goto out;

    } else {

        logoutput("socket fd %i added to epoll", socket_fd);

	add_xdata_to_list(epoll_xdata);

    }

    notifyfs_options.socket_fd=socket_fd;

    /* assign the message callbacks */

    assign_message_callback_server(NOTIFYFS_MESSAGE_TYPE_CLIENT, &handle_client_message);
    assign_message_callback_server(NOTIFYFS_MESSAGE_TYPE_FSEVENT, &handle_fsevent_message);
    assign_message_callback_server(NOTIFYFS_MESSAGE_TYPE_REPLY, &handle_reply_message);
    assign_message_callback_server(NOTIFYFS_MESSAGE_TYPE_MOUNTINFO_REQ, &handle_mountinfo_request);

    /* listen to the network */

    if ( notifyfs_options.listennetwork==1 ) {

	networksocket_fd=create_networksocket(notifyfs_options.networkport);

	if ( networksocket_fd<=0 ) {

    	    logoutput("Error creating networksocket fd: %i.", networksocket_fd);
    	    goto out;

	}

	/* add socket to epoll for reading */

	epoll_xdata=add_to_epoll(socket_fd, EPOLLIN, TYPE_FD_SOCKET, &handle_data_on_networksocket_fd, NULL, &xdata_network);

	if ( ! epoll_xdata ) {

    	    logoutput("Error adding networksocket fd to mainloop.");
    	    goto out;

	} else {

    	    logoutput("socket fd %i added to epoll", networksocket_fd);

	    add_xdata_to_list(epoll_xdata);

	}

	notifyfs_options.networksocket_fd=networksocket_fd;

    }


    /*
    *    add a inotify instance to epoll : default backend 
    *
    */

    /* create the inotify instance */

    inotify_fd=inotify_init();

    if ( inotify_fd<=0 ) {

        logoutput("Error creating inotify fd: %i.", errno);
        goto out;

    }

    /* add inotify to epoll */

    epoll_xdata=add_to_epoll(inotify_fd, EPOLLIN | EPOLLPRI, TYPE_FD_INOTIFY, &handle_data_on_inotify_fd, NULL, &xdata_inotify);

    if ( ! epoll_xdata ) {

        logoutput("error adding inotify fd to mainloop.");
        goto out;

    } else {

        logoutput("inotify fd %i added to epoll", inotify_fd);

	add_xdata_to_list(epoll_xdata);

    }

    notifyfs_options.inotify_fd=inotify_fd;


    /* 
    *     add mount info 
    */

    mountinfo_fd=open(MOUNTINFO_FILE, O_RDONLY);

    if ( mountinfo_fd==-1 ) {

        logoutput("unable to open file %s", MOUNTINFO_FILE);
        goto out;

    }

    epoll_xdata=add_to_epoll(mountinfo_fd, EPOLLERR, TYPE_FD_INOTIFY, &handle_data_on_mountinfo_fd, NULL, &xdata_mountinfo);

    if ( ! epoll_xdata ) {

        logoutput("error adding mountinfo fd to mainloop");
        goto out;

    } else {

        logoutput("mountinfo fd %i added to epoll", mountinfo_fd);

	add_xdata_to_list(epoll_xdata);

    }

    res=start_mountmonitor(&threadid_mountmonitor, &update_notifyfs);

    if ( res<0 ) {

        logoutput("error (%i) starting mountmonitor thread", res);

    } else {

        logoutput("thread for mountmonitor started");

    }


    /* signal the mountmonitor to do the initial reading of the mounttable */

    signal_mountmonitor(1);

    /* if configured add a network socket */

    // if ( notifyfs_options.listennetwork==1 ) {


    /* add the fuse channel(=fd) to the mainloop */

    res=addfusechannelstomainloop(notifyfs_session, notifyfs_options.mountpoint);

    /* handle error here */

    res=startfusethreads();

    /* handle error again */

    res=epoll_mainloop();

    out:

    terminatefuse(NULL);

    if ( xdata_inotify.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_inotify, 0);
	close(xdata_inotify.fd);
	xdata_inotify.fd=0;
	notifyfs_options.inotify_fd=0;
	remove_xdata_from_list(&xdata_inotify);

    }

    if ( xdata_network.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_network, 0);
	close(xdata_network.fd);
	xdata_network.fd=0;
	notifyfs_options.networksocket_fd=0;
	remove_xdata_from_list(&xdata_network);

    }

    if ( xdata_socket.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_socket, 0);
	close(xdata_socket.fd);
	xdata_socket.fd=0;
	notifyfs_options.socket_fd=0;
	remove_xdata_from_list(&xdata_socket);

    }

    unlink(notifyfs_options.socket);

    if ( xdata_mountinfo.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_mountinfo, 0);
	close(xdata_mountinfo.fd);
	xdata_mountinfo.fd=0;
	remove_xdata_from_list(&xdata_mountinfo);

    }

    /* remove any remaining xdata from mainloop */

    destroy_mainloop();

    if ( threadid_mountmonitor ) pthread_cancel(threadid_mountmonitor);

    fuse_opt_free_args(&notifyfs_fuse_args);

    skipeverything:

    closelog();

    return res ? 1 : 0;

}
