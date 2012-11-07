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

#define ENTRY_STATUS_OK         	0
#define ENTRY_STATUS_REMOVED    	1

#define FSEVENT_INODE_STATUS_OK		1
#define FSEVENT_INODE_STATUS_REMOVED	2
#define FSEVENT_INODE_STATUS_SLEEP	3

#define FSEVENT_INODE_ACTION_NOTSET	0
#define FSEVENT_INODE_ACTION_CREATE	1
#define FSEVENT_INODE_ACTION_REMOVE	2
#define FSEVENT_INODE_ACTION_SLEEP	3
#define FSEVENT_INODE_ACTION_WAKEUP	4

#define FSEVENT_ACTION_TREE_NOTSET	0
#define FSEVENT_ACTION_TREE_UP		1
#define FSEVENT_ACTION_TREE_REMOVE	2

/* defines to distinguish the moments a stat call is done here:

   - when a user browses the notifyfs filesystem, calls like lookup and getattr do a stat on the underlying fs
   - when notifyfs receives an inotify create (IN_CREATE) it does a stat to check the entry 
   - when notifyfs receives an inotify attrib or modify (IN_ATTRIB or IN_MODIFY) it does a stat to get the latest stat 

   simular defines for the filesystem 

*/

#define FSEVENT_STAT_STAT		1
#define FSEVENT_STAT_IN_CREATE		2
#define FSEVENT_STAT_IN_ATTRIB		3
#define FSEVENT_STAT_IN_MODIFY		4

#define FSEVENT_STAT_FS_CREATE		5
#define FSEVENT_STAT_FS_ATTRIB		6
#define FSEVENT_STAT_FS_MODIFY		7


struct notifyfs_inode_struct {
    fuse_ino_t ino;
    uint64_t nlookup;
    struct notifyfs_inode_struct *id_next;
    struct notifyfs_entry_struct *alias;
    struct stat st;
    struct effective_watch_struct *effective_watch;
    unsigned char status;
    struct timespec laststat;
    int lastaction;
    unsigned char method;
};

struct notifyfs_entry_struct {
    char *name;
    struct notifyfs_inode_struct *inode;
    struct notifyfs_entry_struct *name_next;
    struct notifyfs_entry_struct *name_prev;
    struct notifyfs_entry_struct *parent;
    int nameindex_value;
    unsigned char status;
    struct mount_entry_struct *mount_entry;
    unsigned char synced;
};

// Prototypes

int init_hashtables();

void add_to_inode_hash_table(struct notifyfs_inode_struct *inode);
void add_to_name_hash_table(struct notifyfs_entry_struct *entry);
void remove_entry_from_name_hash(struct notifyfs_entry_struct *entry);

struct notifyfs_inode_struct *find_inode(fuse_ino_t ino);
struct notifyfs_entry_struct *find_entry_raw(struct notifyfs_entry_struct *parent, const char *name, unsigned char raw);
struct notifyfs_entry_struct *find_entry(fuse_ino_t ino, const char *name);

struct notifyfs_entry_struct *create_entry(struct notifyfs_entry_struct *parent, const char *name, struct notifyfs_inode_struct *inode);
void remove_entry(struct notifyfs_entry_struct *entry);
void assign_inode(struct notifyfs_entry_struct *entry);
struct notifyfs_entry_struct *new_entry(fuse_ino_t parent, const char *name);

int create_root();
unsigned char isrootinode(struct notifyfs_inode_struct *inode);
unsigned char isrootentry(struct notifyfs_entry_struct *entry);
unsigned long long get_inoctr();
struct notifyfs_inode_struct *get_rootinode();
struct notifyfs_entry_struct *get_rootentry();

struct notifyfs_entry_struct *get_next_entry(struct notifyfs_entry_struct *parent, struct notifyfs_entry_struct *entry);

#endif
