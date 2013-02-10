/*

  2010, 2011, 2012 Stef Bon <stefbon@gmail.com>

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

#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>

#include <fuse/fuse_lowlevel.h>

#include "entry-management.h"
#include "path-resolution.h"
#include "logging.h"
#include "notifyfs.h"
#include "notifyfs-io.h"
#include "utils.h"
#include "message.h"
#include "client-io.h"
#include "client.h"
#include "watches.h"
#include "mountinfo.h"
#include "epoll-utils.h"
#include "message-server.h"
#include "path-resolution.h"
#include "socket.h"

#include "workerthreads.h"
#include "networkutils.h"

#include "changestate.h"

#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM
#define MAXIMUM_PROCESS_FSEVENTS_NRTHREADS	4

extern struct notifyfs_options_struct notifyfs_options;
extern struct fuse_chan *notifyfs_chan;

static struct workerthreads_queue_struct *global_workerthreads_queue=NULL;

struct fsevents_queue_struct {
    struct notifyfs_fsevent_struct *first;
    struct notifyfs_fsevent_struct *last;
    pthread_mutex_t mutex;
    int nrthreads;
};

/* initialize the main queue for fsevents */

struct fsevents_queue_struct main_fsevents_queue={NULL, NULL, PTHREAD_MUTEX_INITIALIZER, 0};

static int check_pathlen(struct notifyfs_entry_struct *entry)
{
    int len=1;
    struct notifyfs_inode_struct *inode;
    char *name;

    while(entry) {

	inode=get_inode(entry->inode);

	/* check the inode status: if pending to be removed or already removed then ready */

	if (inode->status==FSEVENT_INODE_STATUS_TOBEREMOVED || inode->status==FSEVENT_INODE_STATUS_REMOVED ) {

	    /* if a part of the path is removed already or to be removed assume it's already in progress */

	    len=-EINPROGRESS;
	    break;

	} else if (entry->parent>=0 && (inode->status==FSEVENT_INODE_STATUS_TOBEUNMOUNTED || inode->status==FSEVENT_INODE_STATUS_UNMOUNTED)) {

	    /* if a parent is (to be) unmounted then assume it's already in  progress 
		TEST THIS !!! it's possible that events are mixed up like:
		unmount, then direct a create of a new file in this directory
		this event may not be ignored !! */

	    len=-EINPROGRESS;
	    break;

	}

	/* add the lenght of entry->name plus a slash for every name */

	name=get_data(entry->name);

	len+=strlen(name)+1;

	entry=get_entry(entry->parent);

	if (isrootentry(entry)==1) break;

    }

    return len;

}

static int determine_path_custom(struct notifyfs_fsevent_struct *fsevent)
{
    struct notifyfs_entry_struct *entry=fsevent->entry;
    char *pos;
    int nreturn=0, len;
    char *name;
    char *path;

    if (isrootentry(entry)==1) {

	nreturn=3;

    } else {

	nreturn=check_pathlen(entry);
	if (nreturn<0) goto error;

    }

    fsevent->path=malloc(nreturn);

    if ( ! fsevent->path) {

	nreturn=-ENOMEM;
	goto error;

    }

    fsevent->pathallocated=1;

    pos=fsevent->path+nreturn-1;
    *pos='\0';

    while (entry) {

	name=get_data(entry->name);

	len=strlen(name);
	pos-=len;

	memcpy(pos, name, len);

	pos--;
	*pos='/';

	entry=get_entry(entry->parent);

	if (isrootentry(entry)==1) break;

    }

    return nreturn;

    error:

    if (fsevent->path) {

	free(fsevent->path);
	fsevent->path=NULL;
	fsevent->pathallocated=0;

    }

    return nreturn;

}

/* check an entry is in the view
    in case this is a view part of a clientwatch, it's possible that the name boundaries are not set yet ...
    normally when a watch is set, it's part of a monitor message
    after this the client will get the contents of the directory 
    and then it will "know" the boundaries, ie the first and the last name
    untill then these names are not set
*/

