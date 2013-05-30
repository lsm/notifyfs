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
#ifndef NOTIFYFS_CLIENT_H
#define NOTIFYFS_CLIENT_H

#define NOTIFYFS_CLIENTSTATUS_NOTSET    0
#define NOTIFYFS_CLIENTSTATUS_UP        1
#define NOTIFYFS_CLIENTSTATUS_DOWN      2
#define NOTIFYFS_CLIENTSTATUS_SLEEP     3


struct client_struct {
    unsigned long owner_id;
    unsigned char type;
    pid_t pid;
    pid_t tid;
    uid_t uid;
    gid_t gid;
    struct client_struct *next;
    struct client_struct *prev;
    pthread_mutex_t mutex;
    unsigned char status;
    int mode;
    void *clientwatches;
    struct notifyfs_connection_struct *connection;
    char *buffer;
    size_t lenbuffer;
    void *data;
};

// Prototypes

struct client_struct *register_client(unsigned int fd, pid_t pid, uid_t uid, gid_t gid, unsigned char type, int mode);
struct client_struct *lookup_client(pid_t pid, unsigned char lockset);
void remove_client(struct client_struct *client);

int lock_clientslist();
int unlock_clientslist();

struct client_struct *get_next_client(struct client_struct *client);

void lock_client(struct client_struct *client);
void unlock_client(struct client_struct *client);

void add_client_to_list(struct client_struct *client);
void remove_client_from_list(struct client_struct *client);

#endif
