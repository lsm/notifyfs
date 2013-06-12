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

#define WATCHES_TABLESIZE          1024

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

#include "message-base.h"
#include "message-send.h"


#ifdef __gnu_linux__

#ifdef HAVE_INOTIFY
#include "watches-linux-inotify.c"

int set_watch_backend_os_specific(struct watch_struct *watch)
{
    return set_watch_backend_inotify(watch);
}

int change_watch_backend_os_specific(struct watch_struct *watch)
{
    return change_watch_backend_inotify(watch);
}

void remove_watch_backend_os_specific(struct watch_struct *watch)
{
    remove_watch_backend_inotify(watch);
}

void initialize_watches_backend()
{
    initialize_inotify();
}

void close_watches_backend()
{
    close_inotify();

}

#else
#include "watches-notsup.c"
#endif

#else

#include "watches-notsup.c"

#endif

struct watch_struct *watch_table[WATCHES_TABLESIZE];

unsigned long watchctr = 1;
struct watch_struct *watch_list=NULL;
pthread_mutex_t watchctr_mutex=PTHREAD_MUTEX_INITIALIZER;

void lock_watch(struct watch_struct *watch)
{
    pthread_mutex_lock(&watch->mutex);
}

void unlock_watch(struct watch_struct *watch)
{
    pthread_mutex_unlock(&watch->mutex);
}

void init_watch_hashtables()
{
    int i;

    for (i=0;i<WATCHES_TABLESIZE;i++) {

	watch_table[i]=NULL;

    }

}

/* here some function to lookup the eff watch, given the mount entry */

void add_watch_to_table(struct watch_struct *watch)
{
    int hash=watch->inode->ino%WATCHES_TABLESIZE;

    if ( watch_table[hash] ) watch_table[hash]->prev_hash=watch;
    watch->next_hash=watch_table[hash];
    watch_table[hash]=watch;

}

void remove_watch_from_table(struct watch_struct *watch)
{
    int hash=watch->inode->ino%WATCHES_TABLESIZE;

    if ( watch_table[hash]==watch ) watch_table[hash]=watch->next_hash;
    if ( watch->next_hash ) watch->next_hash->prev_hash=watch->prev_hash;
    if ( watch->prev_hash ) watch->prev_hash->next_hash=watch->next_hash;

}

/* simple lookup function of watch */

struct watch_struct *lookup_watch_inode(struct notifyfs_inode_struct *inode)
{
    struct watch_struct *watch=NULL;
    int hash=inode->ino%WATCHES_TABLESIZE;

    /* lookup using the ino */

    watch=watch_table[hash];

    while(watch) {

	if (watch->inode==inode) break;

	watch=watch->next_hash;

    }

    return watch;

}

struct watch_struct *lookup_watch_list(unsigned long ctr)
{
    struct watch_struct *watch=NULL;

    /* lookup using the ctr */

    pthread_mutex_lock(&watchctr_mutex);

    watch=watch_list;

    while(watch) {

	if (watch->ctr==ctr) break;

	watch=watch->next;

    }

    pthread_mutex_unlock(&watchctr_mutex);

    return watch;

}



void add_watch_to_list(struct watch_struct *watch)
{

    pthread_mutex_lock(&watchctr_mutex);

    if (watch_list) watch_list->prev=watch;
    watch->next=watch_list;
    watch->prev=NULL;
    watch_list=watch;

    watch->ctr=watchctr;

    watchctr++;

    pthread_mutex_unlock(&watchctr_mutex);

}

void remove_watch_from_list(struct watch_struct *watch)
{

    pthread_mutex_lock(&watchctr_mutex);

    if (watch->next) watch->next->prev=watch->prev;
    if (watch->prev) watch->prev->next=watch->next;

    if (watch_list==watch) watch_list=watch->next;

    pthread_mutex_unlock(&watchctr_mutex);

}


