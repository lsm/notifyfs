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

#include <fcntl.h>
#include <dirent.h>

#include <fuse/fuse_lowlevel.h>

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

#include "logging.h"
#include "testfs.h"
#include "entry-management.h"
#include "fuse-loop-epoll-mt.h"

#include "socket.h"



#define LISTEN_BACKLOG 50

struct sockaddr_un address;
socklen_t address_length;
int socket_fd;

/*
* function to handle when data arrives on the socket
* this means that a client tries to connect
*/

void *handle_data_on_socket_fd(struct epoll_event *epoll_event)
{
    struct epoll_extended_data_struct *epoll_xdata;
    int res;

    /* get the epoll_xdata from event */

    epoll_xdata=(struct epoll_extended_data_struct *) epoll_event->data.ptr;

    /* here handle here received on the socket fd:
       examples:
       - set watch
       - remove watch
    */

    if ( epoll_event->events & ( EPOLLHUP | EPOLLRDHUP ) ) {

        // hangup of remote site: notifyfs stopped...

        res=remove_xdata_from_epoll(epoll_xdata, 0);

        if ( res<0 ) {

            logoutput1("error(%i) removing xdata from epoll\n", res);

        }


    } else if ( epoll_event->events & EPOLLIN ) {
        struct client_message_struct *message;
        int nlenread=0, i=0;
        size_t size=32 * (sizeof(struct client_message_struct));
        char buff[size];

        // data available for read

        // int nbytes, res, nerror=0;
        // struct message_struct message;
        // unsigned char tries=0;

        /* here handle the data */

        logoutput1("handling data on socket fd %i", epoll_xdata->fd);

        nlenread=read(epoll_xdata->fd, buff, size);

        if ( nlenread<0 ) {

            logoutput1("reading socket fd %i gives error %i", epoll_xdata->fd, errno);

        } else {

            while (i<nlenread) {

                message=(struct message_struct *) &buff[i];

                if ( message ) {

                    if ( message->type==NOTIFYFS_MESSAGE_TYPE_SETWATCH ) {
                	char path[message->len];

			/* here set a watch depending the attributes of the message:
			   - id
			   - mask
			   - len>0

			   additional a name which is the path*/

			/* here reply with an unique id?? 
			   and a message id ??
			   res=read_path_from_socket(fd, name)
			   res=create_watch(mask, 
			*/

			res=read(epoll_xdata->fd, path, message->len);

			logoutput0("received a message to set a watch (mask: %i) on %s", path);

                    } else if ( message->type==NOTIFYFS_MESSAGE_TYPE_DELWATCH ) {

			/* here delete a watch depending the attributes of the message:
			   - id of the watch
			res=remove_watch(message->id);
			*/

			logoutput0("received a message to remove a watch (id: %i)", message->id);

                    } else {

                        logoutput0("getting unknown message type %i on fd %i", message->type, epoll_xdata->fd);

                    }

                }

		i+=nlenread;

            }

        }

    }


    out:

    return;

}



/* function to send a notify message to client app */

int send_notify_message(int fd, unsigned char type, unsigned long id, int mask, int len, char *name)
{
    struct client_message_struct message;
    int res;

    message.type=type;
    message.id=id;
    message.mask=mask;
    message.len=len;

    if ( len>0 && name ) {
	int messagesize=sizeof(struct client_message_struct)+len;
	char buff[messagesize];

	/* when dealing with an extra field create one buff */

	memcpy(buff, &message, sizeof(struct client_message_struct));
	memcpy(buff+sizeof(struct client_message_struct), name, len);

	res=write(fd, buff, messagesize);

    } else {

	res=write(fd, &message, sizeof(struct client_message_struct));

    }

    /* here handling of error .... */

    return res;

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
