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
#include <sys/socket.h>
#include <sys/epoll.h>

#include <pthread.h>
#include <time.h>

#include <fuse/fuse_lowlevel.h>

#include "notifyfs.h"

#include "entry-management.h"
#include "logging.h"
#include "utils.h"
#include "path-resolution.h"
#include "epoll-utils.h"
#include "message.h"
#include "watches.h"
#include "client-io.h"
#include "client.h"
#include "workerthreads.h"

struct client_struct *clients_list=NULL;
pthread_mutex_t clients_list_mutex=PTHREAD_MUTEX_INITIALIZER;

struct client_struct *create_client(unsigned int fd, pid_t pid, uid_t uid, gid_t gid)
{
    struct client_struct *client=NULL;

    client=malloc(sizeof(struct client_struct));

    if ( client ) {

	client->fd=fd;

	client->buff0size=sizeof(struct notifyfs_message_body);
	client->recvbuffsize=NOTIFYFS_SERVER_RECVBUFFERSIZE;
	client->sendbuffsize=NOTIFYFS_SERVER_SENDBUFFERSIZE;

	/* pid maybe the tid */

	client->tid=pid;
	client->pid=pid; /* unsure yet, will be confirmed later */

	client->uid=uid;
	client->gid=gid;

	client->next=NULL;
	client->prev=NULL;

	pthread_mutex_init(&client->mutex, NULL);
	pthread_cond_init(&client->cond, NULL);
	client->lock=0;

	client->status=NOTIFYFS_CLIENTSTATUS_NOTSET;
	client->messagemask=0;

    }

    return client;

}

/* function to remove a client app */

void remove_client(struct client_struct *client)
{
    int res;

    /* lock client */

    lock_client(client);

    client->status=NOTIFYFS_CLIENTSTATUS_DOWN;

    /* unlock client 
       do not destroy it yet, this is done when the client really disconnects from socket */

    unlock_client(client);

}

/* function to register a client

   important to first check the client is already in the list, so registered befor
   to use is of course the pid as unique key 

*/

struct client_struct *register_client(unsigned int fd, pid_t pid, uid_t uid, gid_t gid)
{
    struct client_struct *client=NULL;

    logoutput("register_client: for pid %i", (int) pid);

    pthread_mutex_lock(&clients_list_mutex);

    /* check existing clients */

    client=clients_list;

    while (client) {

        if ( client->pid==pid ) break;
        client=client->next;

    }

    pthread_mutex_unlock(&clients_list_mutex);

    if ( client ) {

        logoutput("register_client: client for pid %i does already exist", (int) pid);

    } else {

	client=create_client(fd, pid, uid, gid);

	/* insert at begin of list */

	if ( client) {

	    pthread_mutex_lock(&clients_list_mutex);

	    if ( clients_list ) clients_list->prev=client;
	    client->next=clients_list;
	    client->prev=NULL;
	    clients_list=client;

	    pthread_mutex_unlock(&clients_list_mutex);

	}

    }

    unlock:

    return client;

}

struct client_struct *lookup_client_unlocked(pid_t pid)
{
    struct client_struct *client=NULL;

    client=clients_list;

    while (client) {

        if ( client->pid==pid ) break;

	/* if client has different threads, check these also
           they report a different pid*/

	if (belongtosameprocess(client->pid, pid)==1) break;

        client=client->next;

    }

    return client;

}

struct client_struct *lookup_client_locked(pid_t pid)
{
    struct client_struct *client=NULL;

    pthread_mutex_lock(&clients_list_mutex);

    client=lookup_client_unlocked(pid);

    pthread_mutex_unlock(&clients_list_mutex);

    return client;

}


struct client_struct *lookup_client(pid_t pid, unsigned char lockset)
{
    struct client_struct *client=NULL;

    if (lockset==0) {

	client=lookup_client_locked(pid);

    } else {

	client=lookup_client_unlocked(pid);

    }

    return client;

}