static void add_clientwatch_owner(struct notifyfs_owner_struct *notifyfs_owner, struct clientwatch_struct *clientwatch)
{

    if (notifyfs_owner->type==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct client_struct *client=notifyfs_owner->owner.localclient;

	/* add it to list per client */

	pthread_mutex_lock(&client->mutex);

	if (client->clientwatches) ((struct clientwatch_struct *) client->clientwatches)->prev_per_owner=clientwatch;
	clientwatch->next_per_owner=(struct clientwatch_struct *) client->clientwatches;
	clientwatch->prev_per_owner=NULL;
	client->clientwatches=(void *) clientwatch;

	pthread_mutex_unlock(&client->mutex);

    } else if (notifyfs_owner->type==NOTIFYFS_OWNERTYPE_SERVER) {
	struct notifyfs_server_struct *server=notifyfs_owner->owner.remoteserver;

	/* add it to list per server */

	pthread_mutex_lock(&server->mutex);

	if (server->clientwatches) ((struct clientwatch_struct *) server->clientwatches)->prev_per_owner=clientwatch;
	clientwatch->next_per_owner=(struct clientwatch_struct *) server->clientwatches;
	clientwatch->prev_per_owner=NULL;
	server->clientwatches=(void *) clientwatch;

	pthread_mutex_unlock(&server->mutex);

    }

}

