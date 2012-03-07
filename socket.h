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
#ifndef NOTIFYFS_SOCKET_H
#define NOTIFYFS_SOCKET_H

/* different types of messages:
   - an app registers (client->server)
   - a fs registers (client->server)
   - an app signs off (client->server)
   - this service sends a notify event to an app client (server->client)
   - this service sends a notify watch to client fs (server->client)
   - this service receives an notify event from a client fs (client->server)
*/

#define NOTIFYFS_MESSAGE_TYPE_NOTSET           0
#define NOTIFYFS_MESSAGE_TYPE_REGISTERAPP      1
#define NOTIFYFS_MESSAGE_TYPE_SIGNOFF          2
#define NOTIFYFS_MESSAGE_TYPE_NOTIFYAPP        3
#define NOTIFYFS_MESSAGE_TYPE_REGISTERFS       4
#define NOTIFYFS_MESSAGE_TYPE_SETWATCH         5
#define NOTIFYFS_MESSAGE_TYPE_FSEVENT          6
#define NOTIFYFS_MESSAGE_TYPE_DELWATCH         7
#define NOTIFYFS_MESSAGE_TYPE_SLEEPWATCH       8
#define NOTIFYFS_MESSAGE_TYPE_WAKEWATCH        9


struct client_message_struct {
    unsigned char type;
    unsigned long id;
    int mask;
    int len;
};


/* all in one struct..  to send an notify event to client app, or 
   as request from client app to be informed on a specific path with a certain mask
   or as a message to forward to a client fs, or as an event reported by the fs

   in cases where additional information is required and assumed to be present it's
   send directly after the message has been send
   for example the name of th entry which is deleted when the notify watch has been
   set on an directory and an entry in that directory is removed
*/

/* incase of len>0 the number of bytes of the name of the entry is also send,
   or the path.. */




// Prototypes

void *handle_data_on_connection_fd(struct epoll_event *epoll_event);
void *handle_data_on_socket_fd(struct epoll_event *epoll_event);

int create_socket(char *path);
int send_notify_message(int fd, unsigned char type, unsigned long id, int mask, int len, char *name);

#endif



