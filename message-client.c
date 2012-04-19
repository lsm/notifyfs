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

#define LOG_LOGAREA LOG_LOGAREA_MESSAGE

#include "logging.h"
#include "client.h"

#include "message.h"
#include "message-client.h"
#include "mountstatus.h"

static struct notifyfs_message_callbacks message_cb={NULL, NULL, NULL, NULL};

void assign_message_callback_client(unsigned char type, void *callback)
{

    assign_message_callback(type, callback, &message_cb);

}

int receive_message_from_server(int fd)
{
    return receive_message(fd, NULL, &message_cb);

}
