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
#include <dirent.h>

#include <sys/stat.h>
#include <sys/param.h>

#include <pthread.h>
#include <time.h>

#include "notifyfs-io.h"
#include "notifyfs.h"

#include "entry-management.h"
#include "logging.h"
#include "utils.h"

#include "workerthreads.h"
#include "message.h"
#include "handlemessage.h"

#include "path-resolution.h"
#include "epoll-utils.h"
#include "message-server.h"
#include "client-io.h"
#include "client.h"
#include "handleclientmessage.h"
#include "watches.h"
#include "socket.h"
#include "networkutils.h"

#define NOTIFYFS_COMMAND_UPDATE		1
#define NOTIFYFS_COMMAND_SETWATCH	2
#define NOTIFYFS_COMMAND_REGISTER	3
#define NOTIFYFS_COMMAND_SIGNOFF	4
#define NOTIFYFS_COMMAND_FSEVENT	5

struct command_update_struct {
    char *path;
    unsigned char pathallocated;
};

struct command_setwatch_struct {
    char *path;
    unsigned char pathallocated;
    int owner_watch_id;
    struct fseventmask_struct fseventmask;
};

struct command_fsevent_struct {
    int owner_watch_id;
    struct fseventmask_struct fseventmask;
};

struct clientcommand_struct {
    uint64_t unique;
    unsigned char type;
    struct notifyfs_owner_struct owner;
    union {
	struct command_update_struct update;
	struct command_setwatch_struct setwatch;
	struct command_fsevent_struct fsevent;
    } command;
};

static struct workerthreads_queue_struct *global_workerthreads_queue=NULL;


/* function to process the update command from a client 

*/

static void process_command_update(struct clientcommand_struct *clientcommand)
{
    struct call_info_struct call_info;
    struct command_update_struct *command_update=&(clientcommand->command.update);
    struct notifyfs_entry_struct *entry, *parent;
    struct watch_struct *watch=NULL;
    struct notifyfs_inode_struct *inode;
    struct stat st;

    logoutput("process_command_update: process path %s", command_update->path);

    init_call_info(&call_info, NULL);
    call_info.path=command_update->path;

    /* make sure the path does exist (and lookup the entry and get stat) */

    create_notifyfs_path(&call_info, &st);

    parent=call_info.entry;

    if (! parent) {
        struct notifyfs_connection_struct *connection=NULL;

	if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {

	    connection=clientcommand->owner.owner.localclient->connection;

	} else if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_SERVER) {

	    connection=clientcommand->owner.owner.remoteserver->connection;

	}

	if (connection) {
	    logoutput("process_command_update: error creating path %s", call_info.path);

	    /* here correct the fs 
		and send an error
	    */

	    send_reply_message(connection->fd, clientcommand->unique, ENOENT, NULL, 0);
	    goto finish;

	}

    }

    /* if no watch has been set the contents is not cached

	TODO: check the watch is watching "enough"

    */

    inode=get_inode(parent->inode);
    watch=lookup_watch(inode);

    if ( ! watch) {
	struct timespec current_time;
	struct notifyfs_attr_struct *attr;

	/* object is not watched : so check it manually */

	attr=get_attr(inode->attr);

	if (attr->mtim.tv_sec<st.st_mtim.tv_sec || 
	    (attr->mtim.tv_sec==st.st_mtim.tv_sec && attr->mtim.tv_nsec<st.st_mtim.tv_nsec) ) {
	    int res;
	    struct timespec rightnow;

	    get_current_time(&rightnow);

	    /* when contents is changed (mtime is newer than the latest access) sync it */

	    res=sync_directory_full(call_info.path, parent, &rightnow);

	    if (res>=0) {


		remove_old_entries(parent, &rightnow);

	    } else {

		/* */

		logoutput("process_command_update: error %i sync directory %s", res, call_info.path);

	    }

	} else {
	    struct timespec rightnow;

	    get_current_time(&rightnow);

	    sync_directory_simple(call_info.path, parent, &rightnow);

	}

    }

    /* reply the sender */

    if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {
	struct notifyfs_connection_struct *connection=clientcommand->owner.owner.localclient->connection;

	if (connection) send_reply_message(connection->fd, clientcommand->unique, 0, NULL, 0);

    } else if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
	struct notifyfs_connection_struct *connection=clientcommand->owner.owner.remoteserver->connection;

	if (connection) send_reply_message(connection->fd, clientcommand->unique, 0, NULL, 0);

    }

    finish:

    if (command_update->pathallocated==1) {

	if (command_update->path) {

	    free(command_update->path);
	    command_update->path=NULL;

	}

	command_update->pathallocated=0;

    }

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
    struct call_info_struct call_info;
    struct command_setwatch_struct *command_setwatch=&(clientcommand->command.setwatch);
    struct notifyfs_entry_struct *entry;

    logoutput("process_command_setwatch");

    init_call_info(&call_info, NULL);


    call_info.path=command_setwatch->path;

    /* make sure the path does exist (and lookup the entry) */

    create_notifyfs_path(&call_info, NULL);

    entry=call_info.entry;

    if (! entry) {
        struct notifyfs_connection_struct *connection=NULL;

	if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_CLIENT) {

	    connection=clientcommand->owner.owner.localclient->connection;

	} else if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_SERVER) {

	    connection=clientcommand->owner.owner.remoteserver->connection;

	}

	logoutput("process_command_setwatch: error creating path %s", call_info.path);

	/* here correct the fs 
	    and send an error
	*/

	if (connection) {

	    send_reply_message(connection->fd, clientcommand->unique, ENOENT, NULL, 0);
	    goto finish;

	}

    }

    if (command_setwatch->owner_watch_id>0) {
	struct notifyfs_inode_struct *inode;

	inode=get_inode(entry->inode);

	/* with a local sender, add the mountindex also */

	add_clientwatch(inode, &command_setwatch->fseventmask, command_setwatch->owner_watch_id, &clientcommand->owner, command_setwatch->path, call_info.mount);

    }

    finish:

    if (command_setwatch->pathallocated==1) {

	if (command_setwatch->path) {

	    free(command_setwatch->path);
	    command_setwatch->path=NULL;

	}

	command_setwatch->pathallocated=0;

    }

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

	lock_client(client);
	client->status=NOTIFYFS_CLIENTSTATUS_UP;
	unlock_client(client);

    } else if (clientcommand->owner.type==NOTIFYFS_OWNERTYPE_SERVER) {
	struct notifyfs_server_struct *server=clientcommand->owner.owner.remoteserver;

	connection=server->connection;

	lock_notifyfs_server(server);
	server->status=NOTIFYFS_SERVERSTATUS_UP;
	unlock_notifyfs_server(server);

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

	lock_notifyfs_server(server);
	server->status=NOTIFYFS_SERVERSTATUS_DOWN;
	unlock_notifyfs_server(server);

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
    struct fseventmask_struct *fseventmask=&(command_fsevent->fseventmask);

    logoutput("process_command_fsevent: received a fsevent %i:%i:%i:%i", fseventmask->attrib_event, fseventmask->xattr_event, fseventmask->file_event, fseventmask->move_event);

    /* here find the right watch */

    /* owner is known!!! 

    */

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

    }

    free(clientcommand);

}

