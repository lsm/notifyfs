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

#ifndef FUSE_WORKSPACE_EPOLL_UTILS_H
#define FUSE_WORKSPACE_EPOLL_UTILS_H

// epoll parameters

#define MAX_EPOLL_NREVENTS 		32
#define MAX_EPOLL_NRFDS			32

// types of fd's

#define TYPE_FD_NOTSET			0
#define TYPE_FD_SIGNAL			1
#define TYPE_FD_TIMER               	2
#define TYPE_FD_FUSE			3
#define TYPE_FD_CUSTOMMIN		4
#define TYPE_FD_SOCKET                  5
#define TYPE_FD_CLIENT                  6
#define TYPE_FD_INOTIFY                 7
#define TYPE_FD_MOUNTINFO               8
#define TYPE_FD_USERS               	9
#define TYPE_FD_PIPE               	10

#define TYPE_FD_CUSTOMMAX               11

#define EVENTLOOP_OK			0
#define EVENTLOOP_EXIT			-1

#define EVENTLOOP_STATUS_NOTSET		0
#define EVENTLOOP_STATUS_SETUP		1
#define EVENTLOOP_STATUS_UP		2
#define EVENTLOOP_STATUS_DOWN		3

#define TIMERENTRY_TYPE_ABSOLUTE		1
#define TIMERENTRY_TYPE_RELATIVE_CURRENTTIME	2
#define TIMERENTRY_TYPE_RELATIVE_CURRENTENTRY	3

#define TIMERENTRY_STATUS_NOTSET		0
#define TIMERENTRY_STATUS_ACTIVE		1
#define TIMERENTRY_STATUS_INACTIVE		2

struct timerentry_struct {
    struct timespec expiretime;
    unsigned char status;
    unsigned long ctr;
    int (*eventcall) (void *data);
    unsigned char allocated;
    struct timerentry_struct *next;
    struct timerentry_struct *prev;
};

/* struct to identify the fd when epoll singals activity on that fd */

struct epoll_extended_data_struct {
    int type_fd;
    int fd;
    void *data;
    unsigned char allocated;
    int (*callback) (int fd, void *data, uint32_t events);
    struct epoll_extended_data_struct *next;
    struct epoll_extended_data_struct *prev;
};

/* mainloop data */

struct epoll_eventloop_struct {
    struct epoll_extended_data_struct *first;
    struct epoll_extended_data_struct *last;
    struct epoll_extended_data_struct *xdata_signal;
    struct epoll_extended_data_struct *xdata_timer;
    int nr;
    int epoll_fd;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned char exit;
    unsigned char status;
    struct timerentry_struct *firsttimer;
    struct timerentry_struct *nexttimer;
    struct timerentry_struct *lasttimer;
    pthread_mutex_t timersmutex;
};

#define EPOLL_XDATA_INIT {0, 0, NULL, 0, NULL, NULL, NULL}
#define EPOLL_EVENTLOOP_INIT {NULL, NULL, NULL, NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER}

/* Prototypes */

int lock_xdata_list(struct epoll_eventloop_struct *epoll_eventloop);
int unlock_xdata_list(struct epoll_eventloop_struct *epoll_eventloop);
struct epoll_extended_data_struct *get_next_xdata(struct epoll_extended_data_struct *epoll_xdata, struct epoll_eventloop_struct *epoll_eventloop);

struct epoll_extended_data_struct *add_to_epoll(int fd, uint32_t events, unsigned char typefd, void *callback, void *data, struct epoll_extended_data_struct *epoll_xdata, struct epoll_eventloop_struct *epoll_eventloop);
int remove_xdata_from_epoll(struct epoll_extended_data_struct *epoll_xdata, struct epoll_eventloop_struct *epoll_eventloop);
int remove_fd_from_epoll(int fd, unsigned char lockset, struct epoll_eventloop_struct *epoll_eventloop_list);

int modify_xdata(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, struct epoll_eventloop_struct *epoll_eventloop);
void add_xdata_to_list(struct epoll_extended_data_struct *epoll_xdata, struct epoll_eventloop_struct *epoll_eventloop);
void remove_xdata_from_list(struct epoll_extended_data_struct *epoll_xdata, unsigned char lockset, struct epoll_eventloop_struct *epoll_eventloop);

struct epoll_extended_data_struct *scan_eventloop(int fd, unsigned char lockset, struct epoll_eventloop_struct *epoll_eventloop);

int insert_timerentry(struct timerentry_struct *new_timerentry, struct epoll_eventloop_struct *epoll_eventloop);
void remove_timerentry(struct timerentry_struct *timerentry, struct epoll_eventloop_struct *epoll_eventloop);
void init_timerentry(struct timerentry_struct *timerentry, unsigned char allocated, struct timespec *expiretime);
void set_timerentry(struct timerentry_struct *timerentry, unsigned char type, struct timespec *expiretime);
struct timerentry_struct *create_timerentry(struct timespec *expiretime);

/* mainloop */

int init_eventloop(struct epoll_eventloop_struct *epoll_eventloop, unsigned char addsignal, unsigned char addtimer);
int start_epoll_eventloop(struct epoll_eventloop_struct *epoll_eventloop);
void destroy_eventloop(struct epoll_eventloop_struct *epoll_eventloop);

#endif
