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

#ifndef ENTRY_MANAGEMENT_H
#define ENTRY_MANAGEMENT_H

#define ENTRY_STATUS_OK         		0
#define ENTRY_STATUS_REMOVED    		1

#define FSEVENT_INODE_STATUS_OK			1
#define FSEVENT_INODE_STATUS_TOBEREMOVED	2
#define FSEVENT_INODE_STATUS_REMOVED		3
#define FSEVENT_INODE_STATUS_TOBEUNMOUNTED	4
#define FSEVENT_INODE_STATUS_UNMOUNTED		5
#define FSEVENT_INODE_STATUS_SLEEP		6

#define FSEVENT_INODE_ACTION_NOTSET		0
#define FSEVENT_INODE_ACTION_CREATE		1
#define FSEVENT_INODE_ACTION_REMOVE		2
#define FSEVENT_INODE_ACTION_SLEEP		3
#define FSEVENT_INODE_ACTION_WAKEUP		4

struct notifyfs_backend_struct {
    unsigned char type;
    char *buffer;
    size_t lenbuffer;
    struct notifyfs_connection_struct *connection;
    unsigned char status;
    int error;
    struct timespec connect_time;
    pthread_mutex_t mutex;
    void *data;
    int refcount;
    struct notifyfs_backend_struct *next;
    struct notifyfs_backend_struct *prev;
};

struct supermount_struct {
    int major;
    int minor;
    int refcount;
    struct notifyfs_filesystem_struct *fs;
    char *source;
    char *options;
    struct notifyfs_backend_struct *backend;
    struct backendfunctions_struct *backendfunctions;
    struct supermount_struct *next;
    struct supermount_struct *prev;
};

// Prototypes

int init_entry_management();

unsigned long new_owner_id();

struct notifyfs_inode_struct *find_inode(fuse_ino_t ino);
struct notifyfs_entry_struct *find_entry_by_ino(fuse_ino_t ino, const char *name);
struct notifyfs_entry_struct *find_entry_by_entry(struct notifyfs_entry_struct *parent, const char *name);

struct notifyfs_entry_struct *create_entry(struct notifyfs_entry_struct *parent, const char *name);
void remove_entry(struct notifyfs_entry_struct *entry);

void assign_inode(struct notifyfs_entry_struct *entry);

struct notifyfs_attr_struct *assign_attr(struct stat *st, struct notifyfs_inode_struct *inode);
void remove_attr(struct notifyfs_attr_struct *attr);

struct notifyfs_mount_struct *create_mount(struct notifyfs_entry_struct *entry, int major, int minor);
void remove_mount(struct notifyfs_mount_struct *mount);
struct notifyfs_mount_struct *find_mount_majorminor(int major, int minor, struct notifyfs_mount_struct *new_mount);
struct notifyfs_backend_struct *get_mount_backend(struct notifyfs_mount_struct *mount);

struct supermount_struct *add_supermount(int major, int minor, char *source, char *options);
struct supermount_struct *find_supermount_majorminor(int major, int minor);
int remove_mount_supermount(struct supermount_struct *supermount);

void activate_view(struct view_struct *view);
struct view_struct *get_next_view(pid_t pid, void **index);

unsigned char isrootinode(struct notifyfs_inode_struct *inode);
unsigned char isrootentry(struct notifyfs_entry_struct *entry);

int create_root();
unsigned long long get_inoctr();
struct notifyfs_inode_struct *get_rootinode();
struct notifyfs_entry_struct *get_rootentry();
void assign_rootentry();

void copy_stat_to_notifyfs(struct stat *st, struct notifyfs_entry_struct *entry);
void get_stat_from_cache(struct stat *st, struct notifyfs_entry_struct *entry);
void get_stat_from_notifyfs(struct stat *st, struct notifyfs_entry_struct *entry);

void notify_kernel_delete(struct fuse_chan *chan, struct notifyfs_entry_struct *entry);

#endif
