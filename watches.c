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

#ifdef HAVE_INOTIFY
#include <sys/inotify.h>
#endif

#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_WATCHES

#define WATCHES_TABLESIZE          1024

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "epoll-utils.h"
#include "notifyfs-io.h"
#include "notifyfs.h"

#include "workerthreads.h"

#include "entry-management.h"
#include "path-resolution.h"
#include "options.h"
#include "mountinfo.h"
#include "message.h"
#include "message-server.h"
#include "client-io.h"
#include "client.h"
#include "watches.h"
#include "changestate.h"
#include "utils.h"
#include "socket.h"
#include "networkutils.h"

#ifdef HAVE_INOTIFY
#include "watches-backend-inotify.c"
#else
#include "watches-backend-inotify-notsup.c"
#endif

struct watch_struct *first_watch;
struct watch_struct *last_watch;
struct watch_struct *watch_table[WATCHES_TABLESIZE];

unsigned long watchctr = 1;
struct watch_struct *watch_list=NULL;
pthread_mutex_t watchctr_mutex=PTHREAD_MUTEX_INITIALIZER;

static int check_pathlen(struct notifyfs_entry_struct *entry)
{
    int len=1;
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

static char *determine_path_custom(struct notifyfs_entry_struct *entry)
{
    char *pos;
    int nreturn=0, len;
    char *name;
    char *path;

    if (isrootentry(entry)==1) {

	nreturn=3;

    } else {

	nreturn=check_pathlen(entry);
	if (nreturn<0) goto out;

    }

    path=malloc(nreturn);

    if ( ! path) {

	nreturn=-ENOMEM;
	goto out;

    }

    pos=path+nreturn-1;
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

    return path;

}


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

struct watch_struct *lookup_watch(struct notifyfs_inode_struct *inode)
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

void add_watch_to_list(struct watch_struct *watch)
{

    pthread_mutex_lock(&watchctr_mutex);

    if (watch_list) watch_list->prev=watch;
    watch->next=watch_list;
    watch->prev=NULL;
    watch_list=watch;

    watchctr++;

    watch->ctr=watchctr;

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

void set_watch_backend_os_specific(struct watch_struct *watch, char *path)
{
    set_watch_backend_inotify(watch, path);
}

void change_watch_backend_os_specific(struct watch_struct *watch, char *path)
{
    change_watch_backend_inotify(watch, path);
}

void remove_watch_backend_os_specific(struct watch_struct *watch)
{
    remove_watch_backend_inotify(watch);
}

void init_notifyfs_fsevent(struct notifyfs_fsevent_struct *fsevent)
{

    fsevent->status=0;

    fsevent->fseventmask.attrib_event=0;
    fsevent->fseventmask.xattr_event=0;
    fsevent->fseventmask.file_event=0;
    fsevent->fseventmask.move_event=0;
    fsevent->fseventmask.fs_event=0;

    fsevent->entry=NULL;
    fsevent->path=NULL;
    fsevent->pathallocated=0;

    fsevent->detect_time.tv_sec=0;
    fsevent->detect_time.tv_nsec=0;
    fsevent->process_time.tv_sec=0;
    fsevent->process_time.tv_nsec=0;

    fsevent->lock_entry=NULL;
    fsevent->watch=NULL;
    fsevent->mount_entry=NULL;

    fsevent->next=NULL;
    fsevent->prev=NULL;

}

void destroy_notifyfs_fsevent(struct notifyfs_fsevent_struct *fsevent)
{

    if (fsevent->pathallocated==1) {

	if (fsevent->path) {

	    free(fsevent->path);
	    fsevent->path=NULL;

	}

	fsevent->pathallocated=0;

    }

    free(fsevent);

}

unsigned char compare_fseventmasks(struct fseventmask_struct *maska, struct fseventmask_struct *maskb)
{
    unsigned char differ=0;

    if (maska->attrib_event != maskb->attrib_event) {

	differ=1;
	goto out;

    }

    if (maska->xattr_event != maskb->xattr_event) {

	differ=1;
	goto out;

    }

    if (maska->file_event != maskb->file_event) {

	differ=1;
	goto out;

    }

    if (maska->move_event != maskb->move_event) {

	differ=1;
	goto out;

    }

    if (maska->fs_event != maskb->fs_event) {

	differ=1;

    }

    out:

    return differ;

}

/* merge two fsevent masks */

unsigned char merge_fseventmasks(struct fseventmask_struct *maska, struct fseventmask_struct *maskb)
{
    unsigned char differ=0;

    if (compare_fseventmasks(maska, maskb)==1) {
	struct fseventmask_struct maskc;

	maskc.attrib_event = maska->attrib_event | maskb->attrib_event;
	maskc.xattr_event = maska->xattr_event | maskb->xattr_event;
	maskc.file_event = maska->file_event | maskb->file_event;
	maskc.move_event = maska->move_event | maskb->move_event;
	maskc.fs_event = maska->fs_event | maskb->fs_event;

	if (compare_fseventmasks(maska, &maskc)==1) {

	    maska->attrib_event = maskc.attrib_event;
	    maska->xattr_event = maskc.xattr_event;
	    maska->file_event = maskc.file_event;
	    maska->move_event = maskc.move_event;
	    maska->fs_event = maskc.fs_event;

	    differ=1;

	}

    }

    return differ;

}

/* replace one fseventmask by another */

void replace_fseventmask(struct fseventmask_struct *maska, struct fseventmask_struct *maskb)
{

    maska->attrib_event = maskb->attrib_event;
    maska->xattr_event = maskb->xattr_event;
    maska->file_event = maskb->file_event;
    maska->move_event = maskb->move_event;
    maska->fs_event = maskb->fs_event;

}

void send_setwatch_message_remote(struct notifyfs_server_struct *notifyfs_server, struct watch_struct *watch, char *path)
{

    logoutput("send_setwatch_message_remote: todo");

}

/*
    function to test a mount has a backend, and if so, forward the watch to that backend

    the path on the remote host depends on the "root" of what has been shared (smb) or exported (nfs)

    for example a nfs export from 192.168.0.2:/usr/share
    is mounted at /data for example

    now if a watch is set on /data/some/dir/to/watch, the corresponding path on 192.168.0.2 is
    /usr/share/some/dir/to/watch

    this is simple for nfs, it's a bit difficult for smb: the "rootpath" of the share is in
    netbioslanguage, and is not a directory. For example:
    the source of a mount is //mainserver/public, so the "share" name is public. For this 
    host it's impossible to detect what directory/path this share is build with. So, it sends
    the share name to the notifyfs server on mainserver like:

    cifs:/public/some/dir/to/watch

    and let it to the notifyfs process on mainserver to find out what directory the share
    public is on (by parsing the smb.conf file for example)

    for sshfs this can be also very complicated

    using sshfs like:

    sshfs sbon@192.168.0.2:/ ~/mount -o allow_other

    works ok, since it mounts the root (/) at ~/mount, but it's a bit more complicated when

    sshfs sbon@192.168.0.2: ~/mount -o allow_other

    will take the home directory on 192.168.0.2 of sbon as root.

    Now this host is also not able to determine the home of sbon on 192.168.0.2. To handle this case

    a template is send:

    sshfs:sbon@%HOME%/some/dir/to/watch

*/


static void forward_watch_backend(int mountindex, struct watch_struct *watch, char *path)
{
    struct notifyfs_mount_struct *mount=get_mount(mountindex);
    struct notifyfs_server_struct *notifyfs_server=get_mount_backend(mount);

    logoutput("forward_watch_backend");

    if (notifyfs_server) {

	/* here to test allow errors */

	if (notifyfs_server->status==NOTIFYFS_SERVERSTATUS_UP || notifyfs_server->status==NOTIFYFS_SERVERSTATUS_ERROR) {
	    char *mountpoint=NULL;
	    struct notifyfs_entry_struct *entry=NULL;
	    logoutput("forward_watch_backend: remote server found");

	    /* path must be a subdirectory of mountpoint of mount */

	    entry=get_entry(mount->entry);

	    mountpoint=determine_path_custom(entry);

	    if (mountpoint) {

		if (issubdirectory(path, mountpoint, 1)==1) {
		    int lenpath=strlen(path);
		    int lenmountpoint=strlen(mountpoint);

		    if (lenpath==lenmountpoint) {
			pathstring url;

			/* send a watch on the root of remote backend */

			/* determine what to send ?? 
			    test the filesystem
			*/

			determine_remotepath(mount, "/", url, sizeof(pathstring));

		    } else {
			pathstring url;

			determine_remotepath(mount, path+lenmountpoint, url, sizeof(pathstring));

		    }

		}

	    }

	}

    }

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



void add_clientwatch(struct notifyfs_inode_struct *inode, struct fseventmask_struct *fseventmask, int id, struct notifyfs_owner_struct *notifyfs_owner, char *path, int mountindex)
{
    struct clientwatch_struct *clientwatch=NULL;
    struct watch_struct *watch=NULL;
    int new_fsevent_mask, new_type;
    unsigned char watchcreated=0, fseventmask_changed=0;

    if (path) {

	logoutput("add_clientwatch: on %s client watch id %i, %i:%i:%i:%i", path, id, fseventmask->attrib_event, fseventmask->xattr_event, fseventmask->file_event, fseventmask->move_event);

    } else {

	logoutput("add_clientwatch: on UNKNOWN client watch id %i, %i:%i:%i:%i", id, fseventmask->attrib_event, fseventmask->xattr_event, fseventmask->file_event, fseventmask->move_event);

    }

    watch=lookup_watch(inode);

    if ( ! watch ) {

	logoutput("add_clientwatch: no watch found, creating one");

	watch=malloc(sizeof(struct watch_struct));

	if (watch) {

	    watch->ctr=0;
	    watch->inode=inode;

	    watch->fseventmask.attrib_event=0;
	    watch->fseventmask.xattr_event=0;
	    watch->fseventmask.file_event=0;
	    watch->fseventmask.move_event=0;
	    watch->fseventmask.fs_event=0;

	    watch->nrwatches=0;
	    watch->clientwatches=NULL;

	    pthread_mutex_init(&watch->mutex, NULL);
	    pthread_cond_init(&watch->cond, NULL);

	    watch->lock=0;

	    watch->next_hash=NULL;
	    watch->prev_hash=NULL;

	    watch->next=NULL;
	    watch->prev=NULL;

	    add_watch_to_table(watch);
	    add_watch_to_list(watch);

	    watchcreated=1;

	} else {

	    logoutput("add_clientwatch: unable to allocate a watch");
	    goto out;

	}

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

	    clientwatch->fseventmask.attrib_event=fseventmask->attrib_event;
	    clientwatch->fseventmask.xattr_event=fseventmask->xattr_event;
	    clientwatch->fseventmask.file_event=fseventmask->file_event;
	    clientwatch->fseventmask.move_event=fseventmask->move_event;
	    clientwatch->fseventmask.fs_event=fseventmask->fs_event;

	    clientwatch->watch=watch;

	    clientwatch->owner_watch_id=id;

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
	    goto unlock;

	}

    } else {

	/* replace existing mask: todo maybe also merge */

	clientwatch->fseventmask.attrib_event=fseventmask->attrib_event;
	clientwatch->fseventmask.xattr_event=fseventmask->xattr_event;
	clientwatch->fseventmask.file_event=fseventmask->file_event;
	clientwatch->fseventmask.move_event=fseventmask->move_event;
	clientwatch->fseventmask.fs_event=fseventmask->fs_event;

    }

    /* test there is a new mask */

    if ( merge_fseventmasks(&watch->fseventmask, fseventmask)==1) {
	struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	if ( S_ISDIR(attr->cached_st.st_mode)) {
	    unsigned char fullsync=0;
	    struct stat st;

	    /* check the directory is up to date */

	    if (lstat(path, &st)==0) {

		if (attr->mtim.tv_sec<st.st_mtim.tv_sec || (attr->mtim.tv_sec==st.st_mtim.tv_sec && attr->mtim.tv_nsec<st.st_mtim.tv_nsec)) fullsync=1;

	    } else {

		logoutput("add_clientwatch: error %i setting watch on %s", errno, path);

		/* TODO: add action to react on this event */

	    }

	    if (fullsync==1) {
		struct notifyfs_entry_struct *parent=get_entry(inode->alias);
		struct timespec rightnow;
		int res;

		get_current_time(&rightnow);

		/* a full sync, because this directory is not in cache yet, or no watch and the contents has changed */

		res=sync_directory_full(path, parent, &rightnow);

		if (res==-ENOENT) {

		    logoutput("add_clientwatch: error %i setting watch on %s", res, path);

		    /* additional action required: check what happened, why is this enoent?? */

		} else if (res==-ENOTDIR) {

		    logoutput("add_clientwatch: error %i setting watch on %s", res, path);

		    /* additional action required: correct the fs..*/

		} else if (res<0) {

		    logoutput("add_clientwatch: error %i setting watch on %s", res, path);

		} else {

		    attr->mtim.tv_sec=st.st_mtim.tv_sec;
		    attr->mtim.tv_nsec=st.st_mtim.tv_nsec;

		    remove_old_entries(parent, &rightnow);

		}

	    } else {
		struct notifyfs_entry_struct *parent=get_entry(inode->alias);
		struct timespec rightnow;

		get_current_time(&rightnow);

		sync_directory_simple(path, parent, &rightnow);

	    }

	}

	set_watch_backend_os_specific(watch, path);

	/* test it's on a network or fuse fs */

	if (mountindex>=0) forward_watch_backend(mountindex, watch, path);

    }

    unlock:

    pthread_mutex_unlock(&watch->mutex);

    out:

    return;

}

/* remove a clientwatch */

void remove_clientwatch_from_watch(struct clientwatch_struct *clientwatch)
{
    struct watch_struct *watch=clientwatch->watch;

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

	lock_notifyfs_server(server);

	if (clientwatch->prev_per_owner) clientwatch->prev_per_owner->next_per_owner=clientwatch->next_per_owner;
	if (clientwatch->next_per_owner) clientwatch->next_per_owner->prev_per_owner=clientwatch->prev_per_owner;

	if (server->clientwatches==(void *) clientwatch) server->clientwatches=(void *) clientwatch->next_per_owner;

	if (sendmessage==1) {

    	    if ( server->status==NOTIFYFS_SERVERSTATUS_UP ) connection=server->connection;

	}


	unlock_notifyfs_server(server);

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

    lock_notifyfs_server(server);

    clientwatch=(struct clientwatch_struct *) server->clientwatches;

    while(clientwatch) {

	remove_clientwatch_from_watch(clientwatch);

	if (clientwatch->prev_per_owner) clientwatch->prev_per_owner->next_per_owner=clientwatch->next_per_owner;
	if (clientwatch->next_per_owner) clientwatch->next_per_owner->prev_per_owner=clientwatch->prev_per_owner;

	server->clientwatches=(void *) clientwatch->next_per_owner;

	free(clientwatch);

	clientwatch=(struct clientwatch_struct *) server->clientwatches;

    }

    unlock_notifyfs_server(server);

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

int sync_directory_full(char *path, struct notifyfs_entry_struct *parent, struct timespec *sync_time)
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
	unsigned char attrcreated=0;

	memcpy(tmppath, path, lenpath);
	*(tmppath+lenpath)='/';

	parent_inode=get_inode(parent->inode);

	while((de=readdir(dp))) {

	    /* here check the entry exist... */

	    name=de->d_name;

	    if ( strcmp(name, ".")==0 ) {

		continue;

	    } else if ( strcmp(name, "..")==0 ) {

		continue;

	    }

	    lenname=strlen(name);

	    /* add this entry to the base path */

	    memcpy(tmppath+lenpath+1, name, lenname);
	    *(tmppath+lenpath+1+lenname)='\0';

	    /* read the stat */

	    res=lstat(tmppath, &st);

	    /* huh?? */

	    if ( res==-1 ) {

		continue;

		/* here additional action: lookup the entry and if exist: a delete */

	    }

	    entry=find_entry_raw(parent, parent_inode, name, 1, create_entry);

	    if ( entry ) {

		if (entry->inode==-1) assign_inode(entry);

		if (entry->inode>=0) {

		    nreturn++;

		    inode=get_inode(entry->inode);

		    if (inode->attr>=0) {

			attr=get_attr(inode->attr);
			attrcreated=0;

			copy_stat(&attr->cached_st, &st);
			copy_stat_times(&attr->cached_st, &st);

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

void remove_old_entries(struct notifyfs_entry_struct *parent, struct timespec *sync_time)
{
    struct notifyfs_entry_struct *entry, *next_entry;
    struct notifyfs_inode_struct *inode, *parent_inode;
    struct notifyfs_attr_struct *attr;

    logoutput("remove_old_entries");

    parent_inode=get_inode(parent->inode);

    entry=get_next_entry(parent, NULL);

    while (entry) {

	inode=get_inode(entry->inode);
	attr=get_attr(inode->attr);

	next_entry=get_next_entry(parent, entry);

	if (attr) {

	    /* if stime is less then parent entry access it's is gone */

	    if (attr->atim.tv_sec<sync_time->tv_sec ||(attr->atim.tv_sec==sync_time->tv_sec && attr->atim.tv_nsec<sync_time->tv_nsec) ) {
		char *name=get_data(entry->name);

		logoutput("remove_old_entries: remove %s", name);

		remove_entry_from_name_hash(entry);
		remove_entry(entry);

		/* TODO:
		correct the fs & signals */

	    }

	} else {

	    remove_entry_from_name_hash(entry);
	    remove_entry(entry);

	}

	entry=next_entry;

    }

}

void sync_directory_simple(char *path, struct notifyfs_entry_struct *parent, struct timespec *sync_time)
{
    struct notifyfs_entry_struct *entry, *next_entry;
    struct notifyfs_inode_struct *inode, *parent_inode;
    struct notifyfs_attr_struct *attr;
    char *name;
    int lenpath=strlen(path), lenname;
    char tmppath[lenpath+256];
    struct stat st;

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

		remove_entry_from_name_hash(entry);
		remove_entry(entry);

		/* TODO: here send messages .... */

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

		copy_stat(&attr->cached_st, &st);
		copy_stat_times(&attr->cached_st, &st);

		/* TODO: here compare the mtim and ctim to get an event */

		attr->atim.tv_sec=sync_time->tv_sec;
		attr->atim.tv_nsec=sync_time->tv_nsec;

	    }

	}

	entry=get_next_entry(parent, entry);

    }

}

