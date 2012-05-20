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

//#include <sys/socket.h>
//#include <netinet/in.h>
//#include <netdb.h>
// #include <arpa/inet.h>

#include <sys/epoll.h>
#include <pthread.h>

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

#include "logging.h"
#include "notifyfs.h"
#include "epoll-utils.h"
#include "watches.h"

#include "mountinfo.h"
#include "message.h"

#include "networksocket.h"

#define LISTEN_BACKLOG 50

struct sockaddr_in address;
socklen_t address_length;
int networksocket_fd;

struct notifyfsserver_struct *notifyfsserver_list=NULL;
pthread_mutex_t notifyfsserver_list_mutex=PTHREAD_MUTEX_INITIALIZER;

struct notifyfs_message_callbacks message_cb={NULL, NULL, NULL, NULL};

void assign_message_callback_notifyfsserver(unsigned char type, void *callback)
{

    assign_message_callback(type, callback, &message_cb);

}

int receive_message_from_notifyfsserver(int fd)
{
    return receive_message(fd, NULL, &message_cb);

}

/* function to read when data arrives on a network connection fd 
   the remote site can be a server (data is fsevent) 
   or a client (data is a setwatch request) */

void handle_data_on_networkconnection_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo)
{
    int nreturn=0;

    if ( events & ( EPOLLHUP | EPOLLRDHUP ) ) {
	struct notifyfsserver_struct *notifyfsserver=(struct notifyfsserver_struct *) epoll_xdata->data;

        nreturn=remove_xdata_from_epoll(epoll_xdata, 0);

        if ( nreturn<0 ) {

            logoutput("error(%i) removing xdata from epoll\n", nreturn);

        }

	close(epoll_xdata->fd);
	epoll_xdata->fd=0;

	remove_xdata_from_list(epoll_xdata);

	free(epoll_xdata);

	if ( notifyfsserver ) {

	    /* set server down */

	    notifyfsserver->status=NOTIFYFS_SERVERSTATUS_DOWN;
	    notifyfsserver->fd=0;

	}

    } else {
        int nlenread=0;
        struct notifyfsserver_struct *notifyfsserver=(struct notifyfsserver_struct *) epoll_xdata->data;

        /* here handle the data available for read */

        logoutput("handle_data_on_networkconnection_fd %i", epoll_xdata->fd);

	/* following reads the data (checks it versus the remote role) and does the right action */

	nlenread=receive_message_from_notifyfsserver(epoll_xdata->fd);

    }

}

/* function to register a remote notifyfs server
   this can be server which connects to this server or a 
   server this server connects to*/

struct notifyfsserver_struct *register_notifyfsserver(unsigned int fd, unsigned char initiator)
{
    struct notifyfsserver_struct *notifyfsserver=NULL;
    int res=0, nreturn=0;
    struct epoll_extended_data_struct *server_xdata;
    socklen_t lensocket;

    logoutput1("register_notifyfsserver for fd %i", fd);

    notifyfsserver=malloc(sizeof(struct notifyfsserver_struct));

    if ( ! notifyfsserver ) goto out;

    memset(notifyfsserver, 0, sizeof(struct notifyfsserver_struct));

    notifyfsserver->fd=fd;
    notifyfsserver->next=NULL;
    notifyfsserver->prev=NULL;
    notifyfsserver->status=NOTIFYFS_SERVERSTATUS_NOTSET;
    notifyfsserver->initiator=initiator;

    /* get info about the connection local and remote */

    lensocket=sizeof(struct sockaddr_in);

    res=getsockname(fd, (struct sockaddr *) &notifyfsserver->localaddr, &lensocket);
    res=getpeername(fd, (struct sockaddr *) &notifyfsserver->remoteaddr, &lensocket);

    /* add to epoll */

    server_xdata=add_to_epoll(fd, EPOLLIN | EPOLLET, TYPE_FD_SERVER, &handle_data_on_networkconnection_fd, (void *) notifyfsserver, NULL);

    if ( ! server_xdata ) {

	logoutput("handle_data_on_networksocket_fd: error adding server fd %i to mainloop", fd);

    } else {

	add_xdata_to_list(server_xdata);

    }

    res=pthread_mutex_lock(&notifyfsserver_list_mutex);

    /* insert at begin of list */

    if ( notifyfsserver_list ) notifyfsserver_list->prev=notifyfsserver;
    notifyfsserver->next=notifyfsserver_list;
    notifyfsserver->prev=NULL;
    notifyfsserver_list=notifyfsserver;

    res=pthread_mutex_unlock(&notifyfsserver_list_mutex);

    out:

    return notifyfsserver;

}


/*
* function to handle when data arrives on the networksocket
* this means that a remote server tries to connect
*/

void handle_data_on_networksocket_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo)
{
    int connection_fd;

    logoutput("handle_data_on_networksocket_fd");

    /* add a new fd for this server to communicate */

    connection_fd=accept4(epoll_xdata->fd, (struct sockaddr *) &address, &address_length, SOCK_NONBLOCK);

    if ( connection_fd==-1 ) {

        return;

    } else {

	/* add server, remote server is initiator */

	register_notifyfsserver(connection_fd, 0);

	/* send a message to the remote client */

	/* hello remote server, please send what you want */

    }

}

/* create a network socket other hosts can connect to */

int create_networksocket(int port)
{
    int nreturn=0;

    logoutput("create_networksocket: open networksocket for %i", port);

    /* add socket */

    networksocket_fd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);

    if ( networksocket_fd < 0 ) {

        nreturn=-errno;
        goto out;

    }

    nreturn=networksocket_fd;

    /* bind path/familiy and socket */

    memset(&address, 0, sizeof(struct sockaddr_in));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_ANY);
    address.sin_port=htons(port);

    address_length=sizeof((struct sockaddr *) &address);

    if ( bind(networksocket_fd, (struct sockaddr *) &address, sizeof(struct sockaddr_in)) != 0) {

        nreturn=-errno;
        goto out;

    }

    /* listen */

    if ( listen(networksocket_fd, LISTEN_BACKLOG) != 0 ) {

        nreturn=-errno;

    }

    out:

    return nreturn;

}

/* connect to a remote server 
   serveraddress is determined at runtime, is a ipv4 address
   port is the port set at commandline */

int connect_to_remote_notifyfsserver(char *serveraddress, int port)
{
    int connection_fd=0, nreturn=0;

    logoutput("connect_to_remote_notifyfsserver: trying to connect to %s:%i", serveraddress, port);

    connection_fd=socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

    if ( connection_fd<0 ) {

	nreturn=-errno;
	goto out;

    } else {
	struct sockaddr_in serversocket;

	nreturn=connection_fd;

	memset(&serversocket, 0, sizeof(struct sockaddr_in));

	serversocket.sin_family=AF_INET;
	serversocket.sin_addr.s_addr=inet_addr(serveraddress);
	serversocket.sin_port=htons(port);

	if ( connect(connection_fd, (struct sockaddr *) &serversocket,sizeof(struct sockaddr_in))<0 ) {

	    logoutput("connect_to_remote_notifyfsserver: unable to connect to remote server %s (error: %i)", serveraddress, errno);

	    nreturn=-errno;

	}

    }

    out:

    return nreturn;

}


/* look for notifyfsserver using fd */

struct notifyfsserver_struct *lookup_notifyfsserver_perfd(int fd, unsigned char lockset)
{
    struct notifyfsserver_struct *notifyfsserver=NULL;
    int res;

    if (lockset==0) res=pthread_mutex_lock(&notifyfsserver_list_mutex);

    notifyfsserver=notifyfsserver_list;
    while (notifyfsserver) {

        if ( notifyfsserver->fd==fd ) break;

        notifyfsserver=notifyfsserver->next;

    }

    if (lockset==0) res=pthread_mutex_unlock(&notifyfsserver_list_mutex);

    return notifyfsserver;

}

/* look for notifyfsserver using the remote ipv4address */

struct notifyfsserver_struct *lookup_notifyfsserver_peripv4(char *ipv4address, unsigned char lockset)
{
    struct notifyfsserver_struct *notifyfsserver=NULL;
    int res;

    if ( ! ipv4address ) goto out;

    if (lockset==0) res=pthread_mutex_lock(&notifyfsserver_list_mutex);

    notifyfsserver=notifyfsserver_list;

    while (notifyfsserver) {

	if ( notifyfsserver->remoteaddr.sa_family==AF_INET ) {
	    struct sockaddr_in *sinp=(struct sockaddr_in *) &(notifyfsserver->remoteaddr);

	    if ( sinp->sin_addr.s_addr==inet_addr(ipv4address)) break;

	}

        notifyfsserver=notifyfsserver->next;

    }

    if (lockset==0) res=pthread_mutex_unlock(&notifyfsserver_list_mutex);

    out:

    return notifyfsserver;

}


int lock_notifyfsserverlist()
{
    return pthread_mutex_lock(&notifyfsserver_list_mutex);
}

int unlock_notifyfsserverlist()
{
    return pthread_mutex_unlock(&notifyfsserver_list_mutex);
}


