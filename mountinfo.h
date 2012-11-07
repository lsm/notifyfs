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

#define MOUNTENTRY_CURRENT              0
#define MOUNTENTRY_ADDED                1
#define MOUNTENTRY_REMOVED              2
#define MOUNTENTRY_CURRENT_SORTED       3
#define MOUNTENTRY_REMOVED_KEEP         4
#define MOUNTENTRY_FSTAB		5

#define MOUNT_STATUS_NOTSET             0
#define MOUNT_STATUS_UP                 1
#define MOUNT_STATUS_SLEEP              2
#define MOUNT_STATUS_REMOVE             3

#define MOUNTINFO_CB_ONUPDATE		1
#define MOUNTINFO_CB_NEXT_IN_CURRENT	2
#define MOUNTINFO_CB_NEXT_IN_CHANGED	3
#define MOUNTINFO_CB_LOCK		4
#define MOUNTINFO_CB_UNLOCK		5
#define MOUNTINFO_CB_IGNOREENTRY	6
#define MOUNTINFO_CB_FIRSTRUN		7

#define MOUNTENTRY_TYPEDATA_OBJECT	1
#define MOUNTENTRY_TYPEDATA_MOUNTDATA	2


struct mount_entry_struct {
    unsigned long long unique;
    unsigned long long generation;
    void *index;
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
    unsigned char fstab;
    struct mount_entry_struct *next;
    struct mount_entry_struct *prev;
    struct mount_entry_struct *parent;
    unsigned char typedata0;
    void *data0;
    unsigned char typedata1;
    void *data1;
    unsigned char typedata2;
    void *data2;
};

struct mount_list_struct {
    struct mount_entry_struct *first;
    struct mount_entry_struct *last;
};

struct mountinfo_cb_struct {
    void (*onupdate) (unsigned char firstrun);
    struct mount_entry_struct *(*next_in_current) (struct mount_entry_struct *mount_entry, int direction, unsigned char type);
    struct mount_entry_struct *(*next_in_changed) (struct mount_entry_struct *mount_entry, int direction, unsigned char type);
    int (*lock) ();
    int (*unlock) ();
    unsigned char (*ignore) (char *source, char *fs, char *path);
    void (*firstrun) ();
};


/* prototypes */

unsigned long long get_uniquectr();
void increase_generation_id();
unsigned long long generation_id();

int lock_mountlist();
int unlock_mountlist();

void register_mountinfo_callback(unsigned char type, void *callback);
void run_callback_onupdate(unsigned char firstrun);

unsigned char ignore_mount_entry(char *source, char *fs, char *path);

struct mount_entry_struct *next_mount_entry_changed(struct mount_entry_struct *mount_entry, int direction, unsigned char type);
struct mount_entry_struct *get_next_mount_entry(struct mount_entry_struct *mount_entry, int direction, unsigned char type);

int compare_mount_entries(struct mount_entry_struct *a, struct mount_entry_struct *b);

struct mount_entry_struct *get_mount_entry();
void move_to_unused_list_mount(struct mount_entry_struct *mount_entry);
void add_mount_to_list(struct mount_list_struct *mount_list, struct mount_entry_struct *mount_entry);
void remove_mount_from_list(struct mount_list_struct *mount_list, struct mount_entry_struct *mount_entry);

unsigned char mount_is_up(struct mount_entry_struct *mount_entry);
unsigned char mounted_by_autofs(struct mount_entry_struct *mount_entry);
struct mount_entry_struct *get_rootmount();

struct mount_entry_struct *get_mount(char *path);
struct mount_entry_struct *source_mounted(char *source);

void set_rootmount(struct mount_entry_struct *mount_entry);
unsigned char is_rootmount(struct mount_entry_struct *mount_entry);
unsigned char rootmount_isset();

void logoutput_list(unsigned char type, unsigned char lockset);
int umount_mount_entry(struct mount_entry_struct *mount_entry);

#endif
