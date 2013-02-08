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

#include <pthread.h>
#include <time.h>

#include "notifyfs-io.h"
#include "notifyfs.h"

#include "entry-management.h"
#include "logging.h"
#include "utils.h"
#include "path-resolution.h"
#include "epoll-utils.h"
#include "message.h"
#include "watches.h"
#include "socket.h"
#include "client-io.h"
#include "client.h"

struct client_struct *clients_list=NULL;
pthread_mutex_t clients_list_mutex=PTHREAD_MUTEX_INITIALIZER;

struct client_struct *create_client(unsigned int fd, pid_t pid, uid_t uid, gid_t gid, unsigned char type)
{
    struct client_struct *client=NULL;

    client=malloc(sizeof(struct client_struct));

    if ( client ) {

	client->connection=NULL;
	client->type=type;

	/* pid maybe the tid */

	client->tid=pid;
	client->pid=pid; /* unsure yet, will be confirmed later */

	client->uid=uid;
	client->gid=gid;

	client->next=NULL;
	client->prev=NULL;

	pthread_mutex_init(&client->mutex, NULL);

	client->status=NOTIFYFS_CLIENTSTATUS_NOTSET;

	client->clientwatches=NULL;

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

struct client_struct *register_client(unsigned int fd, pid_t pid, uid_t uid, gid_t gid, unsigned char type)
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

	client=create_client(fd, pid, uid, gid, type);

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

}

void unlock_client(struct client_struct *client)
{
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

    if ( res==0 && S_ISDIR(st.st_mode)) isrunning=1;

    return isrunning;

}

