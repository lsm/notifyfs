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

#ifndef LOCK_MONITOR_H
#define LOCK_MONITOR_H

struct lock_entry_struct {
    int major;
    int minor;
    unsigned long long ino;
    unsigned char mandatory;
    unsigned char kind;
    unsigned char type;
    pid_t pid;
    off_t start;
    off_t end;
    struct timespec detect_time;
    struct lock_entry_struct *next;
    struct lock_entry_struct *prev;
    struct lock_entry_struct *time_next;
    struct lock_entry_struct *time_prev;
    void *entry;
};

int open_locksfile();
void parse_changes_locks();

#endif
