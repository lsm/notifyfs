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

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

#include "logging.h"
#include "notifyfs.h"
#include "epoll-utils.h"
#include "watches.h"
#include "socket.h"

#define LISTEN_BACKLOG 50

struct sockaddr_un address;
socklen_t address_length;
int socket_fd;
struct epoll_extended_data_struct xdata_socket=EPOLL_XDATA_INIT;

struct socket_callbacks_struct {
    int (*socketfd_cb) (int fd, pid_t pid, uid_t uid, gid_t gid);
};

struct socket_callbacks_struct socket_callbacks={NULL};

void assign_socket_callback(int (*socketfd_cb) (int fd, pid_t pid, uid_t uid, gid_t gid))
{
    socket_callbacks.socketfd_cb=socketfd_cb;

}


/*
* function to handle when data arrives on the socket
* this means that a client tries to connect
*/

void handle_data_on_socket_fd(int fd, void *data, uint32_t events)
{
    struct epoll_extended_data_struct *client_xdata;
    struct client_struct *client;
    int connection_fd;
    struct ucred cr;
    socklen_t cl=sizeof(cr), res;
    int n;

    logoutput("handle_data_on_socket_fd");

    // add a new fd to communicate

    connection_fd = accept4(fd, (struct sockaddr *) &address, &address_length, SOCK_NONBLOCK);

    if ( connection_fd==-1 ) {

        return;

    }

    getsockopt(connection_fd, SOL_SOCKET, SO_RCVBUF, (void *) &n, sizeof(int));

    logoutput("handle_data_on_socket_fd: size of buffer: %i", n);

    /* get the credentials */

    res=getsockopt(connection_fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl);

    if ( res!=0 ) {

        return;

    }

    logoutput("handle_data_on_socket_fd: pid %i", cr.pid);

    if (socket_callbacks.socketfd_cb) {

	res=(* socket_callbacks.socketfd_cb) (connection_fd, cr.pid, cr.uid, cr.gid);

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
	struct epoll_extended_data_struct *epoll_xdata;

	epoll_xdata=add_to_epoll(socket_fd, EPOLLIN, TYPE_FD_SOCKET, &handle_data_on_socket_fd, NULL, &xdata_socket, NULL);

	if ( ! epoll_xdata ) {

	    nreturn=-errno;
    	    logoutput("create_socket: error adding socket fd to mainloop.");

	} else {

    	    logoutput("create_socket: socket fd %i added to epoll", socket_fd);
	    add_xdata_to_list(epoll_xdata, NULL);
	    nreturn=socket_fd;

	}

    }

    out:

    return nreturn;

}