/* function to handle the register message from a local client */

void handle_register_message(int fd, void *data,  struct notifyfs_register_message *register_message, void *buff, int len, unsigned char remote)
{
    struct clientcommand_struct *clientcommand=NULL;

    if (remote==0) {
	struct client_struct *client=NULL;

	client=(struct client_struct *) data;

	logoutput("handle register message, client pid %i, message pid %i tid %i", client->pid, register_message->pid, register_message->tid);

	/* check the pid and the tid the client has send, and the tid earlier detected is a task of the mainpid */

	if ( belongtosameprocess(register_message->pid, register_message->tid)==0 ) {

	    logoutput("handle register message: pid %i and tid %i send by client (%i) are not part of same process", register_message->pid, register_message->tid, client->tid);

	    return;

	}

	if ( belongtosameprocess(register_message->pid, client->tid)==0 ) {

	    logoutput("handle register message: pid %i send by client and tid %i are not part of same process", register_message->pid, client->tid);

	    return;

	}

	client->pid=register_message->pid;

    }

    /* process the register futher in a thread */

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=register_message->unique;

	if (remote==0) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;

	    clientcommand->owner.owner.localclient=(struct client_struct *) data;

	} else {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;

	    clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;

	}

	clientcommand->type=NOTIFYFS_COMMAND_REGISTER;

	workerthread=get_workerthread(global_workerthreads_queue);

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

void handle_signoff_message(int fd, void *data, struct notifyfs_signoff_message *signoff_message, void *buff, int len, unsigned char remote)
{
    struct clientcommand_struct *clientcommand=NULL;

    logoutput("handle signoff message");

    /* process the register futher in a thread */

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=signoff_message->unique;

	if (remote==0) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;

	    clientcommand->owner.owner.localclient=(struct client_struct *) data;

	} else {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;

	    clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;

	}

	clientcommand->type=NOTIFYFS_COMMAND_SIGNOFF;

	workerthread=get_workerthread(global_workerthreads_queue);

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

