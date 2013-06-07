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

#include "logging.h"

#include "utils.h"
#include "simple-list.h"
#include "workerthreads.h"

#include "notifyfs-fsevent.h"
#include "notifyfs.h"
#include "notifyfs-io.h"

#include "mountinfo.h"
#include "filesystem.h"

#include "epoll-utils.h"
#include "socket.h"
#include "entry-management.h"
#include "path-resolution.h"

#include "backend.h"
#include "networkservers.h"

#include "message-base.h"
#include "message-send.h"

#include "client.h"
#include "watches.h"

#include "options.h"
#include "changestate.h"


#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM
#define MAXIMUM_PROCESS_FSEVENTS_NRTHREADS	4

extern struct notifyfs_options_struct notifyfs_options;
extern struct fuse_chan *notifyfs_chan;
extern struct simple_group_struct group_view;

static struct workerthreads_queue_struct *global_workerthreads_queue=NULL;
static char *rootpath="/";

struct fsevents_queue_struct {
    struct notifyfs_fsevent_struct *first;
    struct notifyfs_fsevent_struct *last;
    pthread_mutex_t mutex;
    int nrthreads;
};

struct fsevents_queue_struct main_fsevents_queue={NULL, NULL, PTHREAD_MUTEX_INITIALIZER, 0};

static void remove_fsevent_from_queue(struct notifyfs_fsevent_struct *fsevent)
{
    /* remove the previous event from queue */

    if (main_fsevents_queue.first==fsevent) main_fsevents_queue.first=fsevent->next;

    if (fsevent->prev) fsevent->prev->next=fsevent->next;
    if (fsevent->next) fsevent->next->prev=fsevent->prev;

    if (main_fsevents_queue.last==fsevent) main_fsevents_queue.last=fsevent->prev;

}

struct notifyfs_fsevent_struct *create_fsevent(struct notifyfs_entry_struct *entry)
{
    struct notifyfs_fsevent_struct *fsevent=NULL;

    fsevent=malloc(sizeof(struct notifyfs_fsevent_struct));

    if (fsevent) {

	init_notifyfs_fsevent(fsevent);
	fsevent->data=(void *)entry;

    }

    return fsevent;

}

void init_notifyfs_fsevent(struct notifyfs_fsevent_struct *fsevent)
{

    fsevent->status=0;

    fsevent->fseventmask.cache_event=0;
    fsevent->fseventmask.attrib_event=0;
    fsevent->fseventmask.xattr_event=0;
    fsevent->fseventmask.file_event=0;
    fsevent->fseventmask.move_event=0;
    fsevent->fseventmask.fs_event=0;

    fsevent->pathinfo.path=NULL;
    fsevent->pathinfo.flags=0;
    fsevent->pathinfo.len=0;

    fsevent->detect_time.tv_sec=0;
    fsevent->detect_time.tv_nsec=0;
    fsevent->process_time.tv_sec=0;
    fsevent->process_time.tv_nsec=0;

    fsevent->data=NULL;
    fsevent->flags=0;

    fsevent->next=NULL;
    fsevent->prev=NULL;

}

void destroy_notifyfs_fsevent(struct notifyfs_fsevent_struct *fsevent)
{

    free_path_pathinfo(&fsevent->pathinfo);
    free(fsevent);

}

unsigned char compare_attributes(struct stat *cached_st, struct stat *st, struct fseventmask_struct *fseventmask)
{
    unsigned char changed=0;

    /* compare mode, owner and group */

    if (cached_st->st_mode!=st->st_mode) {

	fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_MODE;
	cached_st->st_mode=st->st_mode;

	changed=1;

    }

    if (cached_st->st_uid!=st->st_uid) {

	fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_OWNER;
	cached_st->st_uid=st->st_uid;

	changed=1;

    }

    if (cached_st->st_gid!=st->st_gid) {

	fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_GROUP;
	cached_st->st_gid=st->st_gid;

	changed=1;

    }

    /* nlinks belongs to group MOVE */

    if (cached_st->st_nlink!=st->st_nlink) {

	fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_NLINKS;
	cached_st->st_nlink=st->st_nlink;

	changed=1;

    }

    /* size belongs to group FILE */

    if (cached_st->st_size!=st->st_size) {

	fseventmask->file_event|=NOTIFYFS_FSEVENT_FILE_SIZE;
	cached_st->st_size=st->st_size;

	changed=1;

    }

    /* not yet obvious what happened: xattr or contents changed in directory??

	with linux when adding or removing an entry, the timestamp st_mtime is changed of the parent directory

	when changing the xattr, the timestamp st_ctime is changed

	when using an utility like touch, the timestamps st_atime and possibly st_mtime is changed 
    */

    /* check the mtime */

    if (cached_st->st_mtim.tv_sec<st->st_mtim.tv_sec || (cached_st->st_mtim.tv_sec==st->st_mtim.tv_sec && cached_st->st_mtim.tv_nsec<st->st_mtim.tv_nsec)) {

	/*
	    with a directory the mtime is changed when an entry is added or deleted, this change is not interesting here
	    with a file, when the mtime is changed it should be picked up with the other checks...(like size or contents of file altered by write)

	*/

	cached_st->st_mtim.tv_sec=st->st_mtim.tv_sec;
	cached_st->st_mtim.tv_nsec=st->st_mtim.tv_nsec;

    }

    /* check the ctime */

    if (cached_st->st_ctim.tv_sec<st->st_ctim.tv_sec || (cached_st->st_ctim.tv_sec==st->st_ctim.tv_sec && cached_st->st_ctim.tv_nsec<st->st_ctim.tv_nsec)) {

	/* check for the xattr */

	if (fseventmask->attrib_event==0) {

	    /*

		it's not one of the other attributes, so
		probably something with xattr
		what changed exactly is todo .... 

	    */

	    fseventmask->xattr_event|=NOTIFYFS_FSEVENT_XATTR_NOTSET;

	    changed=1;

	}

	cached_st->st_ctim.tv_sec=st->st_ctim.tv_sec;
	cached_st->st_ctim.tv_nsec=st->st_ctim.tv_nsec;


    }

    /* check for utilities like touch, which change only the timestamps */

    /* check the atime */

    if (attr->cached_st.st_atim.tv_sec<st.st_atim.tv_sec || (attr->cached_st.st_atim.tv_sec==st.st_atim.tv_sec && attr->cached_st.st_atim.tv_nsec<st.st_atim.tv_nsec)) {

	attr->cached_st.st_atim.tv_sec=st.st_atim.tv_sec;
	attr->cached_st.st_atim.tv_nsec=st.st_atim.tv_nsec;

    }

    return changed;

}