static unsigned char entry_in_view(struct notifyfs_entry_struct *entry, struct view_struct *view)
{

    if (!view) return 1;

    if (view->order==NOTIFYFS_INDEX_TYPE_NAME) {

	if (view->first_entry>=0) {
	    struct notifyfs_entry_struct *first_entry=get_entry(view->first_entry);

	    if (entry->nameindex_value<first_entry->nameindex_value) {

		return 0;

	    } else if (entry->nameindex_value==first_entry->nameindex_value) {
		char *name=get_data(entry->name);
		char *first_name=get_data(first_entry->name);

		if (strcmp(first_name, name)>0) return 0;

	    }

	}

	if (view->last_entry>=0) {
	    struct notifyfs_entry_struct *last_entry=get_entry(view->last_entry);

	    if (entry->nameindex_value>last_entry->nameindex_value) {

		return 0;

	    } else if (entry->nameindex_value==last_entry->nameindex_value) {
		char *name=get_data(entry->name);
		char *last_name=get_data(last_entry->name);

		if (strcmp(last_name, name)<0) return 0;

	    }

	}

    }

    return 1;

}

static unsigned char check_fsevent_applies(struct fseventmask_struct *fseventmaska, struct fseventmask_struct *fseventmaskb)
{

    if (fseventmaska->attrib_event & fseventmaskb->attrib_event) {

	return 1;

    } else if (fseventmaska->xattr_event & fseventmaskb->xattr_event) {

	return 1;

    } else if (fseventmaska->file_event & fseventmaskb->file_event) {

	return 1;

    } else if (fseventmaska->move_event & fseventmaskb->move_event) {

	return 1;

    } else if (fseventmaska->fs_event & fseventmaskb->fs_event) {

	return 1;

    }

    return 0;

}

static void send_fsevent_to_clients(struct watch_struct *watch, struct notifyfs_fsevent_struct *fsevent)
{
    struct clientwatch_struct *clientwatch=NULL;

    pthread_mutex_lock(&watch->mutex);

    clientwatch=watch->clientwatches;

    while (clientwatch) {

	if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
	    struct client_struct *client;

	    client=clientwatch->notifyfs_owner.owner.localclient;

    	    if ( client ) {

		logoutput("send_fsevent_to_clients: test client %i is up", (int) client->pid);

        	if ( client->status==NOTIFYFS_CLIENTSTATUS_UP ) {

		    logoutput("send_fsevent_to_clients: test client watch of %i applies", (int) client->pid);

		    /* test here the client is interested in the event */

		    if (check_fsevent_applies(&clientwatch->fseventmask, &fsevent->fseventmask)==1) {
			struct view_struct *view=get_view(clientwatch->view);

			if (entry_in_view(fsevent->entry, view)==1) {
			    struct notifyfs_connection_struct *connection=client->connection;

			    logoutput("send_fsevent_to_clients: entry part of view, check for connection");

			    if (connection) {
    				uint64_t unique=new_uniquectr();

				/* send message */

				send_fsevent_message(connection->fd, unique, clientwatch->owner_watch_id, &fsevent->fseventmask, fsevent->entry->index, &fsevent->detect_time);

			    }

			}

		    }

		}

	    }

	} else if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
	    struct notifyfs_server_struct *server;

	    server=clientwatch->notifyfs_owner.owner.remoteserver;

	    /* when the watch is set/owned by a remote server: always send a message, don't test it's in a view etcetera
	    */

	    if (server) {

		if (server->status==NOTIFYFS_SERVERSTATUS_UP) {
		    struct notifyfs_connection_struct *connection=server->connection;

		    logoutput("send_fsevent_to_clients: remote server up, check for connection");

		    if (connection) {
    			uint64_t unique=new_uniquectr();

			/* send message */

			send_fsevent_message(connection->fd, unique, clientwatch->owner_watch_id, &fsevent->fseventmask, fsevent->entry->index, &fsevent->detect_time);

		    }

		}

	    }

	}

	clientwatch=clientwatch->next_per_watch;

    }

    pthread_mutex_unlock(&watch->mutex);

}

/*
    function which looks for watches which have to be notified when fsevent occurs
    it's possible that there is a watch set on the fsevent->entry, but also
    on the parent of that one
*/

