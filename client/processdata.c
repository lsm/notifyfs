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
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <inttypes.h>

// required??

#include <ctype.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>


#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

#include "logging.h"
#include "epoll-utils.h"
#include "message.h"
#include "client-io.h"

extern unsigned char loglevel;
extern int logarea;

char recvbuffer[NOTIFYFS_CLIENT_RECVBUFFERSIZE];

static void handle_listreply(char buffer[NOTIFYFS_CLIENT_RECVBUFFERSIZE], int size, int count)
{
    struct client_messageentry_struct *messageentry;
    int read=0, sizemessage=sizeof(struct client_messageentry_struct);
    int i=0;

    while(read<size) {

	messageentry=(struct client_messageentry_struct *) &buffer[read];

	logoutput("handle_listreply: read messageentry %i at %i, lenname %i", i, read, messageentry->len);

	if (messageentry) {

	    logoutput("entry: %s", messageentry->name);

	    read+=sizemessage+messageentry->len+1;
	    i++;

	} else {

	    logoutput("messageentry not read");

	    break;

	}

    }

}

/* function to receive a message, reacting on data on fd via callbacks*/

void receive_message(int fd, void *data)
{
    struct msghdr msg;
    int nreturn=0;
    struct iovec iov[2];
    ssize_t lenread;
    struct notifyfs_message_body message_body;

    logoutput("receive_message");

    readbuffer:

    /* prepare msg */

    iov[0].iov_base=(void *) &message_body;
    iov[0].iov_len=sizeof(struct notifyfs_message_body);

    iov[1].iov_base=(void *) recvbuffer;
    iov[1].iov_len=NOTIFYFS_CLIENT_RECVBUFFERSIZE;

    msg.msg_iov=iov;
    msg.msg_iovlen=2;

    msg.msg_control=NULL;
    msg.msg_controllen=0;

    msg.msg_name=NULL;
    msg.msg_namelen=0;

    lenread=recvmsg(fd, &msg, MSG_WAITALL);

    logoutput("receive_message: msg_controllen %i msg_iovlen %i lenread %i", msg.msg_controllen, msg.msg_iovlen, (int)lenread);

    if ( lenread<0 ){

	if (lenread<0) logoutput("receive_message: error %i recvmsg", errno);

    } else if ( msg.msg_controllen==0 ) {

	/* a normal message */

	/* first part is message body */

	if ( message_body.type==NOTIFYFS_MESSAGE_TYPE_LISTREPLY ) {
	    struct notifyfs_listreply_message *listreply_message;

	    logoutput("received a listreply message");

	    listreply_message=(struct notifyfs_listreply_message *) &(message_body.body.listreply_message);

	    handle_listreply(recvbuffer, listreply_message->sizereply, listreply_message->count);

	}

	goto readbuffer;

    }

}
