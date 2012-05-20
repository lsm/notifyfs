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

#define LOG_LOGAREA LOG_LOGAREA_MESSAGE

#include "logging.h"
#include "epoll-utils.h"

#include "mountinfo.h"
#include "client.h"

#include "message.h"
#include "message-server.h"

static struct notifyfs_message_callbacks message_cb={NULL, NULL, NULL, NULL};

void assign_message_callback_server(unsigned char type, void *callback)
{

    assign_message_callback(type, callback, &message_cb);

}

/* sending of an fd
   this requires a special function, since the fd is stored in a second ctrl message, where
   for the other messages there is one ctrl message */

int send_fd_message(int fd, int fdtosend)
{
    struct msghdr msg;
    char *controlbuffer;
    struct cmsghdr *cmptr;
    struct iovec io_vector[3];
    int nreturn=0;
    int *fdptr;
    unsigned char *typeptr;
    unsigned char typemessage=NOTIFYFS_MESSAGE_TYPE_FD;

    /* create the control buffer, a control message plus unsigned char */

    msg.msg_controllen=CMSG_SPACE(sizeof(unsigned char)) + CMSG_SPACE(sizeof(int));

    controlbuffer=malloc(msg.msg_controllen);

    if ( ! controlbuffer ) {

	nreturn=-ENOMEM;
	goto out;

    }

    msg.msg_control=controlbuffer;

    /* first header is used to indicate what kind of message this is (stored in unsigned char) */

    cmptr=CMSG_FIRSTHDR(&msg);
    cmptr->cmsg_len=CMSG_LEN(sizeof(unsigned char));
    cmptr->cmsg_level=SOL_SOCKET;
    cmptr->cmsg_type=0;

    typeptr=CMSG_DATA(cmptr);

    memcpy(typeptr, &typemessage, sizeof(unsigned char));

    /* second header is used for the fd */

    cmptr=CMSG_NXTHDR(&msg, cmptr);
    cmptr->cmsg_len=CMSG_LEN(sizeof(int));
    cmptr->cmsg_level=SOL_SOCKET;
    cmptr->cmsg_type=SCM_RIGHTS;

    fdptr=(int *) CMSG_DATA(cmptr);

    memcpy(fdptr, &fdtosend, sizeof(int));


    msg.msg_name=NULL;
    msg.msg_namelen=0;

    io_vector[0].iov_base=NULL;
    io_vector[0].iov_len=0;

    io_vector[1].iov_base=NULL;
    io_vector[1].iov_len=0;

    io_vector[2].iov_base=NULL;
    io_vector[2].iov_len=0;

    msg.msg_iov=io_vector;
    msg.msg_iovlen=3;

    /* the actual sending */

    nreturn=sendmsg(fd, &msg, 0);

    if ( nreturn==-1 ) nreturn=-errno;

    free(controlbuffer);

    out:

    return nreturn;

}

/* sending mount data via socket to clients */

int send_mount_message(int fd, struct mount_entry_struct *mount_entry, uint64_t unique)
{
    struct notifyfs_message_body message;
    struct notifyfs_mount_message *mount_message;
    int nreturn=0;
    int lenmountpoint=0;
    int lenrootpath=0;

    message.type=NOTIFYFS_MESSAGE_TYPE_MOUNTINFO;

    mount_message=&(message.body.mountinfo);

    mount_message->unique=unique;

    strcpy(mount_message->fstype, mount_entry->fstype);
    strcpy(mount_message->mountsource, mount_entry->mountsource);
    strcpy(mount_message->superoptions, mount_entry->superoptions);

    mount_message->minor=mount_entry->minor;
    mount_message->major=mount_entry->major;

    mount_message->isbind=mount_entry->isbind;
    mount_message->isroot=mount_entry->isroot;
    mount_message->isautofs=mount_entry->isautofs;
    mount_message->autofs_indirect=mount_entry->autofs_indirect;
    mount_message->autofs_mounted=mount_entry->autofs_mounted;
    mount_message->status=mount_entry->status;

    if ( mount_entry->mountpoint ) lenmountpoint=strlen(mount_entry->mountpoint)+1;
    if ( mount_entry->rootpath ) lenrootpath=strlen(mount_entry->rootpath)+1;

    nreturn=send_message(fd, &message, mount_entry->mountpoint, lenmountpoint, mount_entry->rootpath, lenrootpath);

    return nreturn;

}

int receive_message_from_client(int fd, struct client_struct *client)
{
    return receive_message(fd, client, &message_cb);

}