static void check_for_watches(struct notifyfs_fsevent_struct *fsevent)
{

    if (fsevent->entry) {
	struct notifyfs_inode_struct *inode=get_inode(fsevent->entry->inode);
	struct watch_struct *watch=lookup_watch(inode);

	if (watch) send_fsevent_to_clients(watch, fsevent);

	if (isrootentry(fsevent->entry)==0) {
	    struct notifyfs_entry_struct *entry;

	    entry=get_entry(fsevent->entry->parent);
	    inode=get_inode(entry->inode);

	    watch=lookup_watch(inode);

	    if (watch) send_fsevent_to_clients(watch, fsevent);


	}

    }

}

static void remove_fsevent_from_queue(struct notifyfs_fsevent_struct *fsevent)
{
    /* remove the previous event from queue */

    if (main_fsevents_queue.first==fsevent) main_fsevents_queue.first=fsevent->next;

    if (fsevent->prev) fsevent->prev->next=fsevent->next;
    if (fsevent->next) fsevent->next->prev=fsevent->prev;

    if (main_fsevents_queue.last==fsevent) main_fsevents_queue.last=fsevent->prev;

}

/* function to remove every clientwatch attached to a watch 
   watch must be locked
   when a watch is removed, a message is send to the client/server owning the watch
*/

static void remove_clientwatches_message(struct watch_struct *watch)
{
    struct clientwatch_struct *clientwatch, *next_clientwatch;
    struct client_struct *client=NULL;

    pthread_mutex_lock(&watch->mutex);

    clientwatch=watch->clientwatches;

    while (clientwatch) {

	next_clientwatch=clientwatch->next_per_watch;

	/* remove from owner (client or server) (and send a message to the owner) */

	remove_clientwatch_from_owner(clientwatch, 1);


	clientwatch->next_per_watch=NULL;
	clientwatch->prev_per_watch=NULL;
        free(clientwatch);

	watch->nrwatches--;

        clientwatch=next_clientwatch;

    }

    pthread_mutex_unlock(&watch->mutex);

}

static void change_clientwatches(struct watch_struct *watch, unsigned char action)
{
    struct clientwatch_struct *clientwatch;

    pthread_mutex_lock(&watch->mutex);

    clientwatch=watch->clientwatches;

    while (clientwatch) {

	if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
	    struct client_struct *client=NULL;

	    client=clientwatch->notifyfs_owner.owner.localclient;

    	    if ( client ) {

        	if ( client->status==NOTIFYFS_CLIENTSTATUS_UP ) {
		    struct notifyfs_connection_struct *connection=client->connection;

		    if (connection) {
			uint64_t unique=new_uniquectr();

			send_changewatch_message(connection->fd, unique, clientwatch->owner_watch_id, action);

		    }

		}

	    }

	} else 	if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
	    struct notifyfs_server_struct *server=NULL;

	    server=clientwatch->notifyfs_owner.owner.remoteserver;

    	    if ( server ) {

        	if ( server->status==NOTIFYFS_SERVERSTATUS_UP ) {
		    struct notifyfs_connection_struct *connection=server->connection;

		    if (connection) {
			uint64_t unique=new_uniquectr();

			send_changewatch_message(connection->fd, unique, clientwatch->owner_watch_id, action);

		    }

		}

	    }
	}

        clientwatch=clientwatch->next_per_watch;

    }

    pthread_mutex_unlock(&watch->mutex);

}

/* process a change recursive 


*/

