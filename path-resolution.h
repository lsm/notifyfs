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

#ifndef NOTIFYFS_PATH_RESOLUTION_H
#define NOTIFYFS_PATH_RESOLUTION_H

#define NOTIFYFS_PATH_NONE      0
#define NOTIFYFS_PATH_FORCE     1
#define NOTIFYFS_PATH_BACKEND   2

struct call_info_struct {
    struct notifyfs_entry_struct *entry;
    struct notifyfs_entry_struct *entry2remove;
    struct effective_watch_struct *effective_watch;
    pthread_t threadid;
    char *path;
    unsigned char freepath;
    void *backend;
    unsigned char typebackend;
    struct call_info_struct *next;
    struct call_info_struct *prev;
    struct mount_entry_struct *mount_entry;
    const struct fuse_ctx *ctx;
    struct client_struct *client;
};

// Prototypes

int determine_path(struct call_info_struct *call_info, unsigned char flags);

struct call_info_struct *get_call_info(struct notifyfs_entry_struct *entry);
void init_call_info(struct call_info_struct *call_info, struct notifyfs_entry_struct *entry);
void remove_call_info(struct call_info_struct *call_info);

#endif
