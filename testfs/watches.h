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

#ifndef TESTFS_WATCHES_H
#define TESTFS_WATCHES_H

#define FSEVENT_BACKEND_METHOD_NOTSET   0
#define FSEVENT_BACKEND_METHOD_INOTIFY  1

#define WATCH_ACTION_NOTSET     0
#define WATCH_ACTION_REMOVE     1
#define WATCH_ACTION_SLEEP      2
#define WATCH_ACTION_WAKEUP     3

struct effective_watch_struct {
    struct testfs_inode_struct *inode;
    unsigned int mask;
    pthread_mutex_t lock_mutex;
    pthread_cond_t lock_condition;
    unsigned char lock;
    struct effective_watch_struct *next;
    struct effective_watch_struct *prev;
    unsigned char typebackend;
    void *backend;
    unsigned long id;
    unsigned long backend_id;
    unsigned char backendset;
    char *path;
};

// Prototypes

int lock_effective_watches();
int unlock_effective_watches();

unsigned long new_watchid();
struct effective_watch_struct *get_effective_watch();

void add_effective_watch_to_list(struct effective_watch_struct *effective_watch);
void remove_effective_watch_from_list(struct effective_watch_struct *effective_watch, unsigned char lockset);

void move_effective_watch_to_unused(struct effective_watch_struct *effective_watch);

struct effective_watch_struct *lookup_watch(unsigned char type, unsigned long id);
int calculate_effmask(struct effective_watch_struct *effective_watch, unsigned char lockset);
struct effective_watch_struct *get_next_effective_watch(struct effective_watch_struct *effective_watch);
int lock_effective_watch(struct effective_watch_struct *effective_watch);
int unlock_effective_watch(struct effective_watch_struct *effective_watch);

int check_for_effective_watch(char *path);

void set_backend(struct call_info_struct *call_info, struct effective_watch_struct *effective_watch);

void del_watch_at_backend(struct effective_watch_struct *effective_watch);
void set_watch_at_backend(struct effective_watch_struct *effective_watch, int newmask);

#endif