static void process_changestate_fsevent_recursive(struct notifyfs_entry_struct *parent, unsigned char self, unsigned char action, char *path)
{
    int res, nlen;
    struct notifyfs_entry_struct *entry, *next_entry;
    struct notifyfs_inode_struct *inode;

    logoutput("process_remove_fsevent_recursive");

    inode=get_inode(parent->inode);
    nlen=strlen(path);

    if (inode) {
	struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	if (self==1) {

	    if (action==FSEVENT_INODE_ACTION_REMOVE) {

		inode->status=FSEVENT_INODE_STATUS_TOBEREMOVED;

	    } else if (action==FSEVENT_INODE_ACTION_SLEEP) {

		inode->status=FSEVENT_INODE_STATUS_SLEEP;

	    } else if (action==FSEVENT_INODE_ACTION_WAKEUP) {

		inode->status=FSEVENT_INODE_STATUS_OK;

	    }

	}

	if (attr) {

	    if (S_ISDIR(attr->cached_st.st_mode)) {
		char *name=NULL;
		int lenname=0;

		/* when a directory: first remove contents (recursive) */

		entry=get_next_entry(parent, NULL);

		while(entry) {

		    next_entry=get_next_entry(parent, entry);

		    name=get_data(entry->name);
		    lenname=strlen(name);

		    *(path+nlen)='/';
		    memcpy(path+nlen+1, name, lenname);
		    *(path+nlen+lenname)='\0';

    		    process_changestate_fsevent_recursive(entry, 1, action, path);

		    *(path+nlen)='\0';

		    entry=next_entry;

		}

	    }

	}

	if (self==1) {
	    struct watch_struct *watch=lookup_watch(inode);

	    if (watch) {

		lock_watch(watch);

		if (action==FSEVENT_INODE_ACTION_REMOVE) {

		    /* here remove the clientwatches and send all the clientwatches a del_watch message */

		    remove_clientwatches_message(watch);
		    remove_watch_backend_os_specific(watch);

		} else if (action==FSEVENT_INODE_ACTION_SLEEP) {

		    change_clientwatches(watch, FSEVENT_INODE_ACTION_SLEEP);
		    remove_watch_backend_os_specific(watch);

		} else if (action==FSEVENT_INODE_ACTION_WAKEUP) {

		    change_clientwatches(watch, FSEVENT_INODE_ACTION_WAKEUP);
		    set_watch_backend_os_specific(watch, path);

		}

		unlock_watch(watch);

		if (action==FSEVENT_INODE_ACTION_REMOVE) {

		    remove_watch_from_list(watch);
		    remove_watch_from_table(watch);

		    pthread_mutex_destroy(&watch->mutex);
		    pthread_cond_destroy(&watch->cond);

		    free(watch);

		}

	    }

	    if (action==FSEVENT_INODE_ACTION_REMOVE) {

		inode->status=FSEVENT_INODE_STATUS_REMOVED;

	    }

	}

    }

    if (action==FSEVENT_INODE_ACTION_REMOVE) {

	if (self==1) {

	    notify_kernel_delete(notifyfs_chan, parent);
	    remove_entry_from_name_hash(parent);
	    remove_entry(parent);

	}

    }

}

