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

#include <sys/stat.h>
#include <sys/param.h>

#include <pthread.h>
#include <time.h>

#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"
#include "notifyfs.h"

#include "epoll-utils.h"

#include "socket.h"

#include "entry-management.h"
#include "logging.h"
#include "utils.h"

#include "backend.h"
#include "networkservers.h"

#include "workerthreads.h"

#include "message-base.h"
#include "message-send.h"
#include "message-receive.h"

#include "path-resolution.h"
#include "entry-management.h"
#include "client.h"
#include "handleclientmessage.h"
#include "watches.h"

#include "networkutils.h"
#include "filesystem.h"

#include "options.h"
#include "changestate.h"

extern struct notifyfs_options_struct notifyfs_options;

#define NOTIFYFS_COMMAND_UPDATE				1
#define NOTIFYFS_COMMAND_SETWATCH			2
#define NOTIFYFS_COMMAND_REGISTER			3
#define NOTIFYFS_COMMAND_SIGNOFF			4
#define NOTIFYFS_COMMAND_FSEVENT			5
#define NOTIFYFS_COMMAND_VIEW				6

struct command_register_struct {
    int mode;
    unsigned char type;
    char *data;
};

struct command_update_struct {
    struct pathinfo_struct pathinfo;
};

struct command_setwatch_struct {
    struct pathinfo_struct pathinfo;
    int owner_watch_id;
    int view;
    struct fseventmask_struct fseventmask;
};

struct command_fsevent_struct {
    int owner_watch_id;
    struct fseventmask_struct fseventmask;
    int entry;
    char *name;
    unsigned char nameallocated;
};

struct command_view_struct {
    int view;
};

struct clientcommand_struct {
    uint64_t unique;
    unsigned char type;
    struct notifyfs_owner_struct owner;
    union {
	struct command_register_struct regis;
	struct command_update_struct update;
	struct command_setwatch_struct setwatch;
	struct command_fsevent_struct fsevent;
	struct command_view_struct view;
    } command;
};

static struct workerthreads_queue_struct *workerthreads_queue=NULL;

