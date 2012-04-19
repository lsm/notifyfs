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

#define FSEVENT_INODE_STATUS_OK		1
#define FSEVENT_INODE_STATUS_REMOVE	2
#define FSEVENT_INODE_STATUS_SLEEP	3

#define FSEVENT_INODE_ACTION_NOTSET	0
#define FSEVENT_INODE_ACTION_CREATE	1
#define FSEVENT_INODE_ACTION_REMOVE	2
#define FSEVENT_INODE_ACTION_SLEEP	3
#define FSEVENT_INODE_ACTION_WAKEUP	4

#define FSEVENT_ACTION_TREE_NOTSET	0
#define FSEVENT_ACTION_TREE_UP		1
#define FSEVENT_ACTION_TREE_REMOVE	2

#define FSEVENT_FILECHANGED_NONE	0
#define FSEVENT_FILECHANGED_FILE	1
#define FSEVENT_FILECHANGED_METADATA	2

struct testfs_inode_struct {
    fuse_ino_t ino;
    uint64_t nlookup;
    struct testfs_inode_struct *id_next;
    struct testfs_entry_struct *alias;
    struct stat st;
    struct effective_watch_struct *effective_watch;
    unsigned char status;
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
    struct mount_entry_struct *mount_entry;
    unsigned char synced;
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

int create_root();
unsigned char isrootinode(struct testfs_inode_struct *inode);
unsigned long long get_inoctr();
struct testfs_inode_struct *get_rootinode();
struct testfs_entry_struct *get_rootentry();

#endif