int lock_clientslist()
{
    return pthread_mutex_lock(&clients_list_mutex);
}

int unlock_clientslist()
{
    return pthread_mutex_unlock(&clients_list_mutex);
}

struct client_struct *get_next_client(struct client_struct *client)
{
    return (client) ? client->next : clients_list;
}

void lock_client(struct client_struct *client)
{
    pthread_mutex_lock(&client->mutex);

    if ( client->lock==1 ) {

        while (client->lock==1) {

    	    pthread_cond_wait(&client->cond, &client->mutex);

        }

    }

    client->lock=1;

    pthread_mutex_unlock(&client->mutex);

}

void unlock_client(struct client_struct *client)
{

    pthread_mutex_lock(&client->mutex);
    client->lock=0;
    pthread_cond_broadcast(&client->cond);
    pthread_mutex_unlock(&client->mutex);

}

unsigned char check_client_is_running(struct client_struct *client)
{
    unsigned char isrunning=0;
    char testpath[32];
    int res=0;
    struct stat st;

    res=snprintf(testpath, 32, "/proc/%i", client->pid);

    res=stat(testpath, &st);

    if ( res!=-1 && S_ISDIR(st.st_mode)) isrunning=1;

    return isrunning;

}

void handle_register_message(struct client_struct *client,  struct notifyfs_register_message *register_message, void *data1, int len1)
{

    logoutput("handle register message, client pid %i, message pid %i tid %i", client->pid, register_message->pid, register_message->tid);

    /* check the pid and the tid the client has send, and the tid earlier detected is a task of the mainpid */

    if ( belongtosameprocess(register_message->pid, register_message->tid)==0 ) {

	logoutput("handle register message: pid %i and tid %i send by client (%i) are not part of same process", register_message->pid, register_message->tid, client->tid);

	/* ignore message.. */

	return;

    }

    if ( belongtosameprocess(register_message->pid, client->tid)==0 ) {

	logoutput("handle register message: pid %i send by client and tid %i are not part of same process", register_message->pid, client->tid);

	/* ignore message.. */

	return;

    }

    if ( register_message->type==NOTIFYFS_MESSAGE_REGISTER_SIGNIN ) {

	client->pid=register_message->pid;
	client->messagemask=register_message->messagemask;

    } else if ( register_message->type==NOTIFYFS_MESSAGE_REGISTER_SIGNOFF ) {

	remove_client(client);

    }

}

void get_directory_entries(char *path, struct notifyfs_entry_struct *parent)
{
    DIR *dp=NULL;
    int lenpath;
    pathstring tmppath;
    struct notifyfs_entry_struct *entry;

    entry=get_next_entry(parent, NULL);

    while(entry) {

	entry->synced=0;
	entry=get_next_entry(parent, entry);

    }

    lenpath=strlen(path);
    memcpy(tmppath, path, lenpath);
    *(tmppath+lenpath)='/';

    dp=opendir(path);

    if ( dp ) {
	struct dirent *de;
	char *name;
	int res, lenname=0;
	unsigned char entrycreated=0, docompare=1;
	struct stat st;

	*(tmppath+lenpath)='/';

	while(1) {

	    de=readdir(dp);

	    if ( ! de ) break;

	    /* here check the entry exist... */

	    name=de->d_name;
	    lenname=strlen(name);

	    if ( strcmp(name, ".")==0 ) {

		continue;

	    } else if ( strcmp(name, "..")==0 ) {

		continue;

	    }


	    /* add this entry to the base path */

	    memcpy(tmppath+lenpath+1, name, lenname);
	    *(tmppath+lenpath+1+lenname)='\0';

	    /* read the stat */

	    logoutput("get_directory_entries: read stat for %s", tmppath);

	    res=stat(tmppath, &st);

	    if ( res==-1 ) continue;

	    /* 	find the matching entry in this fs 
		if not found create it */

	    entry=find_entry(parent->inode->ino, name);
	    entrycreated=0;
	    docompare=1;

	    if ( ! entry ) {

		entry=create_entry(parent, name, NULL);

		if (entry) {

		    assign_inode(entry);

		    if (entry->inode) {

			entrycreated=1;

			add_to_name_hash_table(entry);
			add_to_inode_hash_table(entry->inode);

		    } else {

			remove_entry(entry);

			/* ignore this (memory) error */

			continue;

		    }

		} else {

		    continue;

		}

	    }

	    copy_stat(&(entry->inode->st), &st);

	}

	closedir(dp);

    }

}

