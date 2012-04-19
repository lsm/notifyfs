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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>

#include <fuse/fuse_lowlevel.h>

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

#include "logging.h"
#include "notifyfs.h"
#include "entry-management.h"
#include "epoll-utils.h"
#include "watches.h"

#include "client.h"
#include "mountinfo.h"
#include "message-server.h"

#include "socket.h"



#define LISTEN_BACKLOG 50

struct sockaddr_un address;
socklen_t address_length;
int socket_fd;

/* function which is called when data on a connection fd 
   examples of incoming data:
   - setting type of client, like app or fs
   - signoff
   - event from client fs 
   */

void handle_data_on_connection_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo)
{
    int nreturn=0;

    if ( events & ( EPOLLHUP | EPOLLRDHUP ) ) {
	struct client_struct *client=(struct client_struct *) epoll_xdata->data;

        // hangup of remote site
        // a hickup: what to do with the data still available on the fd

        /* lookup the client and remove it */

        nreturn=remove_xdata_from_epoll(epoll_xdata, 0);

        if ( nreturn<0 ) {

            logoutput("error(%i) removing xdata from epoll\n", nreturn);

        }

	close(epoll_xdata->fd);
	epoll_xdata->fd=0;

	remove_xdata_from_list(epoll_xdata);

	free(epoll_xdata);

	if ( client ) {

	    /* set client to down */

	    client->status_fs=NOTIFYFS_CLIENTSTATUS_DOWN;
	    client->status_app=NOTIFYFS_CLIENTSTATUS_DOWN;
	    client->fd=0;

	}


    } else {
        int nlenread=0;
        struct client_struct *client=(struct client_struct *) epoll_xdata->data;

        /* here handle the data available for read */

        logoutput("handling data on connection fd %i", epoll_xdata->fd);

	nlenread=receive_message_from_client(epoll_xdata->fd, client);

	if ( nlenread<0 ) nreturn=nlenread;

    }

}

/*
* function to handle when data arrives on the socket
* this means that a client tries to connect
*/

void handle_data_on_socket_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo)
{
    struct epoll_extended_data_struct *client_xdata;
    struct client_struct *client;
    int connection_fd;
    struct ucred cr;
    socklen_t cl=sizeof(cr), res;

    logoutput("handle_data_on_socket_fd");

    // add a new fd to communicate

    connection_fd = accept4(epoll_xdata->fd, (struct sockaddr *) &address, &address_length, SOCK_NONBLOCK);

    if ( connection_fd==-1 ) {

        return;

    }

    /* get the credentials */

    res=getsockopt(connection_fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl);

    if ( res!=0 ) {

        return;

    }

    /* check for client */

    client=register_client(connection_fd, cr.pid, cr.uid, cr.gid);
    if ( ! client ) return;

    /* add to epoll */

    client_xdata=add_to_epoll(connection_fd, EPOLLIN | EPOLLET, TYPE_FD_CLIENT, &handle_data_on_connection_fd, (void *) client, NULL);

    if ( ! client_xdata ) {

	logoutput("error adding client fd %i to mainloop", connection_fd);

    } else {

	add_xdata_to_list(client_xdata);

    }


}

int create_socket(char *path)
{
    int nreturn=0;

    /* add socket */

    socket_fd = socket(PF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);

    if ( socket_fd < 0 ) {

        nreturn=-errno;
        goto out;

    }

    /* bind path/familiy and socket */

    memset(&address, 0, sizeof(struct sockaddr_un));

    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, UNIX_PATH_MAX, path);

    address_length=sizeof((struct sockaddr *) &address);

    if ( bind(socket_fd, (struct sockaddr *) &address, sizeof(struct sockaddr_un)) != 0) {

        nreturn=-errno;
        goto out;

    }

    /* listen */

    if ( listen(socket_fd, LISTEN_BACKLOG) != 0 ) {

        nreturn=-errno;
        goto out;

    } else {

        nreturn=socket_fd;

    }

    out:

    return nreturn;

}

