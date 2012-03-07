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

#define ENTRY_STATUS_OK         0
#define ENTRY_STATUS_REMOVED    1

#define BACKEND_METHOD_NOTSET   0
#define BACKEND_METHOD_INOTIFY  1
#define BACKEND_METHOD_POLLING  2
#define BACKEND_METHOD_FORWARD  3

#define TESTFS_PATH_NONE      0
#define TESTFS_PATH_FORCE     1
#define TESTFS_PATH_BACKEND   2

#define WATCH_ACTION_NOTSET     0
#define WATCH_ACTION_REMOVE     1
#define WATCH_ACTION_SLEEP      2
#define WATCH_ACTION_WAKEUP     3

struct testfs_inode_struct {
    fuse_ino_t ino;
    uint64_t nlookup;
    struct testfs_inode_struct *id_next;
    struct testfs_entry_struct *alias;
    struct stat st;
    struct effective_watch_struct *effective_watch;
};

struct effective_watch_struct {
    struct testfs_inode_struct *inode;
    unsigned int mask; /* every inode has a mask here but not used always (then 0 )*/
    pthread_mutex_t lock_mutex;
    pthread_cond_t lock_condition;
    unsigned char lock;
    struct effective_watch_struct *next;
    struct effective_watch_struct *prev;
    unsigned long id;
};


struct testfs_entry_struct {
    char *name;
    struct testfs_inode_struct *inode;
    struct testfs_entry_struct *name_next;
    struct testfs_entry_struct *name_prev;
    struct testfs_entry_struct *parent;
    struct testfs_entry_struct *dir_next;
    struct testfs_entry_struct *dir_prev;
    struct testfs_entry_struct *child; /* in case of a directory pointing to the first child */
    size_t namehash;
    unsigned char status;
    void *ptr;
};

struct call_info_struct {
    struct testfs_entry_struct *entry;
    struct testfs_entry_struct *entry2remove;
    pthread_t threadid;
    char *path;
    void *backend;
    unsigned char typebackend;
    struct call_info_struct *next;
    struct call_info_struct *prev;
    void *ptr;
};

// Prototypes

int init_hashtables();
void add_to_inode_hash_table(struct testfs_inode_struct *inode);
void add_to_name_hash_table(struct testfs_entry_struct *entry);
void remove_entry_from_name_hash(struct testfs_entry_struct *entry);
struct testfs_inode_struct *find_inode(fuse_ino_t inode);
struct testfs_entry_struct *find_entry(fuse_ino_t parent, const char *name);
struct testfs_entry_struct *create_entry(struct testfs_entry_struct *parent, const char *name, struct testfs_inode_struct *inode);
void remove_entry(struct testfs_entry_struct *entry);
void assign_inode(struct testfs_entry_struct *entry);
struct testfs_entry_struct *new_entry(fuse_ino_t parent, const char *name);
unsigned char rootinode(struct testfs_inode_struct *inode);
unsigned long long get_inoctr();

int determine_path(struct call_info_struct *call_info, unsigned char flags);

struct effective_watch_struct *get_effective_watch();
void add_effective_watch_to_list(struct effective_watch_struct *effective_watch);
void remove_effective_watch_from_list(struct effective_watch_struct *effective_watch);
struct effective_watch_struct *lookup_watch(unsigned char type, unsigned long id);

struct call_info_struct *get_call_info(struct testfs_entry_struct *entry);
void init_call_info(struct call_info_struct *call_info, struct testfs_entry_struct *entry);
int lookup_call_info(char *path, unsigned char lockset);
int wait_for_calls(char *path);
void remove_call_info(struct call_info_struct *call_info);

#endif