void send_listreply_message(int fd, uint64_t unique, char *buffer, int sizereply, int count, unsigned char last)
{
    struct notifyfs_message_body message;
    int lenpath;

    message.type=NOTIFYFS_MESSAGE_TYPE_LISTREPLY;

    message.body.listreply_message.unique=unique;
    message.body.listreply_message.sizereply=sizereply;
    message.body.listreply_message.count=count;
    message.body.listreply_message.last=last;

    logoutput("send_listreply_message: ctr %li", (long int)message.body.listreply_message.unique);

    send_message(fd, &message, (void *) buffer, NOTIFYFS_SERVER_SENDBUFFERSIZE);

}

/* function to process the list command from a client:

    - make sure the path does exist
    - make sure the contents of that directory is cached
    - send the client a buffer with entries
    - and .... ?

    */

static void process_command_list(struct clientcommand_struct *clientcommand)
{
    struct call_info_struct call_info;
    struct command_list_struct *command_list=&(clientcommand->command.list);
    struct notifyfs_entry_struct *entry;

    init_call_info(&call_info, NULL);

    call_info.path=command_list->path;

    /* make sure the path does exist (and lookup the entry) */

    create_notifyfs_path(&call_info);

    if (! call_info.entry) {

	logoutput("process_command_list: errror creating path %s", call_info.path);
	goto finish;

    }

    /* if no watch has been set the contents is not cached
	there are better ways to do this
	some field in entry ...
    */

    if (! call_info.entry->inode->effective_watch) {

	get_directory_entries(call_info.path, call_info.entry);

    }

    /* get a batch of entries to send to the client */

    entry=find_entry_raw(call_info.entry, command_list->name, 0);

    if ( ! entry) {

	logoutput("process_command_list: no entries found in %s", call_info.path);

	send_listreply_message(clientcommand->client->fd, clientcommand->unique, NULL, 0, 0, 1);

	goto finish;

    } else {
	char *buff=clientcommand->client->sendbuff;
	int buffsize=clientcommand->client->sendbuffsize;
	struct client_messageentry_struct messageentry, *buffentry;
	int lenname, sizemessage=sizeof(struct client_messageentry_struct);
	int count=0;
	struct notifyfs_entry_struct *firstentry=entry;
	unsigned char typeentry;

	typeentry=command_list->typeentry;

	while (1) {
	    char *pos=buff;
	    struct stat *st;

	    memset(buff, '\0', buffsize);

	    while (entry) {

		lenname=strlen(entry->name);

		/* does it fit ? */

		if (pos+lenname+sizemessage+1>=buff+buffsize) break;

		st=&(entry->inode->st);

		if (typeentry==NOTIFYFS_MESSAGEENTRYTYPE_DIR) {

		    /* skip entries which are not a dir */

		    if ( ! S_ISDIR(st->st_mode)) {

			entry=get_next_entry(call_info.entry, entry);
			continue;

		    }

		} else {

		    if ( S_ISDIR(st->st_mode)) {

			entry=get_next_entry(call_info.entry, entry);
			continue;

		    }

		}

		logoutput("process_command_list: write messageentry %i at %i, lenname %i", count, pos-buff, lenname);

		messageentry.mode=st->st_mode;
		messageentry.uid=st->st_uid;
		messageentry.gid=st->st_gid;
		messageentry.size=st->st_size;

		messageentry.ctime.tv_sec=st->st_ctim.tv_sec;
		messageentry.ctime.tv_nsec=st->st_ctim.tv_nsec;

		messageentry.mtime.tv_sec=st->st_mtim.tv_sec;
		messageentry.mtime.tv_nsec=st->st_mtim.tv_nsec;

		messageentry.atime.tv_sec=st->st_atim.tv_sec;
		messageentry.atime.tv_nsec=st->st_atim.tv_nsec;

		messageentry.len=lenname;

		memcpy(pos, &messageentry, sizemessage);

		buffentry=(struct client_messageentry_struct *) pos;

		memcpy(buffentry->name, entry->name, lenname);

		count++;

		if (count>=command_list->maxentries) break;

		pos+=sizemessage+lenname;

		*pos='\0'; /* terminate name with zero */
		pos++;

		entry=get_next_entry(call_info.entry, entry);

	    }

	    /* send a message */

	    if (count>=command_list->maxentries) {

		/* maximum entries send : the last */

		send_listreply_message(clientcommand->client->fd, clientcommand->unique, buff, pos-buff, count, 1);

		break;

	    } else if (entry) {

		/* still entries available but buffer is full */

		send_listreply_message(clientcommand->client->fd, clientcommand->unique, buff, pos-buff, count, 0);

	    } else {

		if (typeentry==NOTIFYFS_MESSAGEENTRYTYPE_DIR) {

		    /* walk through the index again, but then only the non directories */

		    entry=firstentry;
		    typeentry==NOTIFYFS_MESSAGEENTRYTYPE_NONDIR;

		} else {

		    /* no entries anymore : the last */

		    send_listreply_message(clientcommand->client->fd, clientcommand->unique, buff, pos-buff, count, 1);
		    break;

		}

	    }

	}

    }

    finish:

    if (command_list->path) {

	free(command_list->path);
	command_list->path=NULL;

    }

    if (command_list->name) {

	free(command_list->name);
	command_list->name=NULL;

    }

}

