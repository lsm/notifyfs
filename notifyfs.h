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

#ifndef NOTIFYFS_MAIN_H
#define NOTIFYFS_MAIN_H

#include <fuse/fuse_lowlevel.h>

typedef char pathstring[PATH_MAX+1];
typedef char smallpathstring[SMALL_PATH_MAX+1];


struct notifyfs_options_struct {
     char socket[UNIX_PATH_MAX];
     char *mountpoint;
     unsigned char logging;
     unsigned char logarea;
     unsigned char accessmode;
     unsigned char testmode;
     int socket_fd;
     int inotify_fd;
     pathstring pidfile;
     double attr_timeout;
     double entry_timeout;
     double negative_timeout;
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
    off_t underfs_offset;
    struct call_info_struct *call_info;
};


/* inotify specific */

struct inotify_watch_struct {
    int wd;
    struct notifyfs_inode_struct *inode;
    int mask;
    struct inotify_watch_struct *next;
    struct inotify_watch_struct *prev;
};

#endif