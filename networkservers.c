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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>

#include <pthread.h>


#define LOG_LOGAREA LOG_LOGAREA_SOCKET


#include "logging.h"
#include "epoll-utils.h"
#include "socket.h"
#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"
#include "entry-management.h"
#include "networkservers.h"

static pthread_mutex_t servers_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct notifyfs_server_struct *servers=NULL;

/* for connections which are local */
static struct notifyfs_server_struct local_server;

struct notifyfs_server_struct *get_local_server()
{
    return &local_server;
}

void add_server_to_list_unlocked(struct notifyfs_server_struct *server)
{
    /* add to list */

    if (servers) servers->prev=server;
    server->next=servers;
    servers=server;

}

void add_server_to_list(struct notifyfs_server_struct *server, unsigned char locked)
{

    if (locked==0) pthread_mutex_lock(&servers_mutex);

    add_server_to_list_unlocked(server);

    if (locked==0) pthread_mutex_unlock(&servers_mutex);

}

struct notifyfs_server_struct *create_notifyfs_server()
{
    struct notifyfs_server_struct *notifyfs_server=malloc(sizeof(struct notifyfs_server_struct));

    if (notifyfs_server) {

	notifyfs_server->owner_id=new_owner_id();

	notifyfs_server->type=NOTIFYFS_SERVERTYPE_NOTSET;
	notifyfs_server->buffer=NULL;
	notifyfs_server->lenbuffer=0;
	notifyfs_server->connection=NULL;
	notifyfs_server->status=NOTIFYFS_SERVERSTATUS_NOTSET;
	notifyfs_server->error=0;
	notifyfs_server->connect_time.tv_sec=0;
	notifyfs_server->connect_time.tv_nsec=0;

	pthread_mutex_init(&notifyfs_server->mutex, NULL);

	notifyfs_server->data=NULL;
	notifyfs_server->clientwatches=NULL;

	notifyfs_server->next=NULL;
	notifyfs_server->prev=NULL;

    }

    return notifyfs_server;

}

void init_local_server()
{

    local_server.owner_id=new_owner_id();

    local_server.type=NOTIFYFS_SERVERTYPE_LOCALHOST;
    local_server.status=NOTIFYFS_SERVERSTATUS_UP;

    local_server.buffer=NULL;
    local_server.lenbuffer=0;
    local_server.connection=NULL;

    local_server.error=0;
    local_server.connect_time.tv_sec=0;
    local_server.connect_time.tv_nsec=0;

    pthread_mutex_init(&local_server.mutex, NULL);

    local_server.data=NULL;
    local_server.clientwatches=NULL;

    local_server.next=NULL;
    local_server.prev=NULL;

    add_server_to_list(&local_server, 0);

}

void change_status_server(struct notifyfs_server_struct *server, unsigned char status)
{

    pthread_mutex_lock(&server->mutex);
    server->status=status;
    pthread_mutex_unlock(&server->mutex);

}