void handle_update_message(int fd, void *data, struct notifyfs_update_message *update_message, void *buff, int len, unsigned char remote)
{
    struct clientcommand_struct *clientcommand=NULL;

    if (buff) {

	clientcommand=malloc(sizeof(struct clientcommand_struct));

	if (clientcommand) {
	    struct workerthread_struct *workerthread;

	    memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	    clientcommand->unique=update_message->unique;

	    if (remote==0) {

		clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;

		clientcommand->owner.owner.localclient=(struct client_struct *) data;

	    } else {

		clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;

		clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;

	    }

	    clientcommand->type=NOTIFYFS_COMMAND_UPDATE;

	    /* path is first part of buffer
	    */

	    clientcommand->command.update.pathallocated=0;

	    clientcommand->command.update.path=strdup((char *) buff);

	    if ( ! clientcommand->command.update.path) {

		logoutput("handle_update_message: no memory to allocate %s", (char *) buff);
		goto error;

	    }

	    clientcommand->command.update.pathallocated=1;

	    workerthread=get_workerthread(global_workerthreads_queue);

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

    }

    return;

    error:

    if (clientcommand) {

	if (clientcommand->command.update.pathallocated==1) {

	    free(clientcommand->command.update.path);
	    clientcommand->command.update.path=NULL;
	    clientcommand->command.update.pathallocated=0;

	}

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}

void handle_setwatch_message(int fd, void *data, struct notifyfs_setwatch_message *setwatch_message, void *buff, int len, unsigned char remote)
{
    struct clientcommand_struct *clientcommand=NULL;


    /* some sanity checks */

    if (setwatch_message->watch_id<=0) {

	logoutput("handle setwatch message: error: watch id not positive..");
	return;

    }

    if (buff) {

	clientcommand=malloc(sizeof(struct clientcommand_struct));

	if (clientcommand) {
	    struct workerthread_struct *workerthread;

	    memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	    clientcommand->unique=setwatch_message->unique;

	    if (remote==0) {

		clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;

		clientcommand->owner.owner.localclient=(struct client_struct *) data;

	    } else {

		clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;

		clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;

	    }

	    clientcommand->type=NOTIFYFS_COMMAND_SETWATCH;

	    clientcommand->command.setwatch.path=NULL;
	    clientcommand->command.setwatch.pathallocated=0;

	    if (remote==0) {

		/* path is first part of buffer */

		clientcommand->command.setwatch.path=strdup((char *) buff);

	    } else {

		clientcommand->command.setwatch.path=process_notifyfsurl((char *) buff);

	    }

	    if ( ! clientcommand->command.setwatch.path) {

		logoutput("handle_setwatch_message: no memory to allocate %s", (char *) buff);
		goto error;

	    }

	    clientcommand->command.setwatch.pathallocated=1;

	    clientcommand->command.setwatch.owner_watch_id=setwatch_message->watch_id;

	    clientcommand->command.setwatch.fseventmask.attrib_event=setwatch_message->fseventmask.attrib_event;
	    clientcommand->command.setwatch.fseventmask.xattr_event=setwatch_message->fseventmask.xattr_event;
	    clientcommand->command.setwatch.fseventmask.move_event=setwatch_message->fseventmask.move_event;
	    clientcommand->command.setwatch.fseventmask.file_event=setwatch_message->fseventmask.file_event;
	    clientcommand->command.setwatch.fseventmask.fs_event=setwatch_message->fseventmask.fs_event;

	    workerthread=get_workerthread(global_workerthreads_queue);

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

    }

    return;

    error:

    if (clientcommand) {

	if (clientcommand->command.setwatch.pathallocated==1) {

	    free(clientcommand->command.setwatch.path);
	    clientcommand->command.setwatch.path=NULL;
	    clientcommand->command.setwatch.pathallocated=0;

	}

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}

void handle_fsevent_message(int fd, void *data, struct notifyfs_fsevent_message *fsevent_message, void *buff, int len, unsigned char remote)
{
    struct clientcommand_struct *clientcommand=NULL;

    logoutput("handle fsevent message:");

    clientcommand=malloc(sizeof(struct clientcommand_struct));

    if (clientcommand) {
	struct workerthread_struct *workerthread;

	memset(clientcommand, 0, sizeof(struct clientcommand_struct));

	clientcommand->unique=fsevent_message->unique;

	if (remote==0) {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_CLIENT;

	    clientcommand->owner.owner.localclient=(struct client_struct *) data;

	} else {

	    clientcommand->owner.type=NOTIFYFS_OWNERTYPE_SERVER;

	    clientcommand->owner.owner.remoteserver=(struct notifyfs_server_struct *) data;

	}

	clientcommand->type=NOTIFYFS_COMMAND_FSEVENT;

	workerthread=get_workerthread(global_workerthreads_queue);

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

	free(clientcommand);
	clientcommand=NULL;

    }

    return;

}

void init_handleclientmessage(struct workerthreads_queue_struct *workerthreads_queue)
{

    global_workerthreads_queue=workerthreads_queue;

    assign_notifyfs_message_cb_register(handle_register_message);
    assign_notifyfs_message_cb_signoff(handle_signoff_message);
    assign_notifyfs_message_cb_update(handle_update_message);
    assign_notifyfs_message_cb_setwatch(handle_setwatch_message);
    assign_notifyfs_message_cb_fsevent(handle_fsevent_message);

}
