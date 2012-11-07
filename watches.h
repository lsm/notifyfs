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

#ifndef NOTIFYFS_WATCHES_H
#define NOTIFYFS_WATCHES_H

#define FSEVENT_BACKEND_METHOD_NOTSET   0
#define FSEVENT_BACKEND_METHOD_INOTIFY  1
#define FSEVENT_BACKEND_METHOD_POLLING  2
#define FSEVENT_BACKEND_METHOD_FORWARD  4

#define WATCH_ACTION_NOTSET     0
#define WATCH_ACTION_REMOVE     1
#define WATCH_ACTION_SLEEP      2
#define WATCH_ACTION_WAKEUP     3

#define NOTIFYFS_FSEVENT_DATANAME_LEN	255

#define NOTIFYFS_FSEVENT_NOTSET					0
#define NOTIFYFS_FSEVENT_META					1
#define NOTIFYFS_FSEVENT_FILE					2
#define NOTIFYFS_FSEVENT_MOVE					4
#define NOTIFYFS_FSEVENT_ACCESS					8
#define NOTIFYFS_FSEVENT_FS					16

#define NOTIFYFS_FSEVENT_META_NOTSET				0
#define NOTIFYFS_FSEVENT_META_ATTRIB_NOTSET			1
#define NOTIFYFS_FSEVENT_META_ATTRIB_MODE			2
#define NOTIFYFS_FSEVENT_META_ATTRIB_OWNER			4
#define NOTIFYFS_FSEVENT_META_ATTRIB_GROUP			8
#define NOTIFYFS_FSEVENT_META_ATTRIB				14
#define NOTIFYFS_FSEVENT_META_XATTR_NOTSET			16
#define NOTIFYFS_FSEVENT_META_XATTR_CREATE			32
#define NOTIFYFS_FSEVENT_META_XATTR_MODIFY			64
#define NOTIFYFS_FSEVENT_META_XATTR_DELETE			128
#define NOTIFYFS_FSEVENT_META_XATTR				224

#define NOTIFYFS_FSEVENT_FILE_NOTSET				0
#define NOTIFYFS_FSEVENT_FILE_MODIFIED				1
#define NOTIFYFS_FSEVENT_FILE_SIZE				2
#define NOTIFYFS_FSEVENT_FILE_OPEN				4
#define NOTIFYFS_FSEVENT_FILE_READ				8
#define NOTIFYFS_FSEVENT_FILE_CLOSE_WRITE			16
#define NOTIFYFS_FSEVENT_FILE_CLOSE_NOWRITE			32

#define NOTIFYFS_FSEVENT_MOVE_NOTSET				0
#define NOTIFYFS_FSEVENT_MOVE_CREATED				1
#define NOTIFYFS_FSEVENT_MOVE_MOVED				2
#define NOTIFYFS_FSEVENT_MOVE_MOVED_FROM			4
#define NOTIFYFS_FSEVENT_MOVE_MOVED_TO				8
#define NOTIFYFS_FSEVENT_MOVE_DELETED				16

#define NOTIFYFS_FSEVENT_FS_NOTSET				0
#define NOTIFYFS_FSEVENT_FS_MOUNT				1
#define NOTIFYFS_FSEVENT_FS_UNMOUNT				2
#define NOTIFYFS_FSEVENT_FS_NLINKS				4

struct effective_watch_struct {
    struct notifyfs_inode_struct *inode;
    unsigned int mask;
    unsigned int nrwatches;
    struct watch_struct *watches;
    pthread_mutex_t lock_mutex;
    pthread_cond_t lock_condition;
    unsigned char lock;
    struct effective_watch_struct *next;
    struct effective_watch_struct *prev;
    struct effective_watch_struct *next_hash1;
    struct effective_watch_struct *prev_hash1;
    unsigned char typebackend;
    void *backend;
    unsigned long id;
    unsigned long backend_id;
    unsigned long inotify_id;
    unsigned char backendset;
    char *path;
    int hash1;
    struct mount_entry_struct *mount_entry;
    time_t laststat;
};

// struct to describe the watch which has been set
// it has been set on an inode by a client
// and has a mask
// is part of a list per inode
// is part of a list per client

struct watch_struct {
    unsigned int mask;
    struct effective_watch_struct *effective_watch;
    struct client_struct *client;
    unsigned long id;
    int client_watch_id;
    struct watch_struct *next_per_watch;
    struct watch_struct *prev_per_watch;
    struct watch_struct *next_per_client;
    struct watch_struct *prev_per_client;
};

struct notifyfs_fsevent_struct {
    int group;
    int type;
    struct notifyfs_entry_struct *entry;
    union {
	struct stat st;
	char name[NOTIFYFS_FSEVENT_DATANAME_LEN];
    } data;
};


// Prototypes

int init_watch_hashtables();
void add_watch_to_hashtable1(struct effective_watch_struct *eff_watch, unsigned long long id);
void remove_watch_from_hashtable1(struct effective_watch_struct *eff_watch, unsigned long long id);
struct effective_watch_struct *get_next_eff_watch_hash1(struct effective_watch_struct *effective_watch, unsigned long long id);

int lock_effective_watches();
int unlock_effective_watches();

unsigned long new_watchid();
struct effective_watch_struct *get_effective_watch();

void add_effective_watch_to_list(struct effective_watch_struct *effective_watch);
void remove_effective_watch_from_list(struct effective_watch_struct *effective_watch, unsigned char lockset);

void move_effective_watch_to_unused(struct effective_watch_struct *effective_watch);

struct effective_watch_struct *lookup_watch(unsigned char type, unsigned long id);
int calculate_effmask(struct effective_watch_struct *effective_watch, unsigned char lockset);
struct effective_watch_struct *get_next_effective_watch(struct effective_watch_struct *effective_watch);
int lock_effective_watch(struct effective_watch_struct *effective_watch);
int unlock_effective_watch(struct effective_watch_struct *effective_watch);

struct watch_struct *get_watch();
int add_new_client_watch(struct effective_watch_struct *effective_watch, int mask, int client_watch_id, struct client_struct *client);
void remove_client_watch_from_inode(struct watch_struct *watch);
void remove_client_watch_from_client(struct watch_struct *watch);

int check_for_effective_watch(char *path);

int get_clientmask(struct effective_watch_struct *effective_watch, pid_t pid, unsigned char lockset);

// void set_backend(struct call_info_struct *call_info, struct effective_watch_struct *effective_watch);

void init_effective_watches();
int set_mount_entry_effective_watch(struct call_info_struct *call_info, struct effective_watch_struct *effective_watch);

void set_watch_backend_os_specific(struct effective_watch_struct *effective_watch, char *path, int mask);
void change_watch_backend_os_specific(struct effective_watch_struct *effective_watch, char *path, int mask);
void remove_watch_backend_os_specific(struct effective_watch_struct *effective_watch);

void send_notify_message_clients(struct effective_watch_struct *effective_watch, int mask, int len, char *name, struct stat *st, unsigned char remote);
void send_status_message_clients(struct effective_watch_struct *effective_watch, unsigned char typemessage);

#endif
