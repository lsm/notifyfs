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

#include "client.h"

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
    client->idctr=0;

    client->next=NULL;
    client->prev=NULL;

    pthread_mutex_init(&client->lock_mutex, NULL);
    pthread_cond_init(&client->lock_condition, NULL);

    client->lock=0;
    client->status=0;

    client->mount_entry=NULL;

    /* insert at begin of list */

    if ( clients_list ) clients_list->prev=client;
    client->next=clients_list;
    client->prev=NULL;

    /* no watches yet */

    client->watches=NULL;

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