/* function to test a fsevent (fseventmaskb) applies to the mask of a (clientwatch) fseventmask (fseventmaska) */

static unsigned char check_fsevent_applies(struct fseventmask_struct *fseventmaska, struct fseventmask_struct *fseventmaskb, unsigned char indir)
{

    if (fseventmaska->attrib_event & fseventmaskb->attrib_event) {

	if (indir==1) {

	    if (fseventmaska->attrib_event & NOTIFYFS_FSEVENT_ATTRIB_CHILD) {

		return 1;

	    }

	} else {

	    if (fseventmaska->attrib_event & NOTIFYFS_FSEVENT_ATTRIB_SELF) {

		return 1;

	    }

	}

    } else if (fseventmaska->xattr_event & fseventmaskb->xattr_event) {

	if (indir==1) {

	    if (fseventmaska->xattr_event & NOTIFYFS_FSEVENT_XATTR_CHILD) {

		return 1;

	    }

	} else {

	    if (fseventmaska->xattr_event & NOTIFYFS_FSEVENT_XATTR_SELF) {

		return 1;

	    }

	}

    } else if (fseventmaska->file_event & fseventmaskb->file_event) {

	if (indir==1) {

	    if (fseventmaska->file_event & NOTIFYFS_FSEVENT_FILE_CHILD) {

		return 1;

	    }

	} else {

	    if (fseventmaska->file_event & NOTIFYFS_FSEVENT_FILE_SELF) {

		return 1;

	    }

	}

    } else if (fseventmaska->move_event & fseventmaskb->move_event) {

	if (indir==1) {

	    if (fseventmaska->move_event & NOTIFYFS_FSEVENT_MOVE_CHILD) {

		return 1;

	    }

	} else {

	    if (fseventmaska->move_event & NOTIFYFS_FSEVENT_MOVE_SELF) {

		return 1;

	    }

	}

    } else if (fseventmaska->fs_event & fseventmaskb->fs_event) {

	return 1;

    }

    return 0;

}

/*

    update the count (of entries) in a directory

    determine the views by walking in the hashtable group_view

*/

void update_directory_count(struct watch_struct *watch, unsigned int count)
{
    struct clientwatch_struct *clientwatch=NULL;

    lock_watch(watch);

    watch->count=count;
    clientwatch=watch->clientwatches;

    while(clientwatch) {

	if (clientwatch->view) {
	    struct view_struct *view=clientwatch->view;

	    pthread_mutex_lock(&view->mutex);

	    if (view->count != count) {

		logoutput("update_directory_count: refresh count of view from %i to %i", view->count, count);

		view->count=count;
		pthread_cond_broadcast(&view->cond);

	    }

	    pthread_mutex_unlock(&view->mutex);

	}

	clientwatch=clientwatch->next_per_watch;

    }

    unlock_watch(watch);

}

unsigned char directory_is_viewed(struct watch_struct *watch)
{
    struct clientwatch_struct *clientwatch=NULL;
    unsigned char is_viewed=0;

    lock_watch(watch);

    clientwatch=watch->clientwatches;

    while(clientwatch) {

	if (clientwatch->view) {

	    is_viewed=1;
	    break;

	}

	clientwatch=clientwatch->next_per_watch;

    }

    unlock_watch(watch);

    return is_viewed;

}


void change_status_view(struct view_struct *view, unsigned char status)
{

    pthread_mutex_lock(&view->mutex);

    view->status=status;

    pthread_cond_broadcast(&view->cond);
    pthread_mutex_unlock(&view->mutex);

}

static void signalviewaboutfsevent(struct view_struct *view, struct notifyfs_fsevent_struct *fsevent)
{
    int mode=0;
    struct client_struct *client=NULL;
    struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *) fsevent->data;

    pthread_mutex_lock(&view->mutex);

    client=lookup_client(view->pid, 0);
    if (client) mode=client->mode;

    if (entry_in_view(entry, view)==1) {
	int j=view->queue_index;

	view->eventqueue[j].fseventmask.cache_event=fsevent->fseventmask.cache_event;
	view->eventqueue[j].fseventmask.attrib_event=fsevent->fseventmask.attrib_event;
	view->eventqueue[j].fseventmask.xattr_event=fsevent->fseventmask.xattr_event;
	view->eventqueue[j].fseventmask.file_event=fsevent->fseventmask.file_event;
	view->eventqueue[j].fseventmask.move_event=fsevent->fseventmask.move_event;
	view->eventqueue[j].fseventmask.fs_event=0;

	view->eventqueue[j].entry=entry->index;

	logoutput("notiyfs_clients: broadcast for view %i, entry %i, index queue %i", view->index, entry->index, j);

	view->queue_index=(view->queue_index + 1) % NOTIFYFS_VIEWQUEUE_LEN;

	pthread_cond_broadcast(&view->cond);

    }

    pthread_mutex_unlock(&view->mutex);

}


