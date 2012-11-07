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
#include "epoll-utils.h"
#include "socket.h"

struct sockaddr_un address;
socklen_t address_length;
int socket_fd;

struct socket_callbacks_struct {
    int (*connection_cb) (int fd, void *data);
};

struct socket_callbacks_struct socket_callbacks={NULL};

void assign_connection_callback(int (*connection_cb) (int fd, void *data))
{
    socket_callbacks.connection_cb=connection_cb;

}

/* function which is called when data on a connection fd 
   examples of incoming data:
   - setting type of client, like app or fs
   - signoff
   - event from client fs 
   */

int handle_data_on_connection_fd(int fd, void *data, uint32_t events)
{
    int nreturn=0;

    if ( events & ( EPOLLHUP | EPOLLRDHUP ) ) {
	struct epoll_extended_data_struct *epoll_xdata;

	close(fd);

	epoll_xdata=scan_eventloop(fd, 0, NULL);

	if (epoll_xdata) {

	    remove_xdata_from_epoll(epoll_xdata, NULL);
	    remove_xdata_from_list(epoll_xdata, 0, NULL);

	    free(epoll_xdata);

	}

    } else {
        int nlenread=0;

        /* here handle the data available for read */

        logoutput("handling data on connection fd %i", fd);

	if (socket_callbacks.connection_cb) {

	    (* socket_callbacks.connection_cb) (fd, data);

	}

    }

    return nreturn;

}

int connect_socket(char *path)
{
    int  nreturn=0;

    socket_fd = socket(PF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);

    if (socket_fd < 0) {

        nreturn=-errno;
        goto out;

    }

    /* start with a clean address structure */
    memset(&address, 0, sizeof(struct sockaddr_un));

    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, UNIX_PATH_MAX, path);

    if ( connect(socket_fd, (struct sockaddr *) &address, sizeof(struct sockaddr_un)) != 0) {

        nreturn=-errno;
        goto out;

    }

    out:

    if ( nreturn==0 ) nreturn=socket_fd;

    return nreturn;

}
