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
#ifndef NOTIFYFS_NETWORKSERVERS_H
#define NOTIFYFS_NETWORKSERVERS_H

#define NOTIFYFS_SERVERTYPE_NOTSET				0
#define NOTIFYFS_SERVERTYPE_NETWORK				1
#define NOTIFYFS_SERVERTYPE_LOCALHOST				2

#define NOTIFYFS_SERVERSTATUS_NOTSET				0
#define NOTIFYFS_SERVERSTATUS_DOWN				1
#define NOTIFYFS_SERVERSTATUS_UP				2
#define NOTIFYFS_SERVERSTATUS_ERROR				3

struct notifyfs_server_struct {
    unsigned char owner_id;
    unsigned char type;
    char *buffer;
    size_t lenbuffer;
    struct notifyfs_connection_struct *connection;
    unsigned char status;
    int error;
    struct timespec connect_time;
    pthread_mutex_t mutex;
    void *data;
    void *clientwatches;
    struct notifyfs_server_struct *next;
    struct notifyfs_server_struct *prev;
};

// Prototypes

struct notifyfs_server_struct *get_local_server();
void init_local_server();
struct notifyfs_server_struct *create_notifyfs_server();
void add_server_to_list(struct notifyfs_server_struct *server, unsigned char locked);
void change_status_server(struct notifyfs_server_struct *server, unsigned char status);

#endif
