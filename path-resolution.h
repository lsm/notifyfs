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

typedef char pathstring[PATH_MAX+1];

#define NOTIFYFS_PATH_NONE      	0
#define NOTIFYFS_PATH_FORCE     	1
#define NOTIFYFS_PATH_ACCESS		2

#define NOTIFYFS_PATHINFOFLAGS_NONE		0
#define NOTIFYFS_PATHINFOFLAGS_ALLOCATED	1
#define NOTIFYFS_PATHINFOFLAGS_INUSE		2

struct pathinfo_struct {
    char *path;
    int len;
    unsigned char flags;
};

struct call_info_struct {
    struct notifyfs_entry_struct *entry;
    pthread_t threadid;
    struct pathinfo_struct pathinfo;
    struct stat *st;
    unsigned char flags;
    unsigned char entrycreated;
    unsigned char strict;
    int mount;
    pid_t pid;
    uid_t uid;
    gid_t gid;
    unsigned char mask;
    unsigned int error;
};

struct notifyfs_generic_fh_struct {
    struct notifyfs_entry_struct *entry;
    int fd;
    void *data;
};

struct notifyfs_generic_dirp_struct {
    struct notifyfs_generic_fh_struct generic_fh;
    struct notifyfs_entry_struct *entry;
    struct stat st;
    off_t upperfs_offset;
};

// Prototypes

int check_access_path(char *path, pid_t pid, uid_t uid, gid_t gid, struct stat *st, unsigned char mask);
int determine_path(struct notifyfs_entry_struct *entry, struct pathinfo_struct *pathinfo);

void init_call_info(struct call_info_struct *call_info, struct notifyfs_entry_struct *entry);

struct notifyfs_entry_struct *create_notifyfs_path(struct pathinfo_struct *pathinfo, struct stat *st, unsigned char strict, int mask, int *error, pid_t pid, uid_t uid, gid_t gid);

void free_path_pathinfo(struct pathinfo_struct *pathinfo);

#endif