static void send_fsevent_to_owners(struct watch_struct *watch, struct notifyfs_fsevent_struct *fsevent, unsigned char indir, struct simple_group_struct *group_owner)
{
    struct clientwatch_struct *clientwatch=NULL;

    logoutput("send_fsevent_to_owners: watch id %li", watch->ctr);

    pthread_mutex_lock(&watch->mutex);

    clientwatch=watch->clientwatches;

    while (clientwatch) {

	if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
	    struct client_struct *client=clientwatch->notifyfs_owner.owner.localclient;

	    if (clientwatch->view) {

		signalviewaboutfsevent(clientwatch->view, fsevent);

	    } else if (group_owner) {

		/* check the event is not already send to the client */

		if ( ! lookup_simple_list(group_owner, (void *) &clientwatch->notifyfs_owner)) {

		    if (check_fsevent_applies(&clientwatch->fseventmask, &fsevent->fseventmask, indir)==1) {
			uint64_t unique=new_uniquectr();
			struct notifyfs_connection_struct *connection=client->connection;
			struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *) fsevent->data;

			/* send message */

			send_fsevent_message(connection->fd, unique, clientwatch->owner_watch_id, &fsevent->fseventmask, entry->index, &fsevent->detect_time, indir);

		    }

		}

	    } else {

		if (check_fsevent_applies(&clientwatch->fseventmask, &fsevent->fseventmask, indir)==1) {
		    uint64_t unique=new_uniquectr();
		    struct notifyfs_connection_struct *connection=client->connection;
		    struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *) fsevent->data;

		    /* send message */

		    send_fsevent_message(connection->fd, unique, clientwatch->owner_watch_id, &fsevent->fseventmask, entry->index, &fsevent->detect_time, indir);

		}

	    }


	} else if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
	    struct notifyfs_server_struct *server=clientwatch->notifyfs_owner.owner.remoteserver;

	    /* when the watch is set/owned by a remote server: always send a message, don't test it's in a view etcetera
	    */

	    if (server->type==NOTIFYFS_SERVERTYPE_NETWORK) {
		struct notifyfs_connection_struct *connection=server->connection;

		if (server->status==NOTIFYFS_SERVERSTATUS_UP && connection) {
    		    uint64_t unique=new_uniquectr();

		    if (indir==1) {
			struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *) fsevent->data;
			char *name=get_data(entry->name);

			/* send message */

			send_fsevent_message_remote(connection->fd, unique, clientwatch->owner_watch_id, &fsevent->fseventmask, name, &fsevent->detect_time);

		    } else {

			send_fsevent_message_remote(connection->fd, unique, clientwatch->owner_watch_id, &fsevent->fseventmask, NULL, &fsevent->detect_time);

		    }

		}

	    } else if (server->type==NOTIFYFS_SERVERTYPE_LOCALHOST) {
		struct watch_struct *orig_watch=lookup_watch_list(clientwatch->owner_watch_id);

		if (orig_watch) {
		    struct notifyfs_fsevent_struct *related_fsevent=NULL;

		    if (indir==0) {

			/* event on watch, so this also counts on the corresponding watch */

			related_fsevent=evaluate_remote_fsevent(orig_watch, &fsevent->fseventmask, NULL);

		    } else {
			struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *) fsevent->data;
			char *name=get_data(entry->name);

			/* event "in" directory of watch */

			related_fsevent=evaluate_remote_fsevent(orig_watch, &fsevent->fseventmask, name);

		    }

		    if (related_fsevent) queue_fsevent(related_fsevent);

		}

	    }

	}

	clientwatch=clientwatch->next_per_watch;

    }

    pthread_mutex_unlock(&watch->mutex);

}

/* hash function to identify the owner, by using the connection with the unique fd
*/

int owner_hashfunction(void *data)
{
    struct notifyfs_owner_struct *owner=(struct notifyfs_owner_struct *) data;
    struct notifyfs_connection_struct *connection=NULL;

    if (owner->type==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct client_struct *client=owner->owner.localclient;

	if (client) connection=client->connection;

    } else if (owner->type==NOTIFYFS_OWNERTYPE_SERVER) {
	struct notifyfs_server_struct *server=owner->owner.remoteserver;

	if (server) connection=server->connection;

    }

    if (connection) return connection->fd;

    return 0;

}

static void notify_clients_mountevent(struct notifyfs_fsevent_struct *fsevent, struct notifyfs_mount_struct *mount)
{
    struct client_struct *client;

    lock_clientslist();

    client=get_next_client(NULL);

    while(client) {

	if (client->mode & NOTIFYFS_CLIENTMODE_RECEIVEMOUNTS) {

	    logoutput("notifyfs_clients_mountevent: send (u)mount (TODO)");

	}

	client=get_next_client(client);

    }

    unlock_clientslist();

}



static void notify_clients(struct notifyfs_fsevent_struct *fsevent, struct simple_group_struct *group_owner)
{

    logoutput("notiyfs_clients");

    if (fsevent->data) {
	struct notifyfs_inode_struct *inode=NULL;
        struct watch_struct *watch=NULL;
	struct notifyfs_entry_struct *entry=NULL;

	entry=(struct notifyfs_entry_struct *) fsevent->data;
	inode=get_inode(entry->inode);
	watch=lookup_watch_inode(inode);

	if (watch) send_fsevent_to_owners(watch, fsevent, 0, group_owner);

	if (isrootentry(entry)==0) {

	    entry=get_entry(entry->parent);
	    inode=get_inode(entry->inode);
	    watch=lookup_watch_inode(inode);

	    if (watch) send_fsevent_to_owners(watch, fsevent, 1, group_owner);

	}

    }

}

