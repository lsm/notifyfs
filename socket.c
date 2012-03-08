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

/* sending mount data via socket to clients */

int send_mount_message(int fd, struct mount_entry_struct *mount_entry)
{
    struct msghdr msg;
    struct iovec io_vector[14];
    char *controlbuffer;
    struct cmsghdr *cmptr;
    int nreturn=0;

    /* create the control buffer, a control message plus unsigned char */

    msg.msg_controllen=CMSG_SPACE(sizeof(unsigned char);

    controlbuffer=malloc(msg.msg_controllen);

    if ( ! controlbuffer ) {

	nreturn=-ENOMEM;
	goto out;

    }

    msg.msg_control=controlbuffer;

    /* first header is used to indicate what kind of message this is (stored in unsigned char) */

    cmptr=CMSG_FIRSTHDR(&msg);
    cmptr->cmsg_len=CMSG_LEN(sizeof(unsigned char));
    cmptr->cmsg_type=0;
    *((unsigned char *) CMSG_DATA(cmptr))=NOTIFYFS_MESSAGE_TYPE_MOUNTENTRY;

    msg.msg_name=NULL;
    msg.msg_namelen=0;

    msg.msg_iov=io_vector;
    msg.msg_iovlen=14;

    /* mountpoint */

    if ( mount_entry->mountpoint ) {

	io_vector[0].iov_base=(void *) mount_entry->mountpoint;
	io_vextor[0].iov_len=strlen(mount_entry->mountpoint);

    } else {

	io_vector[0].iov_base=NULL;
	io_vextor[0].iov_len=0;

    }


    /* fstype */

    io_vector[1].iov_base=(void *) mount_entry->fstype;
    io_vextor[1].iov_len=strlen(mount_entry->fstype);


    /* mountsource */

    io_vector[2].iov_base=(void *) mount_entry->mountsource;
    io_vextor[2].iov_len=strlen(mount_entry->mountsource);


    /* superoptions */

    io_vector[3].iov_base=(void *) mount_entry->superoptions;
    io_vextor[3].iov_len=strlen(mount_entry->superoptions);

    /* rootpath */

    if ( mount_entry->rootpath ) {

	io_vector[4].iov_base=mount_entry->rootpath;
	io_vextor[4].iov_len=strlen(mount_entry->rootpath);

    } else {

	io_vector[4].iov_base=NULL;
	io_vextor[4].iov_len=0;

    }

    /* minor */

    io_vector[5].iov_base=(void *) &(mount_entry->minor);
    io_vextor[5].iov_len=sizeof(int);

    /* major */

    io_vector[6].iov_base=(void *) &(mount_entry->major);
    io_vextor[6].iov_len=sizeof(int);

    /* isbind */

    io_vector[7].iov_base=(void *) &(mount_entry->isbind);
    io_vextor[7].iov_len=sizeof(unsigned char);

    /* isroot */

    io_vector[8].iov_base=(void *) &(mount_entry->isroot);
    io_vextor[8].iov_len=sizeof(unsigned char);

    /* isautofs */

    io_vector[9].iov_base=(void *) &(mount_entry->isautofs);
    io_vextor[9].iov_len=sizeof(unsigned char);

    /* autofs_indirect */

    io_vector[10].iov_base=(void *) &(mount_entry->autofs_indirect);
    io_vextor[10].iov_len=sizeof(unsigned char);

    /* autofs_mounted */

    io_vector[11].iov_base=(void *) &(mount_entry->autofs_mounted);
    io_vextor[11].iov_len=sizeof(unsigned char);

    /* status */

    io_vector[12].iov_base=(void *) &(mount_entry->status);
    io_vextor[12].iov_len=sizeof(unsigned char);

    /* remount */

    io_vector[13].iov_base=(void *) &(mount_entry->remount);
    io_vextor[13].iov_len=sizeof(unsigned char);


    /* the actual sending */

    nreturn=sendmsg(fd, &msg, 0);

    free(controlbuffer);

    out:

    return nreturn;

}


send_fd_message(int fd, int fdtosend)
{
    struct msghdr msg;
    char *controlbuffer;
    struct cmsghdr *cmptr;
    int nreturn=0;

    /* create the control buffer, a control message plus unsigned char */

    msg.msg_controllen=CMSG_SPACE(sizeof(unsigned char) + CMSG_SPACE(sizeof(int);

    controlbuffer=malloc(msg.msg_controllen);

    if ( ! controlbuffer ) {

	nreturn=-ENOMEM;
	goto out;

    }

    msg.msg_control=controlbuffer;

    msg.msg_control=control_union.controldata;
    msg.msg_controllen=sizeof(control_un.controldata);

    /* first header is used to indicate what kind of message this is (stored in unsigned char) */

    cmptr=CMSG_FIRSTHDR(&msg);
    cmptr->cmsg_len=CMSG_LEN(sizeof(unsigned char));
    cmptr->cmsg_level=SOL_SOCKET;
    cmptr->cmsg_type=0;
    *((unsigned char *) CMSG_DATA(cmptr))=NOTIFYFS_MESSAGE_TYPE_FD;

    /* second header is used for the fd */

    cmptr=CMSG_NXTHDR(&msg, cmptr);
    cmptr->cmsg_len=CMSG_LEN(sizeof(int));
    cmptr->cmsg_level=SOL_SOCKET;
    cmptr->cmsg_type=SCM_RIGHTS;
    *((int *) CMSG_DATA(cmptr))=fdtosend;


    msg.msg_name=NULL;
    msg.msg_namelen=0;

    /* no futher data */

    msg.msg_iov=NULL;
    msg.msg_iovlen=0;

    /* the actual sending */

    nreturn=sendmsg(fd, &msg, 0);

    free(controlbuffer);

    out:

    return nreturn;

}

int receive_message(int fd, void **prt)
{
    struct msghdr msg;
    struct cmsghdr *cmptr;
    int nreturn=0;
    ssize_t lenread;
    unsigned char *typemessage;

    lenread=recvmesg(fd, &msg, 0);

    if ( lenread<0 ) {

	nreturn=lenread;
	goto out;

    }

    cmptr=CMSG_FIRSTHDR(msg);

    if ( ! cmptr ) {

	nreturn=-EIO;
	goto out;

    }

    typemessage=(unsigned char *) CMSG_DATA(cmptr);

    if ( *typemessage==NOTIFYFS_MESSAGE_TYPE_FD ) {
	int newfd;

	/* dealing with a message to parse a fd.... */
	/* here read the fd from the second header */

	cmptr=CMSG_NXTHDR(&msg, cmptr);

	newfd=*((int *) CMSG_DATA(cmptr));

	/* what to do here ?? */

    } else if ( *typemessage==NOTIFYFS_MESSAGE_TYPE_MOUNTENTRY ) {
	struct mount_entry_struct *mount_entry;


	/* dealing with a message to parse a mount entry */
	/* here read the io vector */

	mount_entry=get_mount_entry();

	if ( ! mount_entry ) {

	    logoutput(

    }

    

