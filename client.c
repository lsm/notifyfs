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
#include <errno.h>
#include <err.h>

#include <inttypes.h>
#include <ctype.h>

#include <sys/stat.h>
#include <sys/param.h>

#include <pthread.h>

#include <fuse/fuse_lowlevel.h>

#include "entry-management.h"
#include "logging.h"
#include "mountinfo.h"
#include "client.h"


#define LOG_LOGAREA LOG_LOGAREA_SOCKET


struct client_struct *clients_list=NULL;
pthread_mutex_t clients_list_mutex=PTHREAD_MUTEX_INITIALIZER;


/* function to register a client

   important to first check the client is already in the list, so registered befor
   to use is of course the pid as unique key 

*/

struct client_struct *register_client(unsigned int fd, pid_t pid, uid_t uid, gid_t gid)
{
    struct client_struct *client=NULL;
    int res=0, nreturn=0;


    logoutput1("register_client for pid %i", (int) pid);


    res=pthread_mutex_lock(&clients_list_mutex);

    /* check existing clients */

    client=clients_list;
    while (client) {

        if ( client->pid==pid ) break;
        client=client->next;

    }
    if ( client ) {

        /* here also a log message */
        nreturn=-EEXIST;
        goto unlock;

    }

    /* no existing: create a new one
       here create a mech. to get from unused list using trylock */

    client=malloc(sizeof(struct client_struct));

    if ( ! client ) {

        nreturn=-ENOMEM;
        goto unlock;

    }

    memset(client, 0, sizeof(struct client_struct));

    client->type=NOTIFYFS_CLIENTTYPE_UNKNOWN; /* has to be set later when client sends a register message */
    client->fd=fd;
    client->pid=pid;
    client->uid=uid;
    client->gid=gid;

    client->next=NULL;
    client->prev=NULL;

    pthread_mutex_init(&client->lock_mutex, NULL);
    pthread_cond_init(&client->lock_condition, NULL);

    client->lock=0;
    client->status_app=NOTIFYFS_CLIENTSTATUS_NOTSET;
    client->status_fs=NOTIFYFS_CLIENTSTATUS_NOTSET;
    client->messagemask=0;

    /* insert at begin of list */

    if ( clients_list ) clients_list->prev=client;
    client->next=clients_list;
    client->prev=NULL;
    clients_list=client;

    unlock:

    res=pthread_mutex_unlock(&clients_list_mutex);

    return client;

}

/* function todo:

    - lookup_watch (inode, client) searches for a watch on inode for client
    - add_watch (inode, client) adds a new watch to the inode and for client, if there is already one update that one
    - lookup_client (pid, uid, gid) to find the right client when receiving an add watch request
    - remove watch (watch ) removes watch, also from different lists
    - 
*/

struct client_struct *lookup_client(pid_t pid, unsigned char lockset)
{
    struct client_struct *client=NULL;
    int res;

    if (lockset==0) res=pthread_mutex_lock(&clients_list_mutex);

    /* check existing clients */

    client=clients_list;
    while (client) {

        if ( client->pid==pid ) break;
        client=client->next;

    }

    if (lockset==0) res=pthread_mutex_unlock(&clients_list_mutex);

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

struct client_struct *get_clientslist()
{
    return clients_list;
}


int lock_client(struct client_struct *client)
{
    int res;

    res=pthread_mutex_lock(&client->lock_mutex);

    if ( client->lock==1 ) {

        while (client->lock==1) {

    	    res=pthread_cond_wait(&client->lock_condition, &client->lock_mutex);

        }

    }

    client->lock=1;

    res=pthread_mutex_unlock(&client->lock_mutex);

    return res;

}

int unlock_client(struct client_struct *client)
{
    int res;

    res=pthread_mutex_lock(&client->lock_mutex);
    client->lock=0;
    res=pthread_cond_broadcast(&client->lock_condition);
    res=pthread_mutex_unlock(&client->lock_mutex);

    return res;

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

/*  function which called to match a client fs and a mount entry

    this is called when a client registers itself as client fs and
    when a mount entry is added

    it is called at both events because a client fs can send a 
    registerfs message AFTER it's mounted

*/

void assign_mountpoint_clientfs(struct client_struct *client, struct mount_entry_struct *mount_entry)
{
    int res;

    // logoutput("assign_mountpoint_clientfs: match clientfs and mount entry");

    if ( client ) {

	if ( ! (client->type&NOTIFYFS_CLIENTTYPE_FS) ) {

	    logoutput("assign_mountpoint_clientfs: error, client set, but not a fs");
	    return;

	}

	/* the thing here is to lookup the mount_entry matching the path the client fs is mounted at */

	if ( mount_entry ) {

	    logoutput("assign_mountpoint_clientfs: client and mount entry both defined: error");

	} else {

	    // logoutput("assign_mountpoint_clientfs: lookup mount entry matching %s", client->path);

	    res=lock_mountlist(MOUNTENTRY_CURRENT);

	    mount_entry=get_next_mount_entry(NULL, MOUNTENTRY_CURRENT);

	    while (mount_entry) {

		if ( strcmp(mount_entry->mountpoint, client->path)==0 ) break;

    		mount_entry=get_next_mount_entry(mount_entry, MOUNTENTRY_CURRENT);

	    }

	    res=unlock_mountlist(MOUNTENTRY_CURRENT);

	    if ( ! mount_entry ) {

		logoutput("assign_mountpoint_clientfs: mount entry not found");

	    } else {

		res=lock_client(client);

		mount_entry->client=(void *) client;
		client->mount_entry=mount_entry;

		logoutput("assign_mountpoint_clientfs: mount entry found, client is up and complete");

		client->status_fs=NOTIFYFS_CLIENTSTATUS_UP;

		res=unlock_client(client);

	    }

	}

    } else {

	if ( ! mount_entry ) {

	    logoutput("assign_mountpoint_clientfs: client and mount entry both not defined: error");

	} else {

	    // logoutput("assign_mountpoint_clientfs: lookup client fs matching %s", mount_entry->mountpoint);

	    res=lock_clientslist();

	    client=get_clientslist();

	    while (client) {

		if ( ! (client->type&NOTIFYFS_CLIENTTYPE_FS) ) {

		    client=client->next;
		    continue;

		}

		// if ( client->status==NOTIFYFS_CLIENTSTATUS_NOTSET ) {

		    /* only look at clients not up */

		    if ( strcmp(client->path, mount_entry->mountpoint)==0 ) break;

		//}

		client=client->next;

	    }

	    res=unlock_clientslist();

	    if ( client ) {

		res=lock_client(client);

		logoutput("assign_mountpoint_clientfs: client found, client is up and complete");

		mount_entry->client=(void *) client;
		client->mount_entry=mount_entry;
		client->status_fs=NOTIFYFS_CLIENTSTATUS_UP;

		res=unlock_client(client);

	    } else {

		logoutput("assign_mountpoint_clientfs: client not found");

	    }

	}

    }

}