static void change_clientwatches(struct watch_struct *watch, unsigned char action, unsigned char sendmessage, struct simple_group_struct *group_owner, char *basepath, struct notifyfs_entry_struct *baseentry)
{
    struct clientwatch_struct *clientwatch, *next_clientwatch;

    pthread_mutex_lock(&watch->mutex);

    clientwatch=watch->clientwatches;

    while (clientwatch) {

	next_clientwatch=clientwatch->next_per_watch;

	if (sendmessage==1) {

	    if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
		struct client_struct *client=clientwatch->notifyfs_owner.owner.localclient;

    		if ( client ) {

		    if ( ! lookup_simple_list(group_owner, (void *) &clientwatch->notifyfs_owner)) {

			add_element_to_group(group_owner, (void *) &clientwatch->notifyfs_owner);

        		if ( client->status==NOTIFYFS_CLIENTSTATUS_UP ) {
			    struct notifyfs_connection_struct *connection=client->connection;

			    if (connection) {
				uint64_t unique=new_uniquectr();
				struct notifyfs_inode_struct *inode=get_inode(baseentry->inode);
				char *name=get_data(baseentry->name);

				send_fsevent_message_remove(connection->fd, unique, inode->ino, baseentry->parent, baseentry->index, name, basepath);

			    }

			}

		    }

		}

	    } else if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
		struct notifyfs_server_struct *server=clientwatch->notifyfs_owner.owner.remoteserver;

		if (server->type==NOTIFYFS_SERVERTYPE_NETWORK) {

        	    if (server->status==NOTIFYFS_SERVERSTATUS_UP) {

			if ( ! lookup_simple_list(group_owner, (void *) &clientwatch->notifyfs_owner)) {
			    struct notifyfs_connection_struct *connection=server->connection;

			    add_element_to_group(group_owner, (void *) &clientwatch->notifyfs_owner);

			    if (connection) {
				uint64_t unique=new_uniquectr();
				struct notifyfs_inode_struct *inode=get_inode(baseentry->inode);
				char *name=get_data(baseentry->name);

				send_fsevent_message_remove(connection->fd, unique, inode->ino, baseentry->parent, baseentry->index, name, basepath);

			    }

			}

		    }

		}

	    }

	}

	if (action==FSEVENT_INODE_ACTION_REMOVE) {

	    remove_clientwatch_from_owner(clientwatch, 0);

	    clientwatch->next_per_watch=NULL;
	    clientwatch->prev_per_watch=NULL;
    	    free(clientwatch);

	    watch->nrwatches--;

	    /* what to do when the number of watches becomes zero */

	}

        clientwatch=next_clientwatch;

    }

    pthread_mutex_unlock(&watch->mutex);

}

/*
    process a change recursive 

    it's about a remove of a directory, an unmount (permanent or autofs)
    in these cases the whole tree is removed or set to sleep (autofs)

*/

