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

#ifndef FUSE_HANDLEEVENTS_H
#define FUSE_HANDLEEVENTS_H

// types of threads
#define TYPE_WORKER_PERMANENT		1
#define TYPE_WORKER_TEMPORARY		2

// number of threads
#ifndef NUM_WORKER_THREADS
#define NUM_WORKER_THREADS		10
#endif

// struct for the threads (permanent and temporary)

struct fuse_workerthread_struct {
    pthread_t threadid;
    unsigned char busy;
    int nr;
    unsigned char typeworker;
    struct fuse_buf fbuf;
    struct fuse_chan *ch;
    struct fuse_session *se;
    struct fuse_workerthread_struct *next;
    struct fuse_workerthread_struct *prev;
};

// Prototypes


int addfusechannelstomainloop(struct fuse_session *se, const char *mountpoint);
int startfusethreads();

int process_fuse_event(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo);

void terminatefuse();
void stopfusethreads();

#endif
