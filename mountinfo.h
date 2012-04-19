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

#ifndef MOUNTINFO_H
#define MOUNTINFO_H

#define MOUNTINFO "/proc/self/mountinfo"
#define FSTAB     "/etc/fstab"

#define MOUNTENTRY_CURRENT              0
#define MOUNTENTRY_NEW                  1
#define MOUNTENTRY_ADDED                2
#define MOUNTENTRY_REMOVED              3
#define MOUNTENTRY_CURRENT_SORTED       4
#define MOUNTENTRY_NEW_SORTED           5
#define MOUNTENTRY_REMOVED_KEEP         6

struct mountinfo_entry_struct {
    int mountid;
    int parentid;
    struct mountinfo_entry_struct *next;
    struct mountinfo_entry_struct *prev;
    struct mountinfo_entry_struct *s_next;
    struct mountinfo_entry_struct *s_prev;
    struct mount_entry_struct *mount_entry;
};

struct mount_entry_struct {
    struct mountinfo_entry_struct *mountinfo_entry;
    char *mountpoint;
    char fstype[64];
    char mountsource[64];
    char superoptions[256];
    char *rootpath;
    int minor;
    int major;
    unsigned char isbind;
    unsigned char isroot;
    unsigned char isautofs;
    unsigned char autofs_indirect;
    unsigned char autofs_mounted;
    unsigned char status;
    unsigned char remount;
    unsigned char processed;
    int refcount;
    struct mount_entry_struct *next;
    struct mount_entry_struct *prev;
    struct mount_entry_struct *parent;
    void *entry;
    void *client;
};

struct mountinfo_list_struct {
    struct mountinfo_entry_struct *first;
    struct mountinfo_entry_struct *last;
    struct mountinfo_entry_struct *s_first;
    struct mountinfo_entry_struct *s_last;
};

struct mount_list_struct {
    struct mount_entry_struct *first;
    struct mount_entry_struct *last;
};

int get_new_mount_list(struct mountinfo_list_struct *mi_list);
void set_parents_raw(struct mountinfo_list_struct *mi_list);
struct mount_entry_struct *get_next_mount_entry(struct mount_entry_struct *mount_entry, unsigned char type);
void signal_mountmonitor(unsigned char doinit);
int start_mountmonitor_thread(pthread_t *pthreadid);
int lock_mountlist(unsigned char type);
int unlock_mountlist(unsigned char type);

unsigned char mount_is_up(struct mount_entry_struct *mount_entry);
unsigned char mounted_by_autofs(struct mount_entry_struct *mount_entry);
struct mount_entry_struct *get_rootmount();


#endif
