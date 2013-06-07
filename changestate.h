/*

  2010, 2011, 2012 Stef Bon <stefbon@gmail.com>

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
#ifndef NOTIFYFS_CHANGESTATE_H
#define NOTIFYFS_CHANGESTATE_H

struct notifyfs_fsevent_struct {
    unsigned char status;
    struct fseventmask_struct fseventmask;
    struct pathinfo_struct pathinfo;
    struct timespec detect_time;
    struct timespec process_time;
    void *data;
    int flags;
    struct notifyfs_fsevent_struct *next;
    struct notifyfs_fsevent_struct *prev;
};


// Prototypes

void queue_fsevent(struct notifyfs_fsevent_struct *fsevent);

struct notifyfs_fsevent_struct *create_fsevent(struct notifyfs_entry_struct *entry);
void init_notifyfs_fsevent(struct notifyfs_fsevent_struct *fsevent);
void destroy_notifyfs_fsevent(struct notifyfs_fsevent_struct *fsevent);

unsigned char compare_attributes(struct stat *cached_st, struct stat *st, struct fseventmask_struct *fseventmask);

void init_changestate(struct workerthreads_queue_struct *workerthreads_queue);
struct notifyfs_fsevent_struct *evaluate_remote_fsevent(struct watch_struct *watch, struct fseventmask_struct *fseventmask, char *name);

unsigned char directory_is_viewed(struct watch_struct *watch);
void update_directory_count(struct watch_struct *watch, unsigned int count);

#endif