static void process_command(void *data)
{
    struct clientcommand_struct *clientcommand=(struct clientcommand_struct *) data;

    if (clientcommand->type==NOTIFYFS_CLIENTCOMMAND_LIST) {

	process_command_list(clientcommand);

    }

    free(clientcommand);

}

/*

*/

void handle_list_message(struct client_struct *client,  struct notifyfs_list_message *list_message, void *data, int len)
{

    if (data) {
	struct clientcommand_struct *clientcommand;

	clientcommand=malloc(sizeof(struct clientcommand_struct));

	if (clientcommand) {
	    struct workerthread_struct *workerthread;
	    int lenpath;

	    clientcommand->unique=list_message->unique;
	    clientcommand->client=client;
	    clientcommand->type=NOTIFYFS_CLIENTCOMMAND_LIST;

	    /* path is first part of data 
		name is second
		note path and name are zero terminated
	    */

	    clientcommand->command.list.maxentries=list_message->maxentries;
	    clientcommand->command.list.typeentry=list_message->typeentry;

	    clientcommand->command.list.path=strdup((char *) data);

	    if ( ! clientcommand->command.list.path) {

		logoutput("handle_list_message: no memory to allocate %s", (char *) data);
		free(clientcommand);
		return;

	    }

	    lenpath=strlen(clientcommand->command.list.path);

	    clientcommand->command.list.name=NULL;

	    if (len>lenpath+1) {

		clientcommand->command.list.name=strdup((char *) (data+lenpath+1));

	    }

	    workerthread=get_thread_from_queue(0);

	    if (workerthread) {

		/* make a workerthread process the list command for client */

		workerthread->processevent_cb=process_command;
		workerthread->data=(void *) clientcommand;

		signal_workerthread(workerthread);

	    } else {

		/* no free thread.... what now ?? */

		logoutput("handle list message: no free thread...");

		if (clientcommand->command.list.path) free(clientcommand->command.list.path);
		if (clientcommand->command.list.name) free(clientcommand->command.list.name);

		free(clientcommand);

	    }

	} else {

	    logoutput("handle list message: cannot allocate command_list ");

	}

    }

}


