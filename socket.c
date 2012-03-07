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
#include "fuse-loop-epoll-mt.h"

#include "client.h"

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


void *handle_data_on_connection_fd(struct epoll_event *epoll_event)
{
    struct epoll_extended_data_struct *epoll_xdata;
    int nreturn=0;

    /* get the epoll_xdata from event */

    epoll_xdata=(struct epoll_extended_data_struct *) epoll_event->data.ptr;

    if ( epoll_event->events & ( EPOLLHUP | EPOLLRDHUP ) ) {

        // hangup of remote site
        // a hickup: what to do with the data still available on the fd

        nreturn=remove_xdata_from_epoll(epoll_xdata, 0);

        if ( nreturn<0 ) {

            logoutput1("error(%i) removing xdata from epoll\n", nreturn);

        }


    } else if ( epoll_event->events & EPOLLIN ) {
        struct client_message_struct *message;
        struct client_struct *client;
        int nlenread=0, i=0;
        size_t size=sizeof(struct client_message_struct);
        char buff[size];

        // data available for read

        // int nbytes, res, nerror=0;
        // struct message_struct message;
        // unsigned char tries=0;

        client=(struct client_struct *) epoll_xdata->data;

        /* here handle the data */

        logoutput1("handling data on connection fd %i", epoll_xdata->fd);

        nlenread=read(epoll_xdata->fd, buff, size);

        if ( nlenread<0 ) {

            logoutput1("reading connection fd %i gives error %i", epoll_xdata->fd, errno);

        } else {

	    logoutput1("handling data on connection fd %i %i bytes read", epoll_xdata->fd, nlenread);

            while (i<nlenread) {

                message=(struct client_message_struct *) buff;

                if ( message ) {

                    /* do some checks :
                       a client fs sends only register, signoff or notify events...
                       and a client app only register or signoff */

                    if ( message->type==NOTIFYFS_MESSAGE_TYPE_REGISTERAPP ) {

                        if ( client->type!=NOTIFYFS_CLIENTTYPE_UNKNOWN ) {

                            /* client type already set.... not necessary */

                            logoutput1("getting registerapp message on fd %i from pid %i, while already set", epoll_xdata->fd, client->pid);

                        } else {

                            client->type=NOTIFYFS_CLIENTTYPE_APP;

                            logoutput1("getting registerapp message on fd %i from pid %i", epoll_xdata->fd, client->pid);

                        }

                    } else if ( message->type==NOTIFYFS_MESSAGE_TYPE_REGISTERFS ) {

                        if ( client->type!=NOTIFYFS_CLIENTTYPE_UNKNOWN ) {

                            /* client type already set.... not necessary */

                            logoutput1("getting registerfs message on fd %i from pid %i, while already set", epoll_xdata->fd, client->pid);

                        } else {

                            client->type=NOTIFYFS_CLIENTTYPE_FS;

			    logoutput1("getting registerfs message on fd %i from pid %i", epoll_xdata->fd, client->pid);

                        }

			if ( message->len>0 ) {
			    char path[message->len];

			    memset(path, '\0', message->len);

			    /* here also read additional data */

			    nlenread=read(epoll_xdata->fd, path, message->len);

			    logoutput1("additional data read from pid %i: %s", client->pid, path);

			    /* what to do with this data?? 
			       it's a mountpoint, well it's supposed to be
			       connect it to a entry/mountpoint
			       it must be there
			    */

			    assign_mountpoint_clientfs(client, path);

			}


                    } else if ( message->type==NOTIFYFS_MESSAGE_TYPE_SIGNOFF ) {

                        /* here remove the client and everything related to it:
                           - every watch set
                           - eventually the backend watches
                        */

			logoutput1("getting signoff message on fd %i from pid %i", epoll_xdata->fd, client->pid);

                    } else if ( message->type==NOTIFYFS_MESSAGE_TYPE_FSEVENT ) {

                        /* something happened on the fs
                           also take into account the eventual extra data
                           i+=message->len...
                        */

                        /* lookup watch using the unique id and client
                        */

                        logoutput1("getting fsevent message on fd %i from pid %i", epoll_xdata->fd, client->pid);

                    } else {

                        logoutput1("getting unknown message on fd %i from pid %i", epoll_xdata->fd, client->pid);

                    }

                }

		i+=nlenread;

            }

        }

    }

}

/*
* function to handle when data arrives on the socket
* this means that a client tries to connect
*/

void *handle_data_on_socket_fd(struct epoll_event *epoll_event)
{
    struct epoll_extended_data_struct *epoll_xdata;
    struct client_struct *client;
    int connection_fd;
    struct ucred cr;
    socklen_t cl=sizeof(cr), res;

    /* get the epoll_xdata from event */

    epoll_xdata=(struct epoll_extended_data_struct *) epoll_event->data.ptr;

    // add a new fd to communicate

    connection_fd = accept4(epoll_xdata->fd, (struct sockaddr *) &address, &address_length, 0);

    if ( connection_fd==-1 ) {

        goto out;

    }

    /* get the credentials */

    res=getsockopt(connection_fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl);

    if ( res!=0 ) {

        goto out;

    }

    /* check for client */

    client=register_client(connection_fd, cr.pid, cr.uid, cr.gid);
    if ( ! client ) goto out;

    /* add to epoll */

    res=add_to_epoll(connection_fd, EPOLLIN | EPOLLET | EPOLLOUT, TYPE_FD_CLIENT, &handle_data_on_connection_fd, client);
    if ( res<0 ) goto out;

    out:

    return;

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

/* function to send a notify message to client app or client fs:
   examples:
   - send a client app event data on watch
   - send a client fs a set watch request
   - send a client fs a del watch request
   */

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