unsigned char on_systemfs(struct notifyfs_entry_struct *entry)
{
    struct notifyfs_mount_struct *mount=get_mount(entry->mount);

    if (mount) {
	struct supermount_struct *supermount=NULL;

	supermount=find_supermount_majorminor(mount->major, mount->minor);

	if (supermount) {
	    struct notifyfs_filesystem_struct *fs=NULL;

    	    fs=supermount->fs;

	    if (fs) {

		if (fs->mode & NOTIFYFS_FILESYSTEM_SYSTEM) {

		    return 1;

		} else {

		    return 0;

		}

	    }

	}

    }

    return 0;

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
    struct supermount_struct *supermount=NULL;
    struct notifyfs_backend_struct *backend=NULL;

    supermount=find_supermount_majorminor(mount->major, mount->minor);

    if (supermount) {

	backend=supermount->backend;

    } else {

	logoutput("forward_watch_backend: no supermount found for %i:%i", mount->major, mount->minor);
	return;

    }

    if ( ! backend ) {

	logoutput("forward_watch_backend: no backend found for %i:%i - %s", mount->major, mount->minor, supermount->source);
	return;

    }

    if (backend->type==NOTIFYFS_BACKENDTYPE_SERVER){
	struct notifyfs_connection_struct *connection=backend->connection;
	struct notifyfs_filesystem_struct *fs=NULL;

    	fs=supermount->fs;

	if ( ! fs ) {

	    logoutput("forward_watch_backend: no filesystem found for %i:%i - %s", mount->major, mount->minor, fs->filesystem);
	    return;

	}

	if ( ! fs->fsfunctions) {

	    logoutput("forward_watch_backend: no fsfunctions found for %s", fs->filesystem);
	    return;

	}

	if ( ! fs->fsfunctions->construct_url) {

	    logoutput("forward_watch_backend: no construct url function found for %s", fs->filesystem);
	    return;

	}

	if (backend->status==NOTIFYFS_BACKENDSTATUS_UP && connection) {
	    struct notifyfs_entry_struct *entry=NULL;
	    struct pathinfo_struct pathinfo={NULL, 0, 0};
	    int res;

	    logoutput("forward_watch_backend: network backend found on %s", path);

	    entry=get_entry(mount->entry);
	    res=determine_path_entry(entry, &pathinfo);

	    if (pathinfo.path) {

		if (issubdirectory(path, pathinfo.path, 1)>0) {
		    int lenpath=strlen(path);
		    pathstring url;
		    uint64_t unique=new_uniquectr();

		    if (lenpath==pathinfo.len) {

			(*fs->fsfunctions->construct_url) (supermount->source, supermount->options, "/", url, sizeof(pathstring));

		    } else {

			(*fs->fsfunctions->construct_url) (supermount->source, supermount->options, path+pathinfo.len, url, sizeof(pathstring));

		    }

		    send_setwatch_message(connection->fd, unique, watch->ctr, (void *) url, -1, watch->fseventmask.attrib_event, watch->fseventmask.xattr_event, watch->fseventmask.file_event, watch->fseventmask.move_event);

		}

	    }

	    free_path_pathinfo(&pathinfo);

	}

    } else if (backend->type==NOTIFYFS_BACKENDTYPE_FUSEFS){
	struct notifyfs_connection_struct *connection=backend->connection;

	if (backend->status==NOTIFYFS_BACKENDSTATUS_UP && connection) {
	    struct notifyfs_entry_struct *entry=NULL;
	    struct pathinfo_struct pathinfo={NULL, 0, 0};
	    int res;

	    logoutput("forward_watch_backend: fuse backend found on %s", path);

	    entry=get_entry(mount->entry);
	    res=determine_path_entry(entry, &pathinfo);

	    if (pathinfo.path) {

		if (issubdirectory(path, pathinfo.path, 1)>0) {
		    int lenpath=strlen(path);
		    uint64_t unique=new_uniquectr();

		    if (lenpath==pathinfo.len) {

			send_setwatch_message(connection->fd, unique, watch->ctr, "/", -1, watch->fseventmask.attrib_event, watch->fseventmask.xattr_event, watch->fseventmask.file_event, watch->fseventmask.move_event);

		    } else {

			send_setwatch_message(connection->fd, unique, watch->ctr, path+pathinfo.len, -1, watch->fseventmask.attrib_event, watch->fseventmask.xattr_event, watch->fseventmask.file_event, watch->fseventmask.move_event);

		    }

		}

	    }

	    free_path_pathinfo(&pathinfo);

	}

    } else if (backend->type==NOTIFYFS_BACKENDTYPE_LOCALHOST) {
	struct notifyfs_entry_struct *entry=NULL;
	struct pathinfo_struct pathinfo={NULL, 0, 0};
	int res;
	struct notifyfs_filesystem_struct *fs=NULL;

    	fs=supermount->fs;

	if ( ! fs ) {

	    logoutput("forward_watch_backend: no filesystem found for %i:%i - %s", mount->major, mount->minor, fs->filesystem);
	    return;

	}

	if ( ! fs->fsfunctions) {

	    logoutput("forward_watch_backend: no fsfunctions found for %s", fs->filesystem);
	    return;

	}

	if ( ! fs->fsfunctions->get_localpath) {

	    logoutput("forward_watch_backend: no get localpath function found for %s", fs->filesystem);
	    return;

	}

	logoutput("forward_watch_backend: local backend found on %s", path);

	entry=get_entry(mount->entry);
	res=determine_path_entry(entry, &pathinfo);

	if (pathinfo.path) {

	    if (issubdirectory(path, pathinfo.path, 1)>0) {
		int lenpath=strlen(path);
		char *localpath=NULL;

		if (lenpath==pathinfo.len) {

		    localpath=(*fs->fsfunctions->get_localpath) (fs->filesystem, supermount->options, supermount->source, "");

		} else {

		    localpath=(*fs->fsfunctions->get_localpath) (fs->filesystem, supermount->options, supermount->source, path+pathinfo.len);

		}

		if (localpath) {
		    struct pathinfo_struct backend_pathinfo;
		    struct stat st;

		    logoutput("forward_watch_backend: local path %s found", localpath);

		    backend_pathinfo.path=localpath;
		    backend_pathinfo.len=strlen(localpath);
		    backend_pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

		    entry=create_notifyfs_path(&backend_pathinfo, &st, 0, 0, &res, 0, 0, 0);

		    if (entry) {
			struct notifyfs_inode_struct *inode=get_inode(entry->inode);
			struct notifyfs_owner_struct owner;
			struct timespec current_time;

			owner.type=NOTIFYFS_OWNERTYPE_SERVER;
			owner.owner.remoteserver=get_local_server();

			get_current_time(&current_time);

			if (on_systemfs(entry)==1) {

			    add_clientwatch(inode, &watch->fseventmask, watch->ctr, &owner, &backend_pathinfo, &current_time, 1);

			} else {

			    add_clientwatch(inode, &watch->fseventmask, watch->ctr, &owner, &backend_pathinfo, &current_time, 0);

			}

		    }

		    free_path_pathinfo(&backend_pathinfo);
		    localpath=NULL;

		}

	    }

	    free_path_pathinfo(&pathinfo);

	}

    }

}



void send_clientcommand_reply(struct clientcommand_struct *clientcommand, int error)
{
    struct notifyfs_connection_struct *connection=NULL;

    if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {

	connection=clientcommand->owner.owner.localclient->connection;

    } else if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_SERVER) {

	connection=clientcommand->owner.owner.remoteserver->connection;

    }

    if (connection) send_reply_message(connection->fd, clientcommand->unique, error, NULL, 0);

}


/* function to process the update command from a client 

*/

