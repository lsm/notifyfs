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
#ifndef NETWORKSOCKET_H
#define NETWORKSOCKET_H

#include <sys/socket.h>
#include <netinet/in.h>

#define NOTIFYFS_SERVERSTATUS_NOTSET    0
#define NOTIFYFS_SERVERSTATUS_UP        1
#define NOTIFYFS_SERVERSTATUS_DOWN      2
#define NOTIFYFS_SERVERSTATUS_SLEEP     3

struct notifyfsserver_struct {
    int fd;
    struct notifyfsserver_struct *next;
    struct notifyfsserver_struct *prev;
    pthread_mutex_t lock_mutex;
    pthread_cond_t lock_condition;
    struct sockaddr localaddr;
    struct sockaddr remoteaddr;
    unsigned char lock;
    unsigned char status;
    unsigned char initiator;
};


// Prototypes

void handle_data_on_networkconnection_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo);
int create_networksocket(int port);
void handle_data_on_networksocket_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo);
int create_networksocket(int port);
int connect_networksocket(char *serveraddress, int port);
struct notifyfsserver_struct *register_notifyfsserver(unsigned int fd, unsigned char type);

struct notifyfsserver_struct *lookup_notifyfsserver_perfd(int fd, unsigned char lockset);
struct notifyfsserver_struct *lookup_notifyfsserver_peripv4(char *ipv4address, unsigned char lockset);

int lock_notifyfsserverlist();
int unlock_notifyfsserverlist();


#endif
