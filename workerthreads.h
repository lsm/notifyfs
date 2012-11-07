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

#ifndef NOTIFYFS_WORKERTHREADS_H
#define NOTIFYFS_WORKERTHREADS_H

struct workerthread_struct {
    pthread_t threadid;
    unsigned char work;
    int nr;
    void (* processevent_cb) (void *data);
    void *data;
    struct workerthread_struct *next;
};

// Prototypes

struct workerthread_struct *get_thread_from_queue(unsigned char removethread);
void put_thread_to_queue(struct workerthread_struct *workerthread, unsigned char newthread);

void signal_workerthread(struct workerthread_struct *workerthread);

int add_workerthreads(unsigned number);
void remove_workerthreads(unsigned number);
void start_workerthreads();

void remove_all_workerthreads();

#endif
