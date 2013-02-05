/*
 
  2010, 2011, 2012 Stef Bon <stefbon@gmail.com>

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

#define LOG_LOGAREA LOG_LOGAREA_MESSAGE

#include "logging.h"
#include "notifyfs-io.h"
#include "message.h"
#include "client-io.h"
#include "client.h"

#include "message-server.h"

void send_reply_message(int fd, uint64_t unique, int error, void *buffer, size_t size)
{
    struct notifyfs_message_body message;

    message.type=NOTIFYFS_MESSAGE_TYPE_REPLY;

    message.body.reply_message.unique=unique;
    message.body.reply_message.error=error;

    logoutput("send_reply_message: fd: %i ctr %li", fd, (long int) message.body.reply_message.unique);

    if (buffer) {

	send_message(fd, &message, buffer, size);

    } else {
	char dummy[1];

	dummy[0]='\0';

	send_message(fd, &message, (void *) dummy, 1);

    }

}

void send_delwatch_message(int fd, uint64_t unique, unsigned long client_watch_id)
{
    struct notifyfs_message_body message;
    char dummy[1];

    message.type=NOTIFYFS_MESSAGE_TYPE_DELWATCH;

    message.body.delwatch_message.unique=unique;
    message.body.delwatch_message.watch_id=client_watch_id;

    logoutput("send_delwatch_message: ctr %li", (long int) message.body.delwatch_message.unique);

    send_message(fd, &message, (void *) dummy, 1);

}

void send_changewatch_message(int fd, uint64_t unique, unsigned long client_watch_id, unsigned char action)
{
    struct notifyfs_message_body message;
    char dummy[1];

    message.type=NOTIFYFS_MESSAGE_TYPE_CHANGEWATCH;

    message.body.changewatch_message.unique=unique;
    message.body.changewatch_message.watch_id=client_watch_id;
    message.body.changewatch_message.action=action;

    logoutput("send_changewatch_message: ctr %li", (long int) message.body.changewatch_message.unique);

    send_message(fd, &message, (void *) dummy, 1);

}


void send_fsevent_message(int fd, uint64_t unique, unsigned long client_watch_id, struct fseventmask_struct *fseventmask, int entryindex, struct timespec *detect_time)
{
    struct notifyfs_message_body message;
    char dummy[1];

    message.type=NOTIFYFS_MESSAGE_TYPE_FSEVENT;

    message.body.fsevent_message.unique=unique;
    message.body.fsevent_message.fseventmask.attrib_event=fseventmask->attrib_event;
    message.body.fsevent_message.fseventmask.xattr_event=fseventmask->xattr_event;
    message.body.fsevent_message.fseventmask.file_event=fseventmask->file_event;
    message.body.fsevent_message.fseventmask.move_event=fseventmask->move_event;
    message.body.fsevent_message.fseventmask.fs_event=fseventmask->fs_event;

    message.body.fsevent_message.entry=entryindex;

    message.body.fsevent_message.detect_time.tv_sec=detect_time->tv_sec;
    message.body.fsevent_message.detect_time.tv_nsec=detect_time->tv_nsec;

    logoutput("send_fsevent_message: ctr %li", (long int) message.body.fsevent_message.unique);

    send_message(fd, &message, (void *) dummy, 1);

}
