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
#include <pthread.h>

#define LOG_LOGAREA LOG_LOGAREA_MESSAGE

#include "logging.h"
#include "message.h"
#include "utils.h"


uint64_t uniquectr=0;
pthread_mutex_t uniquectr_mutex=PTHREAD_MUTEX_INITIALIZER;

uint64_t new_uniquectr()
{

    pthread_mutex_lock(&uniquectr_mutex);
    uniquectr++;
    pthread_mutex_unlock(&uniquectr_mutex);

    return uniquectr;
}


/* function to send a raw message */

int send_message(int fd, struct notifyfs_message_body *message, void *data, int len)
{
    struct msghdr msg;
    struct iovec io_vector[2];
    int nreturn=0;

    msg.msg_controllen=0;
    msg.msg_control=NULL;

    msg.msg_name=NULL;
    msg.msg_namelen=0;

    io_vector[0].iov_base=(void *) message;
    io_vector[0].iov_len=sizeof(struct notifyfs_message_body);

    io_vector[1].iov_base=data;
    io_vector[1].iov_len=len;

    msg.msg_iov=io_vector;
    msg.msg_iovlen=2;

    /* the actual sending */

    nreturn=sendmsg(fd, &msg, 0);

    if ( nreturn==-1 ) {

	nreturn=-errno;

    }

    logoutput("send_message: return %i", nreturn);

    out:

    return nreturn;

}

