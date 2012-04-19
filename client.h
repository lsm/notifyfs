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

#define NOTIFYFS_CLIENTTYPE_UNKNOWN     0
#define NOTIFYFS_CLIENTTYPE_FS          1
#define NOTIFYFS_CLIENTTYPE_APP         2

#define NOTIFYFS_CLIENTSTATUS_NOTSET    0
#define NOTIFYFS_CLIENTSTATUS_UP        1
#define NOTIFYFS_CLIENTSTATUS_DOWN      2
#define NOTIFYFS_CLIENTSTATUS_SLEEP     3


// struct to describe the client
// has a fd it listens to
// has credentials (pid, uid, gid)
// and is part of the list of clients
// has a pointer to custom data:
// - watches for a client app
// - path for a client fs

struct client_struct {
    unsigned char type;
    int fd;
    pid_t pid;
    uid_t uid;
    gid_t gid;
    struct client_struct *next;
    struct client_struct *prev;
    pthread_mutex_t lock_mutex;
    pthread_cond_t lock_condition;
    unsigned char lock;
    unsigned char status_app;
    unsigned char status_fs;
    unsigned char messagemask;
    struct watch_struct *watches;
    struct mount_entry_struct *mount_entry;
    char *path;
};


// Prototypes

struct client_struct *register_client(unsigned int fd, pid_t pid, uid_t uid, gid_t gid);
struct client_struct *lookup_client(pid_t pid, unsigned char lockset);
int lock_clientslist();
int unlock_clientslist();
struct client_struct *get_clientslist();
int lock_client(struct client_struct *client);
int unlock_client(struct client_struct *client);
void assign_mountpoint_clientfs(struct client_struct *client, struct mount_entry_struct *mount_entry);


#endif