static void process_one_fsevent(struct notifyfs_fsevent_struct *fsevent)
{
    unsigned char catched=0;

    logoutput("process_one_fsevent");

    /* do the actual action */

    if (fsevent->fseventmask.fs_event & (NOTIFYFS_FSEVENT_FS_UNMOUNT|NOTIFYFS_FSEVENT_FS_MOUNT)) {

	/* an mount or a unmount */

	if (fsevent->fseventmask.fs_event & NOTIFYFS_FSEVENT_FS_UNMOUNT) {
	    struct mount_entry_struct *mount_entry=NULL;
	    struct notifyfs_entry_struct *entry;

	    catched=1;

	    mount_entry=fsevent->mount_entry;
	    entry=(struct notifyfs_entry_struct *) mount_entry->entry;

	    /* here additional??: when entry not set here, determine it */

	    if (entry) {

		/* notifyfs mount */

		if (entry->mount>=0) {
		    struct notifyfs_mount_struct *mount=get_mount(entry->mount);

		    /* TODO: handle sleeping mount */

		    if (mount_entry->status==MOUNT_STATUS_REMOVE) {

			logoutput("process_one_fsevent: disable mount");

			mount->status=NOTIFYFS_MOUNTSTATUS_DOWN;
			mount->entry=-1;

			unset_mount_backend(mount);

			remove_mount(mount);
			entry->mount=-1;

		    } else if (mount_entry->status==MOUNT_STATUS_SLEEP) {

			logoutput("process_one_fsevent: sleep mount");

			mount->status=NOTIFYFS_MOUNTSTATUS_SLEEP;

		    }

		} else {

		    logoutput("process_one_fsevent: entry has no mount??");

		}

		/* correct the fs and send messages in tree */

		if (mount_entry->status==MOUNT_STATUS_REMOVE) {
		    pathstring path;

		    strcpy(path, mount_entry->mountpoint);

		    /* remove the contents of the directory (but not the directory self), 
                       and send messages to clients when watches are also removed */

		    process_changestate_fsevent_recursive(entry, 0, FSEVENT_INODE_ACTION_REMOVE, path);

		} else if (mount_entry->status==MOUNT_STATUS_SLEEP) {
		    pathstring path;

		    strcpy(path, mount_entry->mountpoint);

		    /* set the whole tree (including watches) to sleep */

		    process_changestate_fsevent_recursive(entry, 0, FSEVENT_INODE_ACTION_SLEEP, path);

		}

		/* here additional: adjust the attributes of the entry since an umount affects these 
		maybe store it in mount_entry?? */

		/* lookup watch ... this entry and parent */

		check_for_watches(fsevent);

		/* here send a message to the client anyway... */

		/* configurable ??? can a client set somewhere an option it wants to receive always mount events */

		/* here also send a message to a local process or remote server when dealing with a special fs like nfs and cifs */ 

	    }

	} else if (fsevent->fseventmask.fs_event & NOTIFYFS_FSEVENT_FS_MOUNT) {
	    struct mount_entry_struct *mount_entry=NULL;
	    struct notifyfs_entry_struct *entry=NULL;
	    int parent_mount=-1;

	    catched=1;

	    mount_entry=fsevent->mount_entry;

	    if (is_rootmount(mount_entry)==1) {

		entry=get_rootentry();

	    } else {
		struct call_info_struct call_info;

		init_call_info(&call_info, NULL);

		call_info.path=mount_entry->mountpoint;

		create_notifyfs_path(&call_info, NULL);

		entry=call_info.entry;
		parent_mount=call_info.mount;

	    }

	    if (entry) {
		char *name=get_data(entry->name);

		logoutput("process_one_fsevent: got entry %s (index: %i), entry mountindex %i path %s", name, entry->index, entry->mount, mount_entry->mountpoint);

		fsevent->entry=entry;

		mount_entry->entry=(void *) entry;
		mount_entry->status=MOUNT_STATUS_UP;

		if (mount_entry->autofs_mounted && mount_entry->remount==1) {
		    pathstring path;

		    strcpy(path, mount_entry->mountpoint);

		    /* set the whole tree (including watches) to sleep */

		    process_changestate_fsevent_recursive(entry, 0, FSEVENT_INODE_ACTION_WAKEUP, path);

		}

		if (entry->mount>=0) {
		    struct notifyfs_mount_struct *mount=get_mount(entry->mount);

		    /* TODO: compare this existing mount and the new mount */

		    mount->status=NOTIFYFS_MOUNTSTATUS_UP;

		    set_mount_backend(mount);

		} else {
		    struct notifyfs_mount_struct *mount=create_mount(mount_entry->fstype, mount_entry->mountsource, mount_entry->superoptions, entry);

		    if (mount) {

			mount->major=mount_entry->major;
			mount->minor=mount_entry->minor;
			mount->mode=0;
			mount->status=NOTIFYFS_MOUNTSTATUS_UP;

			if (mount_entry->isbind==1) mount->mode|=NOTIFYFS_MOUNTMODE_ISBIND;
			if (mount_entry->isroot==1) mount->mode|=NOTIFYFS_MOUNTMODE_ISROOT;
			if (mount_entry->isautofs==1) mount->mode|=NOTIFYFS_MOUNTMODE_AUTOFS;
			if (mount_entry->autofs_indirect==1) mount->mode|=NOTIFYFS_MOUNTMODE_AUTOFS_INDIRECT;
			if (mount_entry->autofs_mounted==1) mount->mode|=NOTIFYFS_MOUNTMODE_AUTOFS_MOUNTED;
			if (mount_entry->remount==1) mount->mode|=NOTIFYFS_MOUNTMODE_REMOUNT;
			if (mount_entry->fstab==1) mount->mode|=NOTIFYFS_MOUNTMODE_FSTAB;

			set_mount_backend(mount);

		    }

		}

		check_for_watches(fsevent);

		/* here send a message to the client anyway... */

		/* configurable ??? can a client set somewhere an option it wants to receive always mount events */

		/* here also send a message to a local process or remote server when dealing with a special fs like nfs and cifs */ 

	    } else {

		mount_entry->status=MOUNT_STATUS_NOTSET;

	    }

	    /* here additional: adjust the attributes of the entry since an umount affects these 
		maybe store it in mount_entry?? */

	}

    } else if (fsevent->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED )) {
	struct notifyfs_inode_struct *inode;
	struct watch_struct *watch;
	pathstring path;

	strcpy(path, fsevent->path);

	catched=1;

	/* here test the watch(es) interested in this event and send eventually a message*/

	check_for_watches(fsevent);

	/* do this also for the parent */

	process_changestate_fsevent_recursive(fsevent->entry, 1, FSEVENT_INODE_ACTION_REMOVE, path);

    } else {

	logoutput("process_one_fsevent: handle %i:%i:%i:%i", fsevent->fseventmask.attrib_event, fsevent->fseventmask.xattr_event, fsevent->fseventmask.file_event, fsevent->fseventmask.move_event);

	if ((fsevent->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_CREATED | NOTIFYFS_FSEVENT_MOVE_MOVED_TO)) ||
	    (fsevent->fseventmask.attrib_event & NOTIFYFS_FSEVENT_ATTRIB_CA) || 
	    (fsevent->fseventmask.xattr_event & NOTIFYFS_FSEVENT_XATTR_CA) || 
	    (fsevent->fseventmask.file_event & (NOTIFYFS_FSEVENT_FILE_MODIFIED | NOTIFYFS_FSEVENT_FILE_SIZE | NOTIFYFS_FSEVENT_FILE_LOCK_CA))) {

	    catched=1;

	    /* creation as already done */

	    logoutput("process_one_fsevent: new or changed entry (created, moved to, file, meta)");

	    check_for_watches(fsevent);

	}

    }

    if (catched==0) logoutput("process_one_fsevent: event %i:%i:%i:%i not handled here", fsevent->fseventmask.attrib_event, fsevent->fseventmask.xattr_event, fsevent->fseventmask.file_event, fsevent->fseventmask.move_event);

}

