/*
  2010, 2011 Stef Bon <stefbon@gmail.com>
  fuse-loop-epoll-mt.h
  This is an alternative main eventloop for the fuse filesystem using epoll and threads.

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

#ifndef FUSE_LOOP_EPOLL_MT_H
#define FUSE_LOOP_EPOLL_MT_H

// epoll parameters
#define MAX_EPOLL_NREVENTS 		32
#define MAX_EPOLL_NRFDS			32

// types of fd's
#define TYPE_FD_SIGNAL			1
#define TYPE_FD_FUSE			2
#define TYPE_FD_CUSTOMMIN		3
#define TYPE_FD_SOCKET                  4
#define TYPE_FD_CLIENT                  5
#define TYPE_FD_INOTIFY                 6
#define TYPE_FD_MOUNTINFO               7
#define TYPE_FD_CUSTOMMAX               8

// types of threads
#define TYPE_WORKER_PERMANENT		1
#define TYPE_WORKER_TEMPORARY		2

// number of threads
#ifndef NUM_WORKER_THREADS
#define NUM_WORKER_THREADS		10
#endif

#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include <semaphore.h>

// struct to identify the fd when epoll singals activity on that fd

struct epoll_extended_data_struct {
    int type_fd;
    int fd;
    void *data;
    int (*callback) (struct epoll_event *event);
    pthread_mutex_t read_mutex;
    struct epoll_event *event;
    struct epoll_extended_data_struct *next;
    struct epoll_extended_data_struct *prev;
};

// struct for the threads (permanent and temporary)

struct fuse_workerthread_struct {
    pthread_t threadid;
    pthread_mutex_t *read_mutex;
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

// manage fd's epoll listens to
int add_to_epoll(int fd, uint32_t events, unsigned char typefd, void *callback, void *data);
int remove_xdata_from_epoll(struct epoll_extended_data_struct *epoll_xdata, unsigned char lockset);
int remove_fd_from_epoll(int fd);
unsigned char scan_epoll_list(int fd);

// mainloop
int init_mainloop();
int fuse_session_loop_epoll_mt(struct fuse_session *se);

#endif