static void process_command_update(struct clientcommand_struct *clientcommand)
{
    struct command_update_struct *command_update=&(clientcommand->command.update);
    struct notifyfs_entry_struct *entry, *parent;
    struct watch_struct *watch=NULL;
    struct notifyfs_inode_struct *inode;
    struct stat st;
    int error=0;

    logoutput("process_command_update: process path %s", command_update->pathinfo.path);

    st.st_mode=0;

    if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct client_struct *client=clientcommand->owner.owner.localclient;

	/*
	    a local client: check the permissions
	*/

	parent=create_notifyfs_path(&command_update->pathinfo, &st, 2, R_OK|X_OK, &error, client->pid, client->uid, client->gid);

    } else {

	/*
	    from a remote server: no permissions checking cause that has been done there already 
	*/

	parent=create_notifyfs_path(&command_update->pathinfo, &st, 0, 0, &error, 0, 0, 0);

    }

    if (! parent || error>0) {
	error=(error>0) ? error : ENOENT;

	logoutput("process_command_update: error (%i:%s) creating path %s", error, strerror(error), command_update->pathinfo.path);
	send_clientcommand_reply(clientcommand, error);

	goto finish;

    }

    /*
	if no watch has been set the contents is not cached
    */

    inode=get_inode(parent->inode);

    if (! inode) {

	logoutput("process_command_update: inode does not exist");
	goto finish;

    }

    watch=lookup_watch_inode(inode);

    if ( ! watch) {
	struct notifyfs_attr_struct *attr;

	/* object is not watched : so check it manually */

	attr=get_attr(inode->attr);

	if ( ! attr) {

	    if (st.st_mode==0) lstat(command_update->pathinfo.path, &st);
	    attr=assign_attr(&st, inode);

	}

	if ( attr) {
	    struct timespec rightnow;

	    get_current_time(&rightnow);

	    res=sync_directory(command_update->pathinfo.path, inode, &rightnow, &st, 1);

	}

    }

    /* reply the sender */

    send_clientcommand_reply(clientcommand, 0);

    finish:

    free_path_pathinfo(&command_update->pathinfo);

}

/* 
    process a message to set a watch

    it can be a message from local client to the local server, via a local socket

    or

    it can be a message about a forwarded watch, when the remote server is the backend

    (todo: a local backend like fuse)

*/

static void process_command_setwatch(struct clientcommand_struct *clientcommand)
{
    struct command_setwatch_struct *command_setwatch=&(clientcommand->command.setwatch);
    struct notifyfs_entry_struct *entry=NULL;
    struct notifyfs_inode_struct *inode=NULL;
    int error=0;
    struct watch_struct *watch=NULL;
    struct clientwatch_struct *clientwatch=NULL;
    struct client_struct *client=NULL;
    struct view_struct *view=NULL;
    struct stat st;
    struct fseventmask_struct *fseventmask=&command_setwatch->fseventmask;
    struct fseventmask_struct zero_fseventmask=NOTIFYFS_ZERO_FSEVENTMASK;
    struct timespec current_time;

    logoutput("process_command_setwatch");

    if (command_setwatch->view>=0) {

	view=get_view(command_setwatch->view);

	if ( !view ) {

	    logoutput("process_command_setwatch: view %i not found", command_setwatch->view);
	    error=EINVAL;
	    goto error;

	} else {

	    logoutput("process_command_setwatch: view %i found", command_setwatch->view);

	}

    }

    if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {

	client=clientcommand->owner.owner.localclient;

	if (view) {

	    if (view->pid != client->pid) {

		error=EINVAL;
		logoutput("process_command_setwatch: error, client has pid %i, but view has pid %i", client->pid, view->pid);
		goto error;

	    }

	    if (view->parent_entry>=0) {

		entry=get_entry(view->parent_entry);

	    }

	}

	if (entry) {
	    int res;

	    logoutput("process_command_setwatch: entry set");

	    /* entry is known (from view): determine the path from here */

	    res=determine_path(entry, &command_setwatch->pathinfo);

	    if (res>=0) res=check_access_path(command_setwatch->pathinfo.path, client->pid, client->uid, client->gid, &st, R_OK|X_OK);

	    if (res<0) {

		error=abs(res);
		goto error;

	    }

	    logoutput("process_command_setwatch: path %s found", command_setwatch->pathinfo.path);

	} else {

	    logoutput("process_command_setwatch: entry not set, path %s", command_setwatch->pathinfo.path);

	    entry=create_notifyfs_path(&command_setwatch->pathinfo, &st, 2, R_OK, &error, client->pid, client->uid, client->gid);

	}

    } else {

	if (view) {

	    /* only a client can set a view */

	    error=EINVAL;
	    goto error;

	}

	/*
	    from a remote server: no permissions checking cause that has been done there already 
	*/

	entry=create_notifyfs_path(&command_setwatch->pathinfo, &st, 0, 0, &error, 0, 0, 0);

	if (st.st_mode==0) {

	    lstat(command_setwatch->pathinfo.path, &st);

	}

    }

    if (! entry || error>0) {

	error=(error>0) ? error : ENOENT;
	goto error;

    }

    inode=get_inode(entry->inode);

    if (view) {

	/* walk through all clientwatches used by this client */

	clientwatch=client->clientwatches;

	while(clientwatch) {

	    if (clientwatch->view==view) {

		watch=clientwatch->watch;

		if (watch->inode != inode) {

		    /*
			this view has been used for another directory before
			maybe stop additional actions here for this directory like
			polling (the poor mans fs notify method)

		    */

		    clientwatch->view=NULL;

		}

	    }

	    clientwatch=clientwatch->next_per_owner;

	}

	view->parent_entry=entry->index;

	/* make sure the view is active: it is present in the hashtable */

	activate_view(view);

    }

    /*
	check the underlying fs first to make sure the right watch is set:
	- on linux an inotify watch is not usefull on virtual filesystems like proc, sys and dev
	maybe something like capabilities of underlying fs

    */

    get_current_time(&current_time);

    if (on_systemfs(entry)==1) {

	clientwatch=add_clientwatch(inode, fseventmask, command_setwatch->owner_watch_id, &clientcommand->owner, &command_setwatch->pathinfo, &current_time, 1);

    } else {

	clientwatch=add_clientwatch(inode, fseventmask, command_setwatch->owner_watch_id, &clientcommand->owner, &command_setwatch->pathinfo, &current_time, 0);

    }

    if (clientwatch) {
	struct watch_struct *watch=clientwatch->watch;

	if (view) {

	    clientwatch->view=view;

	    /*
		set status to synclevel 1
		this means for the client that:
		- watch has been set without error
		- it can start synchronize it's own cache
	    */

	    logoutput("process_command_setwatch: set view %i to synclevel 1", view->index);

	    pthread_mutex_lock(&view->mutex);
	    view->status=NOTIFYFS_VIEWSTATUS_SYNCLEVEL1;
	    pthread_cond_broadcast(&view->cond);
	    pthread_mutex_unlock(&view->mutex);

	}

	if (notifyfs_options.listennetwork==0 && entry->mount>=0) {

	    forward_watch_backend(entry->mount, watch, command_setwatch->pathinfo.path);

	}

	if (watch->create_time.tv_sec==current_time.tv_sec && watch->create_time.tv_nsec==current_time.tv_nsec) {
	    struct notifyfs_attr_struct *attr;

	    /* watch is just created: synchronize the directory */

	    attr=get_attr(inode->attr);
	    if (! attr) attr=assign_attr(&st, inode);

	    if ( attr) {

		res=sync_directory(command_setwatch->pathinfo.path, inode, &current_time, &st, 1);

	    }

	}

	if (view) {

	    logoutput("process_command_setwatch: set view %i to synclevel 3", view->index);

	    pthread_mutex_lock(&view->mutex);
	    view->status=NOTIFYFS_VIEWSTATUS_SYNCLEVEL3;
	    pthread_cond_broadcast(&view->cond);
	    pthread_mutex_unlock(&view->mutex);

	}

    }

    finish:

    free_path_pathinfo(&command_setwatch->pathinfo);
    return;

    error:

    logoutput("process_command_setwatch: error (%i:%s)", error, strerror(error));
    send_clientcommand_reply(clientcommand, error);
    free_path_pathinfo(&command_setwatch->pathinfo);

}