/* function which is called by the workerthread to do the actual work 
    it basically looks for the first fsevent with status WAITING
    and process that futher
    when finished with that look for another waiting fsevent
    the queue is locked when reading and/or changing the status of the individual fsevents..
*/

static void process_fsevent(void *data)
{
    struct notifyfs_fsevent_struct *fsevent=NULL;
    struct timespec rightnow;

    logoutput("process_fsevent");

    fsevent=(struct notifyfs_fsevent_struct *) data;

    process:

    get_current_time(&rightnow);

    pthread_mutex_lock(&main_fsevents_queue.mutex);

    if (! fsevent) {

	/* get one from the queue: the first waiting */

	fsevent=main_fsevents_queue.first;

	while(fsevent) {

	    if (fsevent->status==NOTIFYFS_FSEVENT_STATUS_WAITING) {

		break;

	    } else if (fsevent->status==NOTIFYFS_FSEVENT_STATUS_DONE) {

		/* while walking in the queue remove old fsevents */

		/* events long ago are expired and of no use anymore: remove 
		make this period configurable ..*/

		if ( is_later(&rightnow, &fsevent->process_time, 5, 0)==1) {
		    struct notifyfs_fsevent_struct *next_fsevent=fsevent->next;

		    remove_fsevent_from_queue(fsevent);
		    destroy_notifyfs_fsevent(fsevent);

		    fsevent=next_fsevent;
		    continue;

		}

	    }

	    fsevent=fsevent->next;

	}

    }

    if (fsevent) {

	/* found one: change the status */

	fsevent->status=NOTIFYFS_FSEVENT_STATUS_PROCESSED;

    } else {

	main_fsevents_queue.nrthreads--;

    }

    pthread_mutex_unlock(&main_fsevents_queue.mutex);

    if (fsevent) {

	fsevent->process_time.tv_sec=rightnow.tv_sec;
	fsevent->process_time.tv_nsec=rightnow.tv_nsec;

	process_one_fsevent(fsevent);

	/* change the status to done 
	    is it really required to lock the whole queue for that ?? 
	*/

	pthread_mutex_lock(&main_fsevents_queue.mutex);
	fsevent->status=NOTIFYFS_FSEVENT_STATUS_DONE;
	pthread_mutex_unlock(&main_fsevents_queue.mutex);

	fsevent=NULL;

	/* jump back to look for another fsevent */

	goto process;

    }

}



