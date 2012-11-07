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
#include <sys/epoll.h>
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
#include "testclient.h"

#include "message.h"
#include "socket.h"
#include "usermenu.h"
#include "processdata.h"

unsigned char loglevel=2;
int logarea=255;
int socket_fd;

/* send a client message, from client to server, like:
   - register a client as app or as fs or both
   - signoff as client at server
   - give messagemask, to inform the server about what messages to receive, like mountinfo
   */

void send_register_message(int fd)
{
    struct notifyfs_message_body message;

    message.type=NOTIFYFS_MESSAGE_TYPE_REGISTER;

    message.body.register_message.type=0;
    message.body.register_message.messagemask=0;
    message.body.register_message.unique=new_uniquectr();

    message.body.register_message.pid=getpid();
    message.body.register_message.tid=message.body.register_message.pid;

    logoutput("send_register_message: ctr %li, pid %i", message.body.register_message.unique, message.body.register_message.pid);

    send_message(fd, &message, NULL, 0);

}


int main(int argc, char *argv[])
{
    int res, epoll_fd;
    struct stat st;
    char socketpath[UNIX_PATH_MAX];
    int c_arg;
    uint64_t midctr;
    pthread_t menuthreadid=0;
    struct epoll_extended_data_struct xdata_socket=EPOLL_XDATA_INIT;
    struct epoll_extended_data_struct *epoll_xdata;

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

    epoll_fd=init_eventloop(NULL, 0, 0);

    if ( epoll_fd<0 ) {

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

    /* add socket to epoll for reading */

    epoll_xdata=add_to_epoll(socket_fd, EPOLLIN, TYPE_FD_SOCKET, &handle_data_on_connection_fd, NULL, &xdata_socket, NULL);

    if ( ! epoll_xdata ) {

        logoutput("Error adding socket fd to mainloop.");
        goto out;

    } else {

        logoutput("socket fd %i added to epoll", socket_fd);

	add_xdata_to_list(epoll_xdata, NULL);

    }

    assign_connection_callback(receive_message);

    /* register at server */

    send_register_message(socket_fd);

    /* start the menu thread */

    start_showmenu_thread(&menuthreadid);

    /* start a mainloop here ...*/

    res=start_epoll_eventloop(NULL);

    if ( xdata_socket.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_socket, NULL);
	close(xdata_socket.fd);
	xdata_socket.fd=0;
	remove_xdata_from_list(&xdata_socket, 0, NULL);

    }

    /* remove any remaining xdata from mainloop */

    destroy_eventloop(NULL);

    if ( menuthreadid ) pthread_cancel(menuthreadid);

    out:

    closelog();

    return res ? 1 : 0;

}