/* process the register client/server */

static void process_command_register(struct clientcommand_struct *clientcommand)
{
    struct notifyfs_connection_struct *connection=NULL;

    /* just send a reply, nothing else now */

    logoutput("process_command_register");

    if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct client_struct *client=clientcommand->owner.owner.localclient;

	connection=client->connection;

	if (clientcommand->command.regis.type==NOTIFYFS_CLIENTTYPE_APP) {

	    logoutput("process_command_register: handle a normal app");

	    lock_client(client);

	    client->status=NOTIFYFS_CLIENTSTATUS_UP;
	    client->type=NOTIFYFS_CLIENTTYPE_APP;
	    client->mode=clientcommand->command.regis.mode;

	    unlock_client(client);

	} else if (clientcommand->command.regis.type==NOTIFYFS_CLIENTTYPE_FUSEFS ) {

	    /* here transform the client to a fuse fs backend */

	    lock_client(client);
	    client->status=NOTIFYFS_CLIENTSTATUS_UP;
	    client->type=NOTIFYFS_CLIENTTYPE_FUSEFS;
	    unlock_client(client);

	    /* futher action is taken when it's mounted */

	    if (clientcommand->command.regis.data) {

		logoutput("process_command_register: handle a fuse fs (data: %s)", clientcommand->command.regis.data);

		client->data=clientcommand->command.regis.data;
		clientcommand->command.regis.data=NULL;

	    } else {

		logoutput("process_command_register: handle a fuse fs (no path)");

	    }

	}

    } else if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
	struct notifyfs_server_struct *server=clientcommand->owner.owner.remoteserver;

	connection=server->connection;
	change_status_server(server, NOTIFYFS_SERVERSTATUS_UP);

    }

    /* there must be a connection... this is just action after receiving message from the client on that connection.. */

    if (connection) send_reply_message(connection->fd, clientcommand->unique, 0, NULL, 0);

}