/* function to test queueing a change state entry is necessary 

   note the queue has to be locked 
    TODO: take in account the type of the action, sleep versus remove, what to
    do when both in queue
   */

static unsigned char queue_required(struct notifyfs_fsevent_struct *fsevent)
{
    unsigned char doqueue=0;

    if ( ! main_fsevents_queue.first ) {

        /* queue is empty: put it on queue */

        doqueue=1;

	main_fsevents_queue.first=fsevent;
	main_fsevents_queue.last=fsevent;

	logoutput("queue_required: path %s, queue is empty", fsevent->path);

    } else {
	struct notifyfs_fsevent_struct *fsevent_walk=main_fsevents_queue.last;
        char *path2, *path1=fsevent->path;
        int len1=strlen(path1), len2;

	logoutput("queue_required: path %s, check the queue", fsevent->path);

        doqueue=1;

        /* walk through queue to check there is a related call already there */

        while(fsevent_walk) {

	    /* compare this previous event with the new one */

            path2=fsevent_walk->path;
            len2=strlen(path2);

            if ( len1>len2 ) {

                /* test path1 is a real subdirectory of path2 (the previous one)*/

                if ( strncmp(path1+len2, "/", 1)==0 && strncmp(path1, path2, len2)==0 ) {

		    if (fsevent_walk->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED )) {

			/* previous fsevent was a remove */
			/* ignore everything in subtree  */

			if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_WAITING || fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_PROCESSED) {

			    doqueue=0;
			    break;

			}

		    }

		}

	    } else if (len1==len2) {

		if (strcmp(fsevent_walk->path, fsevent->path)==0) {

		    /* paths are the same, but it may be another entry ... */

		    if (fsevent_walk->fseventmask.fs_event & NOTIFYFS_FSEVENT_FS_UNMOUNT) {

			if (fsevent->fseventmask.fs_event & NOTIFYFS_FSEVENT_FS_UNMOUNT) {

			    /* there is a previous umount event waiting: anything else here is ignored */

			    if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_WAITING) {

				merge_fseventmasks(&fsevent_walk->fseventmask, &fsevent->fseventmask);
				doqueue=0;
				break;

			    } else if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_DONE) {

				remove_fsevent_from_queue(fsevent_walk);

			    }

			}

		    } else if (fsevent_walk->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED )) {

			if (fsevent->entry==fsevent_walk->entry) {

			    /* ignore everything else on this entry here */

			    doqueue=0;
			    break;

			}

		    } else {

			if (fsevent->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED )) {

			    /* already deleted.. */

			    if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_WAITING) {

				remove_fsevent_from_queue(fsevent_walk);
				doqueue=0;
				break;

			    } else if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_DONE) {

				remove_fsevent_from_queue(fsevent_walk);

			    }

			} else {

			    /* any other event than a remove is safe to merge */

			    if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_WAITING) {

				merge_fseventmasks(&fsevent_walk->fseventmask, &fsevent->fseventmask);
				doqueue=0;
				break;

			    } else if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_DONE) {

				remove_fsevent_from_queue(fsevent_walk);

			    }

			}

		    }

		}

	    } else if (len1<len2) {

                /* test path2 is a real subdirectory of path1 */

                if ( strncmp(path2+len1, "/", 1)==0 && strncmp(path2, path1, len1)==0 ) {

		    /* path is removed and every thing in it */

		    if (fsevent->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED)) {

			if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_DONE) {

			    remove_fsevent_from_queue(fsevent_walk);

			} else if (fsevent_walk->status==NOTIFYFS_FSEVENT_STATUS_WAITING) {

			    /* previous event is not yet processed: replace it by the new one */

			    replace_fseventmask(&fsevent_walk->fseventmask, &fsevent->fseventmask);
			    doqueue=0;
			    break;

			}

		    }

		}

	    }

	    fsevent_walk=fsevent_walk->prev;

	}

	if (doqueue==1) {

	    /* queue/add at tail */

	    if (main_fsevents_queue.last) main_fsevents_queue.last->next=fsevent;
	    fsevent->next=NULL;
	    fsevent->prev=main_fsevents_queue.last;
	    main_fsevents_queue.last=fsevent;

	}

    }

    return doqueue;

}