int sync_directory(struct notifyfs_inode_struct *inode, struct timespec *current_time, struct stat *st, 
{
    struct notifyfs_attr_struct *attr;
    int res=0;

    attr=get_attr(inode->attr);
    if (! attr) attr=assign_attr(st, inode);

    /*
	check the directory needs a full sync or a simple one
	this depends of course
    */

    if (attr->mtim.tv_sec<st->st_mtim.tv_sec || (attr->mtim.tv_sec==st->st_mtim.tv_sec && attr->mtim.tv_nsec<st->st_mtim.tv_nsec) ) {

	res=sync_directory_full(command_setwatch->pathinfo.path, entry, &current_time, 1);

	if (res>=0) {
	    unsigned int count=0;

		    logoutput("process_command_setwatch: remove old entries");

		    count=remove_old_entries(entry, &current_time, 1);
		    update_directory_count(watch, count);

		} else {

		    /* */

		    logoutput("process_command_view: error %i sync directory %s", res, command_setwatch->pathinfo.path);

		}

	    } else {
		unsigned int count=0;

		logoutput("process_command_setwatch: simple synchronize");

		count=sync_directory_simple(command_setwatch->pathinfo.path, entry, &current_time, 1);
		update_directory_count(watch, count);

	    }


/*
    polling functions
*/



int set_watch_polling(struct watch_struct *watch)
{
    int res;
    struct timerspec expiretime;
    struct timerentry_struct *timerentry=NULL;

    /* here:
    - create a timer with a certain expire time
    - what command to run??
    - this command should "reset" the timer
    */

    get_current_time(&expiretime);

    expiretime.tv_sec+=5; /* to be configured */

    if (watch->mode & NOTIFYFS_WATCHMODE_OSSPECIFIC) watch->mode-=NOTIFYFS_WATCHMODE_OSSPECIFIC;
    watch->mode|=NOTIFYFS_WATCHMODE_POLLING;

    timerentry=create_timerentry(&expiretime);

    if (timerentry) {

	





struct clientwatch_struct *add_clientwatch(struct notifyfs_inode_struct *inode, struct fseventmask_struct *fseventmask, int id, struct notifyfs_owner_struct *notifyfs_owner, struct pathinfo_struct *pathinfo, struct timespec *update_time, unsigned char onsystemfs)
{
    struct clientwatch_struct *clientwatch=NULL;
    struct watch_struct *watch=NULL;
    unsigned char watchcreated=0;
    int nreturn=0;
    struct notifyfs_attr_struct *attr=get_attr(inode->attr);
    struct fseventmask_struct zero_fseventmask=NOTIFYFS_ZERO_FSEVENTMASK;

    watch=lookup_watch_inode(inode);

    if (pathinfo->path) {

	/* possible here compare the pathinfo->path with the one stored in watch->pathinfo.path*/

	logoutput("add_clientwatch: on %s client watch id %i, %i:%i:%i:%i", pathinfo->path, id, fseventmask->attrib_event, fseventmask->xattr_event, fseventmask->file_event, fseventmask->move_event);

    } else {

	if (! watch) {

	    if (compare_fseventmasks(&zero_fseventmask, fseventmask)==1) {

		logoutput("add_clientwatch: path not set for client watch id %i, while fseventmask is not zero, cannot continue", id);
		goto out;

	    } else {

		logoutput("add_clientwatch: on UNKNOWN path client watch id %i, %i:%i:%i:%i", id, fseventmask->attrib_event, fseventmask->xattr_event, fseventmask->file_event, fseventmask->move_event);

	    }

	}

    }

    if ( ! watch ) {

	logoutput("add_clientwatch: no watch found, creating one");

	watch=malloc(sizeof(struct watch_struct));

	if (watch) {

	    watch->ctr=0;
	    watch->mode=0;
	    watch->inode=inode;

	    /* take over the path only if allocated and not inuse */

	    if ((!(pathinfo->flags & NOTIFYFS_PATHINFOFLAGS_INUSE)) && (pathinfo->flags & NOTIFYFS_PATHINFOFLAGS_ALLOCATED)) {

		watch->pathinfo.path=pathinfo->path;
		watch->pathinfo.len=pathinfo->len;
		watch->pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_INUSE | NOTIFYFS_PATHINFOFLAGS_ALLOCATED;
		pathinfo->flags-=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

	    } else {

		watch->pathinfo.path=strdup(pathinfo->path);

		if (watch->pathinfo.path) {

		    watch->pathinfo.len=pathinfo->len;
		    watch->pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_INUSE | NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

		} else {

		    logoutput("add_clientwatch: error alloacting memory for path %s", pathinfo->path);
		    free(watch);
		    watch=NULL;
		    goto out;

		}

	    }

	    replace_fseventmask(&watch->fseventmask, &zero_fseventmask);

	    watch->nrwatches=0;
	    watch->clientwatches=NULL;

	    pthread_mutex_init(&watch->mutex, NULL);

	    watch->count=0;

	    watch->next_hash=NULL;
	    watch->prev_hash=NULL;
	    watch->next=NULL;
	    watch->prev=NULL;

	    add_watch_to_table(watch); /* lookup table per inode */
	    add_watch_to_list(watch); /* lookup table per ctr */

	    watch->create_time.tv_sec=update_time->tv_sec;
	    watch->create_time.tv_nsec=update_time->tv_nsec;

	    watch->change_time.tv_sec=update_time->tv_sec;
	    watch->change_time.tv_nsec=update_time->tv_nsec;

	} else {

	    logoutput("add_clientwatch: unable to allocate a watch");
	    nreturn=-ENOMEM;
	    goto out;

	}

    } else {

	/* existing watch found */

	watch->change_time.tv_sec=update_time->tv_sec;
	watch->change_time.tv_nsec=update_time->tv_nsec;

    }

    pthread_mutex_lock(&watch->mutex);

    /* lookup client watch 

	if it exists already 
    */

    clientwatch=watch->clientwatches;

    while(clientwatch) {

	if (clientwatch->notifyfs_owner.type==notifyfs_owner->type) {

	    if (notifyfs_owner->type==NOTIFYFS_OWNERTYPE_CLIENT) {

		if (notifyfs_owner->owner.localclient==clientwatch->notifyfs_owner.owner.localclient) break;

	    } else if (notifyfs_owner->type==NOTIFYFS_OWNERTYPE_SERVER) {

		if (notifyfs_owner->owner.remoteserver==clientwatch->notifyfs_owner.owner.remoteserver) break;

	    }

	}

	clientwatch=clientwatch->next_per_watch;

    }

    if ( ! clientwatch) {

	logoutput("add_clientwatch: no clientwatch found, creating one");

	clientwatch=malloc(sizeof(struct clientwatch_struct));

	if (clientwatch) {

	    replace_fseventmask(&clientwatch->fseventmask, fseventmask);

	    clientwatch->watch=watch;
	    watch->nrwatches++;

	    clientwatch->owner_watch_id=id;
	    clientwatch->view=NULL;

	    /* add it to list per watch */

	    if (watch->clientwatches) watch->clientwatches->prev_per_watch=clientwatch;
	    clientwatch->next_per_watch=watch->clientwatches;
	    clientwatch->prev_per_watch=NULL;
	    watch->clientwatches=clientwatch;

	    /* add it to list per owner */

	    add_clientwatch_owner(notifyfs_owner, clientwatch);

	    /* assign owner to clientwatch */

	    clientwatch->notifyfs_owner.type=notifyfs_owner->type;

	    if (notifyfs_owner->type==NOTIFYFS_OWNERTYPE_CLIENT) {

		clientwatch->notifyfs_owner.owner.localclient=notifyfs_owner->owner.localclient;

	    } else if (notifyfs_owner->type==NOTIFYFS_OWNERTYPE_SERVER) {

		clientwatch->notifyfs_owner.owner.remoteserver=notifyfs_owner->owner.remoteserver;

	    }

	} else {

	    logoutput("add_clientwatch: unable to allocate a clientwatch");
	    nreturn=-ENOMEM;
	    goto unlock;

	}

    } else {

	/* replace existing mask: todo maybe also merge */

	replace_fseventmask(&clientwatch->fseventmask, fseventmask);

    }

    /* test there is a new mask */

    if ( merge_fseventmasks(&watch->fseventmask, fseventmask)==1) {
	int res=0;

	if (onsystemfs==0) {

	    res=set_watch_backend_os_specific(watch);

	    if (res==-1) {

		nreturn=-errno;

		if (nreturn==-EACCES) {

		    logoutput("add_clientwatch: no access to %s", pathinfo->path);

		} else {

		    logoutput("add_clientwatch: error %i setting watch on %s", nreturn, pathinfo->path);

		}

		goto unlock;

	    }

	} else {

	    res=set_watch_polling(watch);

	}

    }


    unlock:

    pthread_mutex_unlock(&watch->mutex);

    out:

    return clientwatch;

}

/* remove a clientwatch */

void remove_clientwatch_from_watch(struct clientwatch_struct *clientwatch)
{
    struct watch_struct *watch=clientwatch->watch;

    logoutput("remove_clientwatch_from_watch: nr watches %i", watch->nrwatches);

    /* detach from watch */

    pthread_mutex_lock(&watch->mutex);

    if (clientwatch->prev_per_watch) clientwatch->prev_per_watch->next_per_watch=clientwatch->next_per_watch;
    if (clientwatch->next_per_watch) clientwatch->next_per_watch->prev_per_watch=clientwatch->prev_per_watch;

    if (watch->clientwatches==clientwatch) watch->clientwatches=clientwatch->next_per_watch;

    clientwatch->prev_per_watch=NULL;
    clientwatch->next_per_watch=NULL;

    watch->nrwatches--;

    if (watch->nrwatches<=0) {

	logoutput("remove_clientwatch_from_watch: no more watches left");

	/* TODO: additional action.. */

	remove_watch_backend_os_specific(watch);

	watch->fseventmask.attrib_event=0;
	watch->fseventmask.xattr_event=0;
	watch->fseventmask.file_event=0;
	watch->fseventmask.move_event=0;
	watch->fseventmask.fs_event=0;

	/* just leave it hanging around for now.. */

	/* additional action:
	    - remove any watch on the backend if forwarded
	    howto detect that there are watches forwared??

	    watch -> mount -> backend

	    is it required to store the mount in the watch

	*/

    } else {

	clientwatch=watch->clientwatches;

	watch->fseventmask.attrib_event=0;
	watch->fseventmask.xattr_event=0;
	watch->fseventmask.file_event=0;
	watch->fseventmask.move_event=0;
	watch->fseventmask.fs_event=0;

	while(clientwatch) {

	    watch->fseventmask.attrib_event |= clientwatch->fseventmask.attrib_event;
	    watch->fseventmask.xattr_event |= clientwatch->fseventmask.xattr_event;
	    watch->fseventmask.file_event |= clientwatch->fseventmask.file_event;
	    watch->fseventmask.move_event |= clientwatch->fseventmask.move_event;
	    watch->fseventmask.fs_event |= clientwatch->fseventmask.fs_event;

	    clientwatch=clientwatch->next_per_watch;

	}

    }

    pthread_mutex_unlock(&watch->mutex);

}

void remove_clientwatch_from_owner(struct clientwatch_struct *clientwatch, unsigned char sendmessage)
{
    struct notifyfs_connection_struct *connection=NULL;

    if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct client_struct *client=clientwatch->notifyfs_owner.owner.localclient;

	lock_client(client);

	if (clientwatch->prev_per_owner) clientwatch->prev_per_owner->next_per_owner=clientwatch->next_per_owner;
	if (clientwatch->next_per_owner) clientwatch->next_per_owner->prev_per_owner=clientwatch->prev_per_owner;

	if (client->clientwatches==(void *) clientwatch) client->clientwatches=(void *) clientwatch->next_per_owner;

	if (sendmessage==1) {

    	    if ( client->status==NOTIFYFS_CLIENTSTATUS_UP ) connection=client->connection;

	}

	unlock_client(client);

    } else if (clientwatch->notifyfs_owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
	struct notifyfs_server_struct *server=clientwatch->notifyfs_owner.owner.remoteserver;

	pthread_mutex_lock(&server->mutex);

	if (clientwatch->prev_per_owner) clientwatch->prev_per_owner->next_per_owner=clientwatch->next_per_owner;
	if (clientwatch->next_per_owner) clientwatch->next_per_owner->prev_per_owner=clientwatch->prev_per_owner;

	if (server->clientwatches==(void *) clientwatch) server->clientwatches=(void *) clientwatch->next_per_owner;

	if (sendmessage==1) {

    	    if ( server->status==NOTIFYFS_BACKENDSTATUS_UP ) connection=server->connection;

	}

	pthread_mutex_unlock(&server->mutex);

    }

    clientwatch->prev_per_owner=NULL;
    clientwatch->next_per_owner=NULL;

    if (connection) {
	uint64_t unique=new_uniquectr();

	send_delwatch_message(connection->fd, unique, clientwatch->owner_watch_id);

    }

}

void remove_clientwatches_client(struct client_struct *client)
{
    struct clientwatch_struct *clientwatch=NULL;

    lock_client(client);

    clientwatch=(struct clientwatch_struct *) client->clientwatches;

    while (clientwatch) {

	remove_clientwatch_from_watch(clientwatch);

	if (clientwatch->prev_per_owner) clientwatch->prev_per_owner->next_per_owner=clientwatch->next_per_owner;
	if (clientwatch->next_per_owner) clientwatch->next_per_owner->prev_per_owner=clientwatch->prev_per_owner;

	client->clientwatches=(void *) clientwatch->next_per_owner;

	free(clientwatch);

	clientwatch=(struct clientwatch_struct *) client->clientwatches;

    }

    unlock_client(client);

}

void remove_clientwatches_server(struct notifyfs_server_struct *server)
{
    struct clientwatch_struct *clientwatch=NULL;

    pthread_mutex_lock(&server->mutex);

    clientwatch=(struct clientwatch_struct *) server->clientwatches;

    while(clientwatch) {

	remove_clientwatch_from_watch(clientwatch);

	if (clientwatch->prev_per_owner) clientwatch->prev_per_owner->next_per_owner=clientwatch->next_per_owner;
	if (clientwatch->next_per_owner) clientwatch->next_per_owner->prev_per_owner=clientwatch->prev_per_owner;

	server->clientwatches=(void *) clientwatch->next_per_owner;

	free(clientwatch);

	clientwatch=(struct clientwatch_struct *) server->clientwatches;

    }

    pthread_mutex_unlock(&server->mutex);

}

void remove_clientwatches(struct notifyfs_owner_struct *owner)
{
    if (owner->type==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct client_struct *client=owner->owner.localclient;

	remove_clientwatches_client(client);

    } else if (owner->type==NOTIFYFS_OWNERTYPE_SERVER) {
	struct notifyfs_server_struct *server=owner->owner.remoteserver;

	remove_clientwatches_server(server);

    }
}

void initialize_fsnotify_backends()
{
    initialize_inotify();
}

void close_fsnotify_backends()
{
    close_inotify();
}