/* function to receive a message, reacting on data on fd via callbacks*/

void receive_message(int fd, struct client_struct *client)
{
    struct msghdr msg;
    int nreturn=0;
    struct iovec iov[2];
    ssize_t lenread;

    logoutput("receive_message");

    readbuffer:

    /* prepare msg */

    msg.msg_iov=(struct iovec *) client->recvbuff;

    iov[0].iov_base=(void *) client->buff0;
    iov[0].iov_len=client->buff0size;

    iov[1].iov_base=(void *) client->recvbuff;
    iov[1].iov_len=client->recvbuffsize;

    msg.msg_iov=iov;
    msg.msg_iovlen=2;

    msg.msg_control=NULL;
    msg.msg_controllen=0;

    msg.msg_name=NULL;
    msg.msg_namelen=0;

    lenread=recvmsg(fd, &msg, MSG_WAITALL);

    logoutput("receive_message: msg_controllen %i msg_iovlen %i lenread %i", msg.msg_controllen, msg.msg_iovlen, (int)lenread);

    if ( lenread<0 ){

	if (lenread<0) logoutput("receive_message: error %i recvmsg", errno);

    } else if ( msg.msg_controllen==0 ) {

	/* a normal message */

	/* first part is message body */

	struct notifyfs_message_body *message_body=(struct notifyfs_message_body *) msg.msg_iov[0].iov_base;

	if ( message_body->type==NOTIFYFS_MESSAGE_TYPE_REGISTER ) {
	    struct notifyfs_register_message *register_message;

	    logoutput("received a register client message");

	    register_message=(struct notifyfs_register_message *) &(message_body->body.register_message);

	    handle_register_message(client, register_message, msg.msg_iov[1].iov_base, msg.msg_iov[1].iov_len);


	} else if ( message_body->type==NOTIFYFS_MESSAGE_TYPE_LIST ) {
	    struct notifyfs_list_message *list_message;

	    logoutput("received a list message");

	    list_message=(struct notifyfs_list_message *) &(message_body->body.list_message);

	    handle_list_message(client, list_message, msg.msg_iov[1].iov_base, msg.msg_iov[1].iov_len);

	}

	goto readbuffer;

    }

}

void handle_data_on_client_fd(int fd, void *data, uint32_t events)
{

    if ( events & ( EPOLLHUP | EPOLLRDHUP ) ) {
	struct client_struct *client=(struct client_struct *) data;
	struct epoll_extended_data_struct *epoll_xdata;

	close(fd);

	epoll_xdata=scan_eventloop(fd, 0, NULL);

	if (epoll_xdata) {

	    remove_xdata_from_epoll(epoll_xdata, NULL);
	    remove_xdata_from_list(epoll_xdata, 0, NULL);

	    free(epoll_xdata);

	}

	if ( client ) {

	    /* set client to down */

	    client->status=NOTIFYFS_CLIENTSTATUS_DOWN;
	    client->fd=0;

	}

    } else {
        struct client_struct *client=(struct client_struct *) data;

        /* normal io: here handle the data available for read */

	logoutput("handle_data_on_client_fd: process message");

	receive_message(fd, client);

    }

}

/*

    function to react on connection on socket:

    add a client and add the fd to the eventloop

*/

int client_socketfd_callback(int fd, pid_t pid, uid_t uid, gid_t gid)
{
    struct client_struct *client=NULL;

    /* check for client */

    client=register_client(fd, pid, uid, gid);

    if ( client ) {
	struct epoll_extended_data_struct *epoll_xdata;

	/* add to epoll */

	epoll_xdata=add_to_epoll(fd, EPOLLIN | EPOLLET, TYPE_FD_CLIENT, &handle_data_on_client_fd, (void *) client, NULL, NULL);

	if ( ! epoll_xdata ) {

	    logoutput("error adding client fd %i to mainloop", fd);

	} else {

	    add_xdata_to_list(epoll_xdata, NULL);

	}

    }

    return 0;

}