static void process_command_signoff(struct clientcommand_struct *clientcommand)
{
    struct notifyfs_connection_struct *connection=NULL;

    /* just send a reply, nothing else now */

    logoutput("process_command_signoff");

    if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct client_struct *client=clientcommand->owner.owner.localclient;

	connection=client->connection;

	lock_client(client);
	client->status=NOTIFYFS_CLIENTSTATUS_DOWN;
	unlock_client(client);

    } else if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
	struct notifyfs_server_struct *server=clientcommand->owner.owner.remoteserver;

	connection=server->connection;

	change_status_server(server, NOTIFYFS_SERVERSTATUS_DOWN);

    }

    /* there must be a connection... this is just action after receiving message from the client on that connection.. */

    if (connection) send_reply_message(connection->fd, clientcommand->unique, 0, NULL, 0);

}


/* 
    process a message with a fsevent

    this will come from a remote server, when a watch has been forwarded to it before
    (or (todo) from a local fs (fuse) )

*/

static void process_command_fsevent(struct clientcommand_struct *clientcommand)
{
    struct command_fsevent_struct *command_fsevent=&(clientcommand->command.fsevent);
    struct fseventmask_struct *fseventmask=&command_fsevent->fseventmask;
    struct notifyfs_entry_struct *entry=NULL;
    struct watch_struct *watch=NULL;
    struct notifyfs_fsevent_struct *fsevent=NULL;

    logoutput("process_command_fsevent: received a fsevent %i:%i:%i:%i", fseventmask->attrib_event, fseventmask->xattr_event, fseventmask->file_event, fseventmask->move_event);

    /* first lookup the watch using owner id 
	there must be watch this event refers to
    */

    watch=lookup_watch_list(command_fsevent->owner_watch_id);

    if (! watch) {

	logoutput("process_command_fsevent: watch not found for id %i", command_fsevent->owner_watch_id);
	goto finish;

    }

    fsevent=evaluate_remote_fsevent(watch, fseventmask, command_fsevent->name);
    if (fsevent) queue_fsevent(fsevent);

    finish:

    if (command_fsevent->nameallocated==1) {

	if (command_fsevent->name) {

	    free(command_fsevent->name);
	    command_fsevent->name=NULL;

	}

	command_fsevent->nameallocated=0;

    }

}

static void process_command_view(struct clientcommand_struct *clientcommand)
{
    struct command_view_struct *command_view=&(clientcommand->command.view);
    struct notifyfs_entry_struct *entry=NULL;
    struct view_struct *view=NULL;
    struct stat st;
    struct pathinfo_struct pathinfo={NULL, 0, 0};
    struct client_struct *client=NULL;
    int error=0, res;

    logoutput("process_command_view");

    if (clientcommand->owner.type!=NOTIFYFS_OWNERTYPE_CLIENT) {

	/* only a client can set a view */

	error=EINVAL;
	goto finish;

    }

    client=clientcommand->owner.owner.localclient;

    view=get_view(command_view->view);

    if ( !view ) {

	logoutput("process_command_view: view %i not found", command_view->view);
	error=EINVAL;
	goto finish;

    }

    /* activate the view.. means adding to a hashtable */

    activate_view(view);

    entry=get_entry(view->parent_entry);

    if (entry) {

	/*
	    check the permissions
        */

	res=determine_path(entry, &pathinfo);

	if (res==0) {

	    res=check_access_path(pathinfo.path, client->pid, client->uid, client->gid, &st, R_OK|X_OK);

	    if (res<0) error=-res;

	} else if (res<0) {

	    error=-res;

	}

    }

    if (! entry || error>0) {

	error=(error>0) ? error : ENOENT;

	if (error==EACCES) {

	    logoutput("process_command_view: client has not enough permissions");

	} else if (error==ENOENT) {

	    logoutput("process_command_view: path does not exist");

	} else {

	    if (pathinfo.path) {

		logoutput("process_command_view: error %i:%s when access path %s", error, strerror(error), pathinfo.path);

	    } else {

		logoutput("process_command_view: error %i:%s when determing path", error, strerror(error));

	    }

	}

    } else {

	if ( ! S_ISDIR(st.st_mode)) {

	    logoutput("process_command_view: %s is not a directory, can only view a directory", pathinfo.path);
	    error=ENOTDIR;

	}

    }

    finish:

    /* when error then ready */

    if (error>0) {

	send_clientcommand_reply(clientcommand, error);

    } else {
	struct watch_struct *watch=NULL;
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	/* a valid view */

	send_clientcommand_reply(clientcommand, 0);

	pthread_mutex_lock(&view->mutex);
	view->status=NOTIFYFS_VIEWSTATUS_SYNCLEVEL1;
	pthread_cond_broadcast(&view->cond);
	pthread_mutex_unlock(&view->mutex);

	watch=lookup_watch_inode(inode);

	if ( ! watch) {
	    struct timespec current_time;
	    struct notifyfs_attr_struct *attr;

	    /* object is not watched : so check it manually */

	    attr=get_attr(inode->attr);

	    if ( ! attr) {

		/* stat st is set while checking permissions .. */

		attr=assign_attr(&st, inode);

	    }

	    if (attr) {
		struct timespec rightnow;

		get_current_time(&rightnow);

		res=sync_directory(pathinfo.path, inode, &rightnow, &st, 1);

	    }

	}

	pthread_mutex_lock(&view->mutex);
	view->status=NOTIFYFS_VIEWSTATUS_SYNCLEVEL2;
	pthread_cond_broadcast(&view->cond);
	pthread_mutex_unlock(&view->mutex);

    }

    free_path_pathinfo(&pathinfo);

}