/*  process an notifyfs fsevent

    when something is reported (by the fs notify backend like inotify) 
    or detected on the fly
    or by the mountmonitor (mount/unmount)
    or by the lockmonitor

    make this effective in the fs

*/

void queue_fsevent(struct notifyfs_fsevent_struct *notifyfs_fsevent)
{
    struct fseventmask_struct *fseventmask=&notifyfs_fsevent->fseventmask;
    int res;

    logoutput("queue_fsevent");

    /* in some cases the path is already set */

    if (! notifyfs_fsevent->path) {

	res=determine_path_custom(notifyfs_fsevent);

	if (res<0) {

	    if (res==-EINPROGRESS) {

		/* some upper directory is to be removed : ignore */

		return;

	    } else if (res==-ENOMEM) {

		logoutput("queue_fsevent: dropping fsevent due to memory allocation");

		return;

	    } else {

		logoutput("queue_fsevent: dropping fsevent due to unknown error %i", res);

		return;

	    }

	}

    }

    /* adjust the status of this inode in case of an remove or unmount */


    if (fseventmask->move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED)) {
	struct notifyfs_entry_struct *entry=notifyfs_fsevent->entry;
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	inode->status=FSEVENT_INODE_STATUS_TOBEREMOVED;

    }


    if (fseventmask->fs_event & NOTIFYFS_FSEVENT_FS_UNMOUNT) {
	struct notifyfs_entry_struct *entry=notifyfs_fsevent->entry;
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	inode->status=FSEVENT_INODE_STATUS_TOBEUNMOUNTED;

    }

    notifyfs_fsevent->status=NOTIFYFS_FSEVENT_STATUS_WAITING;

    /* lock the queue */

    pthread_mutex_lock(&main_fsevents_queue.mutex);

    res=queue_required(notifyfs_fsevent);

    pthread_mutex_unlock(&main_fsevents_queue.mutex);

    if (res==1) {

	logoutput("queue_fsevent: path: %s, fsevent queued", notifyfs_fsevent->path);

	/* possibly activate a workerthread */

	if (main_fsevents_queue.nrthreads<MAXIMUM_PROCESS_FSEVENTS_NRTHREADS) {
	    struct workerthread_struct *workerthread=NULL;

	    logoutput("queue_fsevent: path: %s, nr threads %i, starting a new thread", notifyfs_fsevent->path, main_fsevents_queue.nrthreads);

	    /* get a thread to do the work */

	    workerthread=get_workerthread(global_workerthreads_queue);

	    if ( workerthread ) {

		/* assign the right callbacks and data */

		workerthread->processevent_cb=process_fsevent;
		workerthread->data=NULL;

		pthread_mutex_lock(&main_fsevents_queue.mutex);
		main_fsevents_queue.nrthreads++;
		pthread_mutex_unlock(&main_fsevents_queue.mutex);

		logoutput("queue_fsevent: sending a signal to workerthread to start");

		/* send signal to start */

		signal_workerthread(workerthread);

	    }

	}

    } else {

	destroy_notifyfs_fsevent(notifyfs_fsevent);

    }

}

struct notifyfs_fsevent_struct *create_fsevent(struct notifyfs_entry_struct *entry)
{
    struct notifyfs_fsevent_struct *fsevent=NULL;

    fsevent=malloc(sizeof(struct notifyfs_fsevent_struct));

    if (fsevent) {

	init_notifyfs_fsevent(fsevent);
	fsevent->entry=entry;

    }

    return fsevent;

}

void init_changestate(struct workerthreads_queue_struct *workerthreads_queue)
{

    global_workerthreads_queue=workerthreads_queue;

}

