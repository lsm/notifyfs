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
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>
#include <sys/time.h>
#include <assert.h>
#include <syslog.h>
#include <time.h>

#include <inttypes.h>

// required??

#include <ctype.h>

#include <sys/types.h>
#include <sys/inotify.h>
#include <pthread.h>


#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

#include "logging.h"
#include "epoll-utils.h"
#include "testapp.h"
#include "client.h"

#include "message.h"
#include "message-client.h"
#include "socket.h"

unsigned char loglevel=2;
int logarea=255;

void log_mountmessage(struct notifyfs_mount_message *mount_message, char *mountpoint, char *rootpath)
{

    logoutput("received a mount message: ");

    if ( mount_message->isbind==1 ) {

        logoutput("(bind) %s on %s type %s", rootpath, mountpoint, mount_message->fstype);

    } else if ( mount_message->autofs_mounted==1 ) {

        logoutput("(mounted by autofs) %s on %s type %s", mount_message->mountsource, mountpoint, mount_message->fstype);

    } else {

        logoutput("%s on %s type %s", mount_message->mountsource, mountpoint, mount_message->fstype);

    }

}

int main(int argc, char *argv[])
{
    int res, epoll_fd, socket_fd;
    struct stat st;
    char socketpath[UNIX_PATH_MAX];
    int c_arg;
    uint64_t midctr;

    // set logging

    openlog("testapp", 0,0); 

    memset(socketpath, '\0', UNIX_PATH_MAX);

    while((c_arg=getopt(argc, argv, "s:"))>=0) {

        if ( c_arg == 's' ) {

            if ( optarg ) {

                strcpy(socketpath, optarg);

            }

        }

    }

    if (strlen(socketpath)==0) {

        printf("path to socket not set, cannot continue\n");
        goto out;

    }


    epoll_fd=init_mainloop();

    if ( epoll_fd<=0 ) {

        logoutput("Error creating epoll fd, error: %i.", epoll_fd);
        goto out;

    } else {

	logoutput("Init mainloop, epoll fd: %i", epoll_fd);

    }

    /*
        create the socket clients can connect to

    */

    socket_fd=connect_socket(socketpath);

    if ( socket_fd<=0 ) {

        logoutput("Error creating socket fd: %i.", socket_fd);
        goto out;

    }

    /* add socket to epoll */

    res=add_to_epoll(socket_fd, EPOLLIN | EPOLLPRI, TYPE_FD_SOCKET, &handle_data_on_connection_fd, NULL);
    if ( res<0 ) {

        logoutput("Error adding socket fd to epoll: %i.", res);
        goto out;

    } else {

        logoutput("socket fd %i added to epoll", socket_fd);

    }

    /* assign the right callback when receiving mountinfo */

    assign_message_callback_client(NOTIFYFS_MESSAGE_TYPE_MOUNTINFO, &log_mountmessage);

    sleep(1);

    /* register at server and ask for mountinfo */

    send_client_message(socket_fd, NOTIFYFS_MESSAGE_CLIENT_REGISTERAPP, NULL, NOTIFYFS_MESSAGE_MASK_MOUNT);

    sleep(1);

    midctr=new_uniquectr();

    /* request the server for the current mounts */

    send_mountinfo_request(socket_fd, NULL, NULL, midctr);

    /* start a mainloop here ...*/

    res=epoll_mainloop();

    out:

    closelog();

    return res ? 1 : 0;

}