static void process_command(void *data)
{
    struct clientcommand_struct *clientcommand=(struct clientcommand_struct *) data;

    if (clientcommand->type==NOTIFYFS_COMMAND_UPDATE) {

	process_command_update(clientcommand);

    } else if (clientcommand->type==NOTIFYFS_COMMAND_SETWATCH ) {

	process_command_setwatch(clientcommand);

    } else if (clientcommand->type==NOTIFYFS_COMMAND_REGISTER ) {

	process_command_register(clientcommand);

    } else if (clientcommand->type==NOTIFYFS_COMMAND_SIGNOFF ) {

	process_command_signoff(clientcommand);

    } else if (clientcommand->type==NOTIFYFS_COMMAND_FSEVENT ) {

	process_command_fsevent(clientcommand);

    } else if (clientcommand->type==NOTIFYFS_COMMAND_VIEW ) {

	process_command_view(clientcommand);

    }

    free(clientcommand);

}

/* function to handle the register message from a local client */

void handle_register_message(int fd, void *data,  struct notifyfs_register_message *register_message, void *buff, int len, unsigned char typedata)
{
    struct clientcommand_struct *clientcommand=NULL;

    if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	logoutput("handle register message: received a register from client");

	if (register_message->type==NOTIFYFS_CLIENTTYPE_FUSEFS) {

	    if ( ! buff ) {

		logoutput("handle register message: received a register from a fuse fs, but buffer empty");
		goto error;

	    }

	}

    } else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

	logoutput("handle register message: received a register from server");

    } else {

	logoutput("handle register message: received a register from unknown sender, cannot continue");
	goto error;

    }

    if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct client_struct *client=NULL;

	client=(struct client_struct *) data;

	logoutput("handle register message, client pid %i, message pid %i tid %i", client->pid, register_message->pid, register_message->tid);

	/* check the pid and the tid the client has send, and the tid earlier detected is a task of the mainpid */

	if ( belongtosameprocess(register_message->pid, register_message->tid)==0 ) {

	    logoutput("handle register message: pid %i and tid %i send by client (%i) are not part of same process", register_message->pid, register_message->tid, client->tid);
	    goto error;

	}

	if ( belongtosameprocess(register_message->pid, client->tid)==0 ) {

	    logoutput("handle register message: pid %i send by client and tid %i are not part of same process", register_message->pid, client->tid);
	    goto error;

	}

	client->pid=register_message->pid;


    }

    /* process the register futher in a thread */

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=register_message->unique;

	clientcommand->command.regis.type=register_message->type;
	clientcommand->command.regis.data=NULL;

	if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;
	    clientcommand->owner.owner.localclient=(struct client_struct *) data;

	    if (register_message->type==NOTIFYFS_CLIENTTYPE_FUSEFS) {

	    	if (buff) {

	    	    clientcommand->command.regis.data=strdup((char *) buff);

	    	    if (! clientcommand->command.regis.data) {

	    		logoutput("handle_register_message: no memory to allocate %s", (char *) buff);
	    		goto error;

	    	    }

	    	}

	    }

	} else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;
	    clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;


	}

	clientcommand->type=NOTIFYFS_COMMAND_REGISTER;
	clientcommand->command.regis.mode=register_message->mode;

	workerthread=get_workerthread(workerthreads_queue);

	if (workerthread) {

	    workerthread->processevent_cb=process_command;
	    workerthread->data=(void *) clientcommand;

	    signal_workerthread(workerthread);

	} else {

	    /* no free thread.... what now ?? */

	    logoutput("handle_register_message: no free thread...");
	    goto error;

	}

    } else {

	logoutput("handle_register_message: cannot allocate clientcommand");

    }

    return;

    error:

    if (clientcommand) {

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}

void handle_signoff_message(int fd, void *data, struct notifyfs_signoff_message *signoff_message, void *buff, int len, unsigned char typedata)
{
    struct clientcommand_struct *clientcommand=NULL;

    if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	logoutput("handle signoff message: received a signoff from client");

    } else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

	logoutput("handle signoff message: received a signoff from server");

    } else {

	logoutput("handle signoff message: received a  signoff from unknown sender, cannot continue");
	goto error;

    }

    /* process the register futher in a thread */

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=signoff_message->unique;

	if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;

	    clientcommand->owner.owner.localclient=(struct client_struct *) data;

	} else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;

	    clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;

	}

	clientcommand->type=NOTIFYFS_COMMAND_SIGNOFF;

	workerthread=get_workerthread(workerthreads_queue);

	if (workerthread) {

	    workerthread->processevent_cb=process_command;
	    workerthread->data=(void *) clientcommand;

	    signal_workerthread(workerthread);

	} else {

	    /* no free thread.... what now ?? */

	    logoutput("handle_signoff_message: no free thread...");
	    goto error;

	}

    } else {

	logoutput("handle_signoff_message: cannot allocate clientcommand");

    }

    return;

    error:

    if (clientcommand) {

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}

