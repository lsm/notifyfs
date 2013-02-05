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

// Prototypes

int create_root();

struct notifyfs_inode_struct *find_inode(fuse_ino_t ino);
struct notifyfs_entry_struct *find_entry_by_ino(fuse_ino_t ino, const char *name);
struct notifyfs_entry_struct *find_entry_by_entry(struct notifyfs_entry_struct *parent, const char *name);

struct notifyfs_entry_struct *create_entry(struct notifyfs_entry_struct *parent, const char *name);
void remove_entry(struct notifyfs_entry_struct *entry);
void assign_inode(struct notifyfs_entry_struct *entry);
struct notifyfs_attr_struct *assign_attr(struct stat *st, struct notifyfs_inode_struct *inode);

struct notifyfs_mount_struct *create_mount(char *fs, char *mountsource, char *superoptions, struct notifyfs_entry_struct *entry);
void remove_mount(struct notifyfs_mount_struct *mount);

unsigned char isrootinode(struct notifyfs_inode_struct *inode);
unsigned char isrootentry(struct notifyfs_entry_struct *entry);

unsigned long long get_inoctr();
struct notifyfs_inode_struct *get_rootinode();
struct notifyfs_entry_struct *get_rootentry();
void assign_rootentry();

void copy_stat_to_notifyfs(struct stat *st, struct notifyfs_entry_struct *entry);
void get_stat_from_cache(struct stat *st, struct notifyfs_entry_struct *entry);
void get_stat_from_notifyfs(struct stat *st, struct notifyfs_entry_struct *entry);

void notify_kernel_delete(struct fuse_chan *chan, struct notifyfs_entry_struct *entry);

#endif