static void process_changestate_fsevent_recursive(struct notifyfs_entry_struct *parent, unsigned char self, unsigned char action, char *workpath, unsigned char sendmessage, struct simple_group_struct *group_owner, char *basepath, struct notifyfs_entry_struct *baseentry)
{
    int res, nlen;

    struct notifyfs_inode_struct *inode;

    logoutput("process_remove_fsevent_recursive");

    inode=get_inode(parent->inode);
    nlen=strlen(workpath);

    if (inode) {
	struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	if (self==1) {
	    struct watch_struct *watch=lookup_watch_inode(inode);

	    if (watch) {

		lock_watch(watch);

		if (action==FSEVENT_INODE_ACTION_REMOVE) {

		    /* remove the watch */

		    change_clientwatches(watch, FSEVENT_INODE_ACTION_REMOVE, sendmessage, group_owner, basepath, baseentry);
		    remove_watch_backend_os_specific(watch);

		} else if (action==FSEVENT_INODE_ACTION_SLEEP) {

		    /* set watch to sleep modus */

		    change_clientwatches(watch, FSEVENT_INODE_ACTION_SLEEP, sendmessage, group_owner, basepath, baseentry);
		    remove_watch_backend_os_specific(watch);

		    /* inform the backend...*/

		    /* backend: 
			- remote server: inform once
			- fuse fs : inform once only when up (it's unmounted!)
			- local server: inform once??
		    */

		} else if (action==FSEVENT_INODE_ACTION_WAKEUP) {

		    /* wakeup the watch*/

		    change_clientwatches(watch, FSEVENT_INODE_ACTION_WAKEUP, sendmessage, group_owner, basepath, baseentry);
		    set_watch_backend_os_specific(watch);

		    /* inform the backend...*/

		    /* backend: 
			- remote server: inform once
			- fuse fs : inform once only when up (it's unmounted!)
			- local server: inform once??
		    */

		}

		unlock_watch(watch);

		if (action==FSEVENT_INODE_ACTION_REMOVE) {

		    remove_watch_from_list(watch);
		    remove_watch_from_table(watch);

		    pthread_mutex_destroy(&watch->mutex);

		    free_path_pathinfo(&watch->pathinfo);

		    free(watch);

		}

	    }

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
		struct notifyfs_entry_struct *entry, *next_entry;

		/* when a directory: first remove contents (recursive) */

		entry=get_next_entry(parent, NULL);

		while(entry) {

		    next_entry=get_next_entry(parent, entry);

		    name=get_data(entry->name);
		    lenname=strlen(name);

		    *(workpath+nlen)='/';
		    memcpy(workpath+nlen+1, name, lenname);
		    *(workpath+nlen+lenname)='\0';

    		    process_changestate_fsevent_recursive(entry, 1, action, workpath, sendmessage, group_owner, basepath, baseentry);

		    *(workpath+nlen)='\0';

		    entry=next_entry;

		}

	    }

	}

	if (self==1) {

	    if (action==FSEVENT_INODE_ACTION_REMOVE) inode->status=FSEVENT_INODE_STATUS_REMOVED;

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

static void process_changestate_fsevent(struct notifyfs_entry_struct *entry, unsigned char action)
{

    logoutput("process_changestate_fsevent");

    if (action==FSEVENT_INODE_ACTION_REMOVE) {

	notify_kernel_delete(notifyfs_chan, entry);
	remove_entry_from_name_hash(entry);
	remove_entry(entry);

    }

}

static void process_one_fsevent(struct notifyfs_fsevent_struct *fsevent)
{
    unsigned char catched=0;

    logoutput("process_one_fsevent");

    if (fsevent->fseventmask.cache_event & (NOTIFYFS_FSEVENT_CACHE_ADDED | NOTIFYFS_FSEVENT_CACHE_REMOVED)) {
	struct simple_group_struct group_owner;

	initialize_group(&group_owner, owner_hashfunction, 64);
	notify_clients(fsevent, &group_owner);

	catched=1;

    } else if (fsevent->fseventmask.fs_event & (NOTIFYFS_FSEVENT_FS_UNMOUNT | NOTIFYFS_FSEVENT_FS_MOUNT)) {
	struct supermount_struct *supermount=(struct supermount_struct *) fsevent->data;

	if (! supermount) goto out;

	if (fsevent->fseventmask.fs_event & NOTIFYFS_FSEVENT_FS_UNMOUNT) {
	    struct notifyfs_entry_struct *entry=NULL;
	    struct stat st;
	    int error=0;

	    catched=1;

	    entry=create_notifyfs_path(&fsevent->pathinfo, &st, 0, 0, &error, 0, 0, 0);

	    if (entry) {
		struct notifyfs_mount_struct *mount=NULL;

		mount=get_mount(entry->mount);

		if (mount) {

		    /*
			existing mount found, what's happening?
			TODO:
			    - when mounting over an existing mountpoint, and then unmounted again, how to find the first mount?
		    */

		    if (mount->major==supermount->major && mount->minor==supermount->minor) {

			if (fsevent->flags & MOUNTENTRY_FLAG_BY_AUTOFS) {
			    pathstring path;

			    /*
				unmount done by automounter
				set the whole tree into sleep
			    */

			    strcpy(path, fsevent->pathinfo.path);

			    process_changestate_fsevent_recursive(entry, 0, FSEVENT_INODE_ACTION_SLEEP, path, 0, NULL, NULL, 0);

			    logoutput("process_one_fsevent: device %i:%i still mounted", mount->major, mount->minor);

			    mount->status=NOTIFYFS_MOUNTSTATUS_SLEEP;
			    notify_clients_mountevent(fsevent, mount);

			} else {
			    pathstring path;
			    int refcount=0, major, minor;
			    struct notifyfs_entry_struct *parent=NULL;

			    /*
				normal permanent umount
			    */

			    strcpy(path, fsevent->pathinfo.path);

			    process_changestate_fsevent_recursive(entry, 0, FSEVENT_INODE_ACTION_REMOVE, path, 0, NULL, NULL, 0);

			    major=supermount->major;
			    minor=supermount->minor;

			    refcount=remove_mount_supermount(supermount);

			    /* send message to clients */

			    if (refcount<=0) {

				logoutput("process_one_fsevent: device %i:%i not mounted", major, minor);

			    } else {

				logoutput("process_one_fsevent: device %i:%i still %i mounted", major, minor, refcount);

			    }

			    remove_mount(mount);

			    parent=get_entry(entry->parent);

			    if (parent) {

				entry->mount=parent->mount;

			    } else {

				entry->mount=-1;

			    }

			}

			notify_clients_mountevent(fsevent, mount);

		    } else {

			logoutput("process_one_fsevent: existing mount %i:%i found, is not the same as %i:%i", mount->major, mount->minor, supermount->major, supermount->minor);

		    }

		} else {

		    logoutput("process_one_fsevent: no mount %i:%i found at %s in notifyfs", supermount->major, supermount->minor, fsevent->pathinfo.path);

		}

	    } else {

		logoutput("process_one_fsevent: unable to create %s in notifyfs for mount %i:%i", fsevent->pathinfo.path, supermount->major, supermount->minor);

	    }

	} else if (fsevent->fseventmask.fs_event & NOTIFYFS_FSEVENT_FS_MOUNT) {
	    struct notifyfs_entry_struct *entry=NULL;
	    struct stat st;
	    int error=0;

	    catched=1;

	    if (strcmp(fsevent->pathinfo.path, "/")==0) {

		entry=get_rootentry();

	    } else {

		entry=create_notifyfs_path(&fsevent->pathinfo, &st, 0, 0, &error, 0, 0, 0);

	    }

	    if (entry) {
		struct notifyfs_mount_struct *mount=NULL;

		mount=get_mount(entry->mount);

		if (mount) {

		    if (mount->entry!=entry->index) mount=NULL;

		}

		if (mount) {

		    /* existing mount found, what's happening? */

		    if (mount->major==supermount->major && mount->minor==supermount->minor) {

			/* already mounted: is it a remount/autofs? */

			if (mount->status==NOTIFYFS_MOUNTSTATUS_SLEEP) {
			    pathstring workpath;

			    /* bring back */

			    strcpy(workpath, fsevent->pathinfo.path);

			    /* wake the whole tree (including watches) */

			    process_changestate_fsevent_recursive(entry, 0, FSEVENT_INODE_ACTION_WAKEUP, workpath, 0, NULL, NULL, 0);

			    mount->status=NOTIFYFS_MOUNTSTATUS_UP;
			    notify_clients_mountevent(fsevent, mount);

			} else {

			    logoutput("process_one_fsevent: %i:%i already mounted at %s", supermount->major, supermount->minor, fsevent->pathinfo.path);
			    goto out;

			}

		    } else {

			mount=NULL;
			logoutput("process_one_fsevent: another mount %i:%i mounted at %s", mount->major, mount->minor, fsevent->pathinfo.path);

		    }

		}

		if (! mount) {

		    logoutput("process_one_fsevent: creating new mount");

		    mount=create_mount(entry, supermount->major, supermount->minor);

		    if (mount) {

			mount->status=NOTIFYFS_MOUNTSTATUS_UP;
			set_supermount_backend(supermount, mount, fsevent->pathinfo.path);
			notify_clients_mountevent(fsevent, mount);

		    }

		}

	    }

	    /* here additional: adjust the attributes of the entry since an umount affects these 
		maybe store it in mountentry?? */

	}

    } else if (fsevent->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED )) {
	struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *)fsevent->data;
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	if (inode) {
	    struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	    if (S_ISDIR(attr->cached_st.st_mode)) {
		pathstring workpath;
		struct simple_group_struct group_owner;

		logoutput("process_one_fsevent: remove/moved, handle %i:%i:%i:%i", fsevent->fseventmask.attrib_event, fsevent->fseventmask.xattr_event, fsevent->fseventmask.file_event, fsevent->fseventmask.move_event);

		strcpy(workpath, fsevent->pathinfo.path);

		initialize_group(&group_owner, owner_hashfunction, 64);

		catched=1;

		/* here test the watch(es) interested in this event and send eventually a message*/

		notify_clients(fsevent, &group_owner);
		process_changestate_fsevent_recursive(entry, 1, FSEVENT_INODE_ACTION_REMOVE, workpath, 1, &group_owner, fsevent->pathinfo.path, entry);

		free_group(&group_owner);

	    }

	}

	if (catched==0) {

	    catched=1;

	    notify_clients(fsevent, NULL);
	    process_changestate_fsevent(entry, FSEVENT_INODE_ACTION_REMOVE);

	}

    } else {

	if ((fsevent->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_CREATED | NOTIFYFS_FSEVENT_MOVE_MOVED_TO)) ||
	    (fsevent->fseventmask.attrib_event & NOTIFYFS_FSEVENT_ATTRIB_CA) || 
	    (fsevent->fseventmask.xattr_event & NOTIFYFS_FSEVENT_XATTR_CA) || 
	    (fsevent->fseventmask.file_event & (NOTIFYFS_FSEVENT_FILE_MODIFIED | NOTIFYFS_FSEVENT_FILE_SIZE | NOTIFYFS_FSEVENT_FILE_LOCK_CA))) {

	    struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *)fsevent->data;
	    struct notifyfs_inode_struct *inode=NULL;
	    struct simple_group_struct group_owner;

	    /* it's possible that the entry does not have an inode yet */

	    if (entry->inode<0) assign_inode(entry);

	    inode=get_inode(entry->inode);

	    if (inode) {
		struct notifyfs_attr_struct *attr=get_attr(inode->attr);

		if (! attr) {
		    struct stat st;

		    if (lstat(fsevent->pathinfo.path, &st)==0) {

			attr=assign_attr(&st, inode);

			if (attr) {

			    copy_stat(&attr->cached_st, &st);
			    copy_stat_times(&attr->cached_st, &st);

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

			/* what to do here: a fsevent indicating a change, a create, but not a delete, but stat gives an error */

			logoutput("process_one_fsevent: new/changed, but stat gives error %i", errno);

		    }

		}

	    }

	    if (fsevent->fseventmask.move_event & (NOTIFYFS_FSEVENT_MOVE_CREATED | NOTIFYFS_FSEVENT_MOVE_MOVED_TO)) {

		add_to_name_hash_table(entry);

	    }

	    catched=1;
	    initialize_group(&group_owner, owner_hashfunction, 64);

	    logoutput("process_one_fsevent: new/changed, handle %i:%i:%i:%i", fsevent->fseventmask.attrib_event, fsevent->fseventmask.xattr_event, fsevent->fseventmask.file_event, fsevent->fseventmask.move_event);

	    notify_clients(fsevent, &group_owner);

	}

    }

    out:

    if (catched==0) {

	/* not handled here */

	logoutput("process_one_fsevent: event %i:%i:%i:%i not handled here", fsevent->fseventmask.attrib_event, fsevent->fseventmask.xattr_event, fsevent->fseventmask.file_event, fsevent->fseventmask.move_event);

    }

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
		make this period configurable ..

		TODO: add the 5 in the config

		*/

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

	logoutput("process_fsevent: got fsevent");

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

	logoutput("process_fsevent: jump back!");

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

	logoutput("queue_required: path %s, queue is empty", fsevent->pathinfo.path);

    } else {
	struct notifyfs_fsevent_struct *fsevent_walk=main_fsevents_queue.last;
        char *path2, *path1=fsevent->pathinfo.path;
        int len1=strlen(path1), len2;

	logoutput("queue_required: path %s, check the queue", path1);

        doqueue=1;

        /* walk through queue to check there is a related call already there */

        while(fsevent_walk) {

	    if ( ! fsevent_walk->pathinfo.path) {

		fsevent_walk=fsevent_walk->prev;
		continue;

	    }

	    /* compare this previous event with the new one */

            path2=fsevent_walk->pathinfo.path;
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

		if (strcmp(path2, path1)==0) {

		    /* paths are the same, but it may be another entry ... */

		    if (fsevent_walk->fseventmask.fs_event & (NOTIFYFS_FSEVENT_FS_UNMOUNT_REMOVE | NOTIFYFS_FSEVENT_FS_UNMOUNT_AUTOFS)) {

			if (fsevent->fseventmask.fs_event & (NOTIFYFS_FSEVENT_FS_UNMOUNT_REMOVE | NOTIFYFS_FSEVENT_FS_UNMOUNT_AUTOFS)) {

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

			if (fsevent->data==fsevent_walk->data) {

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

void queue_fsevent(struct notifyfs_fsevent_struct *fsevent)
{
    struct fseventmask_struct *fseventmask=&fsevent->fseventmask, fseventmask_keep;
    int res=0;
    struct notifyfs_mount_struct *mount=NULL;
    struct supermount_struct *supermount=NULL;
    struct pathinfo_struct pathinfo={NULL, 0, 0};

    logoutput("queue_fsevent");

    if (fseventmask->fs_event==0) {
	struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *)fsevent->data;
	int len=1;

	mount=get_mount(entry->mount);

	supermount=find_supermount_majorminor(mount->major, mount->minor);

	if (supermount->refcount>1) {
	    char *name=NULL;

	    /* determine the path on this mount

	    */

	    while(entry->index>0) {

		if (entry->mount==mount->index && mount->entry != entry->index) {

		    name=get_data(entry->name);

		    len+=strlen(name)+1;

		    entry=get_entry(entry->parent);

		} else {

		    break;

		}

	    }

	    if (len>1) {

		pathinfo.path=malloc(len);

		if (pathinfo.path) {
		    char *pos=pathinfo.path+len-1;
		    int len0;

		    *pos='\0';

		    while(entry->index>0) {

			if (entry->mount==mount->index && mount->entry != entry->index) {

			    name=get_data(entry->name);

			    len0=strlen(name);

			    pos-=len0;

			    memcpy(pos, name, len0);
			    pos--;
			    *pos='/';

			    entry=get_entry(entry->parent);

			} else {

			    break;

			}

		    }

		    pathinfo.len=len;
		    pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

		    replace_fseventmask(&fseventmask_keep, fseventmask);

		}

	    }

	}

	/* adjust the status of this inode in case of an remove */

	if (fseventmask->move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED)) {
	    struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	    if (inode) inode->status=FSEVENT_INODE_STATUS_TOBEREMOVED;

	}

    }

	/* not an mount event */

    /*
	in some cases the path is not set

    */

    if (! fsevent->pathinfo.path) {

	if (fseventmask->move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED )) {
	    struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *)fsevent->data;

	    res=determine_path(entry, &fsevent->pathinfo);

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

    }


    /* test it's required to queue this event */

    fsevent->status=NOTIFYFS_FSEVENT_STATUS_WAITING;

    pthread_mutex_lock(&main_fsevents_queue.mutex);

    if (fsevent->pathinfo.path) {

	res=queue_required(fsevent);

    } else {

	if ( ! main_fsevents_queue.first ) {

	    main_fsevents_queue.first=fsevent;
	    main_fsevents_queue.last=fsevent;

	} else {

	    main_fsevents_queue.last->next=fsevent;
	    fsevent->next=NULL;
	    fsevent->prev=main_fsevents_queue.last;
	    main_fsevents_queue.last=fsevent;

	}

	res=1;

    }

    pthread_mutex_unlock(&main_fsevents_queue.mutex);

    if (res==1) {

	logoutput("queue_fsevent: path: %s, fsevent queued", fsevent->pathinfo.path);

	/* possibly activate a workerthread */

	if (main_fsevents_queue.nrthreads<MAXIMUM_PROCESS_FSEVENTS_NRTHREADS) {
	    struct workerthread_struct *workerthread=NULL;

	    logoutput("queue_fsevent: path: %s, nr threads %i, starting a new thread", fsevent->pathinfo.path, main_fsevents_queue.nrthreads);

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

	destroy_notifyfs_fsevent(fsevent);

    }

    if (pathinfo.path) {

	logoutput("queue_fsevent: found path %s on %i:%i, total mounts %i", pathinfo.path, supermount->major, supermount->minor, supermount->refcount);

	free_path_pathinfo(&pathinfo);

    }

}

void init_changestate(struct workerthreads_queue_struct *workerthreads_queue)
{

    global_workerthreads_queue=workerthreads_queue;

}

struct notifyfs_fsevent_struct *evaluate_remote_fsevent(struct watch_struct *watch, struct fseventmask_struct *fseventmask, char *name)
{
    struct notifyfs_fsevent_struct *fsevent=NULL;
    struct notifyfs_entry_struct *entry=NULL;
    struct notifyfs_inode_struct *inode=NULL;
    struct stat st;
    struct pathinfo_struct pathinfo={NULL, 0, 0};

    if (name) {
	struct notifyfs_entry_struct *parent=NULL;

	inode=watch->inode;
	parent=get_entry(inode->alias);

	entry=find_entry_raw(parent, inode, name, 1, NULL);

	if ( ! entry) {

	    if (fseventmask->move_event & (NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED)) {

		/* ignore this event: it's does not exist here, so a delete message does not matter */

		goto out;

	    }

	    entry=create_entry(parent, name);

	    if ( ! entry) {

		logoutput("evaluate_remote_fsevent: unable to create an entry for %s, cannot continue", name);
		goto out;

	    }

	}

	if (watch->pathinfo.path) {
	    int len1=strlen(name), len0=watch->pathinfo.len;

	    pathinfo.path=malloc(len0 + 2 + len1);

	    if (pathinfo.path) {

		memcpy(pathinfo.path, watch->pathinfo.path, len0);
		*(pathinfo.path + len0)='/';
		len0++;
		memcpy(pathinfo.path+len0, name, len1);
		len0+=len1;
		*(pathinfo.path + len0)='\0';

		pathinfo.len=len0;
		pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

	    }

	}

    } else {

	inode=watch->inode;
	entry=get_entry(inode->alias);

	if (watch->pathinfo.path) {

	    pathinfo.path=watch->pathinfo.path;
	    pathinfo.len=watch->pathinfo.len;
	    pathinfo.flags=0;

	}

    }

    if (!pathinfo.path) {

	int res=determine_path(entry, &pathinfo);

	if (res<0) {

	    logoutput("evaluate_remote_fsevent: unable to create an path for %s, error %i, cannot continue", name, res);
	    goto out;

	}

	pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

    }

    logoutput("evaluate_remote_fsevent: check path %s", pathinfo.path);

    if (lstat(pathinfo.path, &st)==-1) {

	/* path does not exist anymore */

	fsevent=create_fsevent(entry);

	if (fsevent) {

	    fsevent->fseventmask.move_event|=NOTIFYFS_FSEVENT_MOVE_DELETED;
	    fsevent->pathinfo.path=pathinfo.path;
	    fsevent->pathinfo.flags=pathinfo.flags;
	    fsevent->pathinfo.len=pathinfo.len;

	    pathinfo.path=NULL;
	    pathinfo.len=0;
	    pathinfo.flags=0;

	    get_current_time(&fsevent->detect_time);

	} else {

	    logoutput("evaluate_remote_fsevent: unable to create an fsevent for %s, cannot continue", name);

	}

	goto out;

    }

    if (entry->inode<0) assign_inode(entry);

    inode=get_inode(entry->inode);

    if (inode) {
	struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	if (! attr) {

	    attr=assign_attr(&st, inode);

	    if (attr) {

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

	    } else {

		logoutput("evaluate_remote_fsevent: unable to create an attr for %s, cannot continue", name);
		goto out;

	    }

	    fsevent=create_fsevent(entry);

	    if (fsevent) {

		fsevent->pathinfo.path=pathinfo.path;
		fsevent->pathinfo.len=pathinfo.len;
		fsevent->pathinfo.flags=pathinfo.flags;

		pathinfo.path=NULL;
		pathinfo.len=0;
		pathinfo.flags=0;

		get_current_time(&fsevent->detect_time);

		fsevent->fseventmask.attrib_event=fseventmask->attrib_event;
		fsevent->fseventmask.xattr_event=fseventmask->xattr_event;
		fsevent->fseventmask.file_event=fseventmask->file_event;
		fsevent->fseventmask.move_event=fseventmask->move_event;
		fsevent->fseventmask.fs_event=0;

	    } else {

		logoutput("evaluate_remote_fsevent: unable to create an fsevent for %s, cannot continue", name);

	    }

	} else {

	    /* here: able to compare the outcome of lstat and the cached attributes
		only where there is a difference, create an fsevent
	    */

	    /* first completly ignore what the remote notifyfs server/fs has reported, and take what the stat reports */

	    fseventmask->attrib_event=0;
	    fseventmask->xattr_event=0; /* do something else with this info */
	    fseventmask->file_event=0;
	    fseventmask->move_event=0;
	    fseventmask->fs_event=0;

	    if (st.st_mode != attr->cached_st.st_mode) {

		fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_MODE;
		attr->cached_st.st_mode=st.st_mode;

	    }

	    if (st.st_uid != attr->cached_st.st_uid) {

		fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_OWNER;
		attr->cached_st.st_uid=st.st_uid;

	    }

	    if (st.st_gid != attr->cached_st.st_gid) {

		fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_GROUP;
		attr->cached_st.st_gid=st.st_gid;

	    }

	    if (st.st_nlink != attr->cached_st.st_nlink) {

		fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_NLINKS;
		attr->cached_st.st_nlink=st.st_nlink;

	    }

	    if (st.st_size != attr->cached_st.st_size) {

		fseventmask->file_event|=NOTIFYFS_FSEVENT_FILE_SIZE;
		attr->cached_st.st_size=st.st_size;

	    }

	    /*

		what to do with changes in ctime and mtime ??

		attr->cached_st.st_ctime.tv_sec<st.st_ctim.tv_sec || 
		(attr->cached_st.st_ctim.tv_sec==st.st_ctim.tv_sec && attr->cached_st.st_ctim.tv_nsec<st.st_ctim.tv_nsec) ||
		attr->cached_st.st_mtim.tv_sec<st.st_mtim.tv_sec || 
		(attr->cached_st.st_mtim.tv_sec==st.st_mtim.tv_sec && attr->cached_st.st_mtim.tv_nsec<st.st_mtim.tv_nsec)) {
	    */


	    if (fseventmask->file_event>0 || fseventmask->move_event>0 || fseventmask->xattr_event>0 || fseventmask->attrib_event>0) {

		fsevent=create_fsevent(entry);

		if (fsevent) {

		    fsevent->pathinfo.path=pathinfo.path;
		    fsevent->pathinfo.len=pathinfo.len;
		    fsevent->pathinfo.flags=pathinfo.flags;

		    pathinfo.path=NULL;
		    pathinfo.len=0;
		    pathinfo.flags=0;

		    get_current_time(&fsevent->detect_time);

		    fsevent->fseventmask.attrib_event=fseventmask->attrib_event;
		    fsevent->fseventmask.xattr_event=fseventmask->xattr_event;
		    fsevent->fseventmask.file_event=fseventmask->file_event;
		    fsevent->fseventmask.move_event=fseventmask->move_event;
		    fsevent->fseventmask.fs_event=0;

		} else {

		    logoutput("evaluate_remote_fsevent: unable to create an fsevent for %s, cannot continue", name);

		}

	    }

	}

    } else {

	logoutput("evaluate_remote_fsevent: unable to create an inode for %s, cannot continue", name);

    }

    out:

    free_path_pathinfo(&pathinfo);

    return fsevent;

}