void handle_update_message(int fd, void *data, struct notifyfs_update_message *update_message, void *buff, int len, unsigned char typedata)
{
    struct clientcommand_struct *clientcommand=NULL;

    if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	logoutput("handle update message: received an update from client");

    } else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

	logoutput("handle update message: received an update from server");

    } else {

	logoutput("handle update message: received an update from unknown sender, cannot continue");
	goto error;

    }

    if (!buff) {

	logoutput("handle update message: buffer is empty, cannot continue");
	goto error;

    }

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=update_message->unique;

	if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;
	    clientcommand->owner.owner.localclient=(struct client_struct *) data;

	} else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;
	    clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;

	}

	clientcommand->type=NOTIFYFS_COMMAND_UPDATE;

	/* path is first part of buffer
	*/

	clientcommand->command.update.pathinfo.flags=0;
	clientcommand->command.update.pathinfo.len=0;

	clientcommand->command.update.pathinfo.path=strdup((char *) buff);

	if ( ! clientcommand->command.update.pathinfo.path) {

	    logoutput("handle_update_message: no memory to allocate %s", (char *) buff);
	    goto error;

	}

	clientcommand->command.update.pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;
	clientcommand->command.update.pathinfo.len=len;

	workerthread=get_workerthread(workerthreads_queue);

	if (workerthread) {

	    /* make a workerthread process the list command for client */

	    workerthread->processevent_cb=process_command;
	    workerthread->data=(void *) clientcommand;

	    signal_workerthread(workerthread);

	} else {

	    /* no free thread.... what now ?? */

	    logoutput("handle update message: no free thread...");
	    goto error;

	}

    } else {

	logoutput("handle update message: cannot allocate clientcommand");

    }

    return;

    error:

    if (clientcommand) {

	free_path_pathinfo(&clientcommand->command.update.pathinfo);

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}

void handle_setwatch_message(int fd, void *data, struct notifyfs_setwatch_message *setwatch_message, void *buff, int len, unsigned char typedata)
{
    struct clientcommand_struct *clientcommand=NULL;

    if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	logoutput("handle setwatch message: received a setwatch from client");

    } else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

	logoutput("handle setwatch message: received a setwatch from server");

    } else {

	logoutput("handle setwatch message: received a setwatch from unknown sender, cannot continue");
	goto error;

    }

    /* some sanity checks */

    if (setwatch_message->watch_id<=0) {

	logoutput("handle setwatch message: error: watch id not positive..");
	goto error;

    }

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=setwatch_message->unique;

	if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;
	    clientcommand->owner.owner.localclient=(struct client_struct *) data;

	} else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;
	    clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;

	}

	clientcommand->type=NOTIFYFS_COMMAND_SETWATCH;

	clientcommand->command.setwatch.pathinfo.path=NULL;
	clientcommand->command.setwatch.pathinfo.flags=0;
	clientcommand->command.setwatch.pathinfo.len=0;

	if (buff) {

	    if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

		/* path is first part of buffer */

		clientcommand->command.setwatch.pathinfo.path=strdup((char *) buff);

	    } else if (typedata==NOTIFYFS_OWNERTYPE_SERVER) {

		clientcommand->command.setwatch.pathinfo.path=process_notifyfsurl((char *) buff);

	    }

	    if ( ! clientcommand->command.setwatch.pathinfo.path) {

		logoutput("handle_setwatch_message: no memory to allocate %s", (char *) buff);
		goto error;

	    }

	}

	clientcommand->command.setwatch.pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;
	clientcommand->command.setwatch.pathinfo.len=strlen(clientcommand->command.setwatch.pathinfo.path);

	clientcommand->command.setwatch.owner_watch_id=setwatch_message->watch_id;

	clientcommand->command.setwatch.fseventmask.attrib_event=setwatch_message->fseventmask.attrib_event;
	clientcommand->command.setwatch.fseventmask.xattr_event=setwatch_message->fseventmask.xattr_event;
	clientcommand->command.setwatch.fseventmask.move_event=setwatch_message->fseventmask.move_event;
	clientcommand->command.setwatch.fseventmask.file_event=setwatch_message->fseventmask.file_event;
	clientcommand->command.setwatch.fseventmask.fs_event=setwatch_message->fseventmask.fs_event;

	clientcommand->command.setwatch.view=setwatch_message->view;

	workerthread=get_workerthread(workerthreads_queue);

	if (workerthread) {

	    workerthread->processevent_cb=process_command;
	    workerthread->data=(void *) clientcommand;

	    signal_workerthread(workerthread);

	} else {

	    /* no free thread.... what now ?? */

	    logoutput("handle_setwatch_message: no free thread...");
	    goto error;

	}

    } else {

	logoutput("handle_setwatch_message: cannot allocate clientcommand");

    }

    return;

    error:

    if (clientcommand) {

	free_path_pathinfo(&clientcommand->command.setwatch.pathinfo);

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}

/*
    handle an fsevent message coming from a client (which is actually not possible) or a remote server
*/

void handle_fsevent_message(int fd, void *data, struct notifyfs_fsevent_message *fsevent_message, void *buff, int len, unsigned char typedata)
{
    struct clientcommand_struct *clientcommand=NULL;

    if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	logoutput("handle fsevent message: received an fsevent from client: impossible");
	goto error;

    } else if (typedata==NOTIFYFS_OWNERTYPE_BACKEND) {

	logoutput("handle fsevent message: received an fsevent from server/backend");

    } else {

	logoutput("handle fsevent message: received an fsevent from unknown sender, cannot continue");
	goto error;

    }

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;
	struct notifyfs_entry_struct *entry=NULL;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=fsevent_message->unique;
	clientcommand->owner.type=NOTIFYFS_OWNERTYPE_BACKEND;
	clientcommand->owner.owner.backend=(struct notifyfs_backend_struct *) data;

	clientcommand->type=NOTIFYFS_COMMAND_FSEVENT;

	clientcommand->command.fsevent.owner_watch_id=fsevent_message->watch_id;

	clientcommand->command.fsevent.fseventmask.attrib_event=fsevent_message->fseventmask.attrib_event;
	clientcommand->command.fsevent.fseventmask.xattr_event=fsevent_message->fseventmask.xattr_event;
	clientcommand->command.fsevent.fseventmask.move_event=fsevent_message->fseventmask.move_event;
	clientcommand->command.fsevent.fseventmask.file_event=fsevent_message->fseventmask.file_event;
	clientcommand->command.fsevent.fseventmask.fs_event=fsevent_message->fseventmask.fs_event;

        if (fsevent_message->entry>=0) {

	    /* this will probably fail since client or servers sending this fsevent message are usually not using
		the shared memory
	    */

	    entry=get_entry(fsevent_message->entry);

	}

	if (! entry) {

	    if (fsevent_message->indir==1) {

		if ( ! buff ) {

		    logoutput("handle_fsevent_message: error, event in directory, but buff not defined");
		    goto error;

		}

		clientcommand->command.fsevent.name=strdup((char *) buff);

		if (! clientcommand->command.fsevent.name) {

		    logoutput("handle_fsevent_message: error, unable to allocate memory for %s", (char *) buff);
		    goto error;

		}

		clientcommand->command.fsevent.nameallocated=1;

	    }

	}

	workerthread=get_workerthread(workerthreads_queue);

	if (workerthread) {

	    workerthread->processevent_cb=process_command;
	    workerthread->data=(void *) clientcommand;

	    signal_workerthread(workerthread);

	} else {

	    /* no free thread.... what now ?? */

	    logoutput("handle_fsevent_message: no free thread...");
	    goto error;

	}

    } else {

	logoutput("handle_fsevent_message: cannot allocate clientcommand");

    }

    return;

    error:

    if (clientcommand) {

	if (clientcommand->command.fsevent.nameallocated==1) {

	    if (clientcommand->command.fsevent.name) {

		free(clientcommand->command.fsevent.name);
		clientcommand->command.fsevent.name=NULL;

	    }

	    clientcommand->command.fsevent.nameallocated=0;

	}

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}

void handle_view_message(int fd, void *data, struct notifyfs_view_message *view_message, void *buff, int len, unsigned char typedata)
{
    struct clientcommand_struct *clientcommand=NULL;

    if (typedata==NOTIFYFS_OWNERTYPE_CLIENT) {

	logoutput("handle view message: received a view from client");

    } else if (typedata==NOTIFYFS_OWNERTYPE_SERVER || typedata==NOTIFYFS_OWNERTYPE_BACKEND) {

	logoutput("handle view message: received a view from server");

	/*  view messages only from a client */

	goto error;

    } else {

	logoutput("handle view message: received a view from unknown sender, cannot continue");
	goto error;

    }

    /* some sanity checks */

    if (view_message->view<0) {

	logoutput("handle view message: error: view index not positive..");
	goto error;

    }

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=view_message->unique;

	/* owner is always a (local) client */

	clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;
	clientcommand->owner.owner.localclient=(struct client_struct *) data;
	clientcommand->type=NOTIFYFS_COMMAND_VIEW;

	clientcommand->command.view.view=view_message->view;

	workerthread=get_workerthread(workerthreads_queue);

	if (workerthread) {

	    workerthread->processevent_cb=process_command;
	    workerthread->data=(void *) clientcommand;

	    signal_workerthread(workerthread);

	} else {

	    /* no free thread.... what now ?? */

	    logoutput("handle_view_message: no free thread...");
	    goto error;

	}

    } else {

	logoutput("handle_view_message: cannot allocate clientcommand");

    }

    return;

    error:

    if (clientcommand) {

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}


void init_handleclientmessage(struct workerthreads_queue_struct *threads_queue)
{

    workerthreads_queue=threads_queue;

    assign_notifyfs_message_cb_register(handle_register_message);
    assign_notifyfs_message_cb_signoff(handle_signoff_message);
    assign_notifyfs_message_cb_update(handle_update_message);
    assign_notifyfs_message_cb_setwatch(handle_setwatch_message);
    assign_notifyfs_message_cb_fsevent(handle_fsevent_message);
    assign_notifyfs_message_cb_view(handle_view_message);

}
