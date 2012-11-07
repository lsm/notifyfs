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

#define _REENTRANT
#define _GNU_SOURCE

#define PACKAGE_VERSION "1.4"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>

#include <inttypes.h>
#include <ctype.h>
#include <sys/types.h>

#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <pthread.h>
#include <syslog.h>

#include "global-defines.h"

#define LOG_LOGAREA LOG_LOGAREA_MAINLOOP

#define LOGGING

#include "logging.h"
#include "epoll-utils.h"
#include "utils.h"

unsigned long timerctr=0;

struct epoll_eventloop_struct epoll_eventloop_main=EPOLL_EVENTLOOP_INIT;

static int set_timer(struct epoll_eventloop_struct *epoll_eventloop)
{
    struct timerentry_struct *timerentry;
    struct itimerspec new_value;
    int fd, res;

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    timerentry=epoll_eventloop->firsttimer;

    /* there is no need to check the existence of the fd, it has been checked before at add_timerentry */

    fd=epoll_eventloop->xdata_timer->fd;

    if (timerentry) {

	new_value.it_value.tv_sec=timerentry->expiretime.tv_sec;
	new_value.it_value.tv_nsec=timerentry->expiretime.tv_nsec;

    } else {

	new_value.it_value.tv_sec=0;
	new_value.it_value.tv_nsec=0;

    }

    new_value.it_interval.tv_sec=0;
    new_value.it_interval.tv_nsec=0;

    logoutput("set_timer: set at %li.%li on fd %i", new_value.it_value.tv_sec, new_value.it_value.tv_nsec, fd);

    /* (re) set the timer
    note:
    - the expired time is in absolute format (required to compare timerentries with each other )
    - when the timerentry is empty, then this still does what it should do: 
      in that case it disarms the timer */

    res=timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL);

    if (res==-1) {

	logoutput("set_timer: error %i setting timer", errno);
	if (timerentry) timerentry->status=TIMERENTRY_STATUS_INACTIVE;

    } else {

	if (timerentry) timerentry->status=TIMERENTRY_STATUS_ACTIVE;

    }

    return res;

}

static void disable_timer(struct epoll_eventloop_struct *epoll_eventloop)
{
    struct itimerspec new_value;
    int fd;

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    /* there is no need to check the existence of the fd, it has been checked before at add_timerentry */

    fd=epoll_eventloop->xdata_timer->fd;

    new_value.it_value.tv_sec=0;
    new_value.it_value.tv_nsec=0;

    new_value.it_interval.tv_sec=0;
    new_value.it_interval.tv_nsec=0;

    timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL);

}

void init_timerentry(struct timerentry_struct *timerentry, unsigned char allocated, struct timespec *expiretime)
{

    timerentry->status=TIMERENTRY_STATUS_NOTSET;
    timerentry->ctr=0;
    timerentry->allocated=allocated;

    if (expiretime) {

	timerentry->expiretime.tv_sec=expiretime->tv_sec;
	timerentry->expiretime.tv_nsec=expiretime->tv_nsec;

    } else {

	timerentry->expiretime.tv_sec=0;
	timerentry->expiretime.tv_nsec=0;

    }

    timerentry->eventcall=NULL;
    timerentry->next=NULL;
    timerentry->prev=NULL;

}


struct timerentry_struct *create_timerentry(struct timespec *expiretime)
{
    struct timerentry_struct *timerentry=NULL;

    timerentry=malloc(sizeof(struct timerentry_struct));
    if (timerentry) init_timerentry(timerentry, 1, expiretime);

    return timerentry;

}

int insert_timerentry(struct timerentry_struct *new_timerentry, struct epoll_eventloop_struct *epoll_eventloop)
{
    struct timerentry_struct *timerentry=NULL;
    int res, nreturn=0;
    unsigned char firstchanged=0;

    if (new_timerentry) {

	logoutput("insert_timerentry: insert %li.%li", new_timerentry->expiretime.tv_sec, new_timerentry->expiretime.tv_nsec);

    } else {

	logoutput("insert_timerentry: entry empty");
	return 0;

    }

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    pthread_mutex_lock(&epoll_eventloop->timersmutex);

    timerentry=epoll_eventloop->firsttimer;

    if (timerentry) {

        while(timerentry) {

	    if ( timerentry!=new_timerentry ) {

		logoutput("insert_timerentry: compare %li with %li", timerentry->expiretime.tv_sec, new_timerentry->expiretime.tv_sec);

		res=is_later(&timerentry->expiretime, &new_timerentry->expiretime, 0, 0);

		if (res>0) {

		    break;

		}

	    }

	    timerentry=timerentry->next;

	}

    }

    if (timerentry) {

	/* insert before an existing entry */

	new_timerentry->next=timerentry;
	new_timerentry->prev=timerentry->prev;

	if (timerentry->prev) timerentry->prev->next=new_timerentry;
	timerentry->prev=new_timerentry;

	if (epoll_eventloop->firsttimer==timerentry) epoll_eventloop->firsttimer=new_timerentry;
	firstchanged=1;

    } else {

	if (epoll_eventloop->lasttimer) {

	    epoll_eventloop->lasttimer->next=new_timerentry;
	    new_timerentry->prev=epoll_eventloop->lasttimer;

	}

	epoll_eventloop->lasttimer=new_timerentry;

	if ( ! epoll_eventloop->firsttimer) {

	    epoll_eventloop->firsttimer=new_timerentry;
	    firstchanged=1;

	}

    }

    if (firstchanged==1) {

	logoutput("insert_timerentry: first is changed: reset timer ");

	set_timer(epoll_eventloop);

    } else {

	logoutput("insert_timerentry: no reset timer required");

    }

    pthread_mutex_unlock(&epoll_eventloop->timersmutex);

    return nreturn;

}

void remove_timerentry_from_list(struct timerentry_struct *timerentry, struct epoll_eventloop_struct *epoll_eventloop)
{
    logoutput("remove_timerentry_from_list");

    if (timerentry->next) timerentry->next->prev=timerentry->prev;
    if (timerentry->prev) timerentry->prev->next=timerentry->next;

    if (epoll_eventloop->lasttimer==timerentry) epoll_eventloop->lasttimer=timerentry->prev;
    if (epoll_eventloop->firsttimer==timerentry) epoll_eventloop->firsttimer=timerentry->next;

}

void set_timerentry(struct timerentry_struct *timerentry, unsigned char type, struct timespec *expiretime)
{

    logoutput("set_timerentry");

    if (type==TIMERENTRY_TYPE_ABSOLUTE) {
	struct timespec rightnow;

	/* absolute: it must be later than now */

	get_current_time(&rightnow);

	if (is_later(expiretime, &rightnow, 0, 0)==1) {

	    timerentry->expiretime.tv_sec=expiretime->tv_sec;
	    timerentry->expiretime.tv_nsec=expiretime->tv_nsec;

	} else {

	    timerentry->expiretime.tv_sec=0;
	    timerentry->expiretime.tv_nsec=0;

	}

    } else if (type==TIMERENTRY_TYPE_RELATIVE_CURRENTTIME) {
	struct timespec rightnow;
	int extra=0;

	/* relative: add to current time*/

	get_current_time(&rightnow);

	timerentry->expiretime.tv_nsec=rightnow.tv_nsec+expiretime->tv_nsec;

	if (timerentry->expiretime.tv_nsec>1000000000) {

	    timerentry->expiretime.tv_nsec-=1000000000;
	    extra=1;

	} else if (timerentry->expiretime.tv_nsec<0) {

	    timerentry->expiretime.tv_nsec+=1000000000;
	    extra=-1;

	}

	timerentry->expiretime.tv_sec=rightnow.tv_sec+expiretime->tv_sec+extra;

    } else if (type==TIMERENTRY_TYPE_RELATIVE_CURRENTENTRY) {
	int extra=0;

	/* relative: add to current entry*/

	timerentry->expiretime.tv_nsec+=expiretime->tv_nsec;

	if (timerentry->expiretime.tv_nsec>1000000000) {

	    timerentry->expiretime.tv_nsec-=1000000000;
	    extra=1;

	} else if (timerentry->expiretime.tv_nsec<0) {

	    timerentry->expiretime.tv_nsec+=1000000000;
	    extra=-1;

	}

	timerentry->expiretime.tv_sec+=expiretime->tv_sec+extra;

    }

}

static void run_expired_timerentry(void *data, struct epoll_eventloop_struct *epoll_eventloop)
{

    logoutput("run_expired_timerentry");

    disable_timer(epoll_eventloop);

    while(1) {
	struct timespec rightnow;
	struct timerentry_struct *timerentry;
	unsigned char removeit=0;

	get_current_time(&rightnow);

	pthread_mutex_lock(&epoll_eventloop->timersmutex);

	timerentry=epoll_eventloop->firsttimer;
	removeit=0;

	if (timerentry) {

	    /* compare the expiretime with now */

	    if (timerentry->status==TIMERENTRY_STATUS_INACTIVE || is_later(&rightnow, &timerentry->expiretime, 0, 0)>=0) {

		/* remove it from the list, so it's possible to run it when lock is released */

		remove_timerentry_from_list(timerentry, epoll_eventloop);

		removeit=1;

	    }

	}

	logoutput("run_expired_timerentry: A");

	pthread_mutex_unlock(&epoll_eventloop->timersmutex);

	if (removeit==1) {

	    if (timerentry->status==TIMERENTRY_STATUS_ACTIVE) {

		if (timerentry->eventcall) {

		    logoutput("run_expired_timerentry: B : run eventcall");

		    /* run it */

		    int res=(* timerentry->eventcall) (data);

		}

	    }

	    free(timerentry);
	    timerentry=NULL;

	} else {

	    break;

	}

    }

    logoutput("run_expired_timerentry: B: exit");

    set_timer(epoll_eventloop);

}

void remove_timerentry(struct timerentry_struct *timerentry, struct epoll_eventloop_struct *epoll_eventloop)
{
    unsigned char firstchanged=0;

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    if ( ! epoll_eventloop->xdata_timer) {

	logoutput("remove_timerentry: error: xdata_timer not set");

	return;

    } else {

	if ( epoll_eventloop->xdata_timer->fd<=0 ) {

	    logoutput("remove_timerentry: error: timer fd not set");
	    return;

	}

    }

    pthread_mutex_lock(&epoll_eventloop->timersmutex);

    if (timerentry==epoll_eventloop->firsttimer) firstchanged=1;

    remove_timerentry_from_list(timerentry, epoll_eventloop);

    if (timerentry->allocated==1) {

	free(timerentry);

    } else {

	timerentry->status=TIMERENTRY_STATUS_INACTIVE;

    }

    if (firstchanged==1) set_timer(epoll_eventloop);

    pthread_mutex_unlock(&epoll_eventloop->timersmutex);

}

int lock_eventloop(struct epoll_eventloop_struct *epoll_eventloop)
{
    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    return pthread_mutex_lock(&epoll_eventloop->mutex);
}

int unlock_eventloop(struct epoll_eventloop_struct *epoll_eventloop)
{
    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    return pthread_mutex_unlock(&epoll_eventloop->mutex);
}

void add_xdata_to_list(struct epoll_extended_data_struct *epoll_xdata, struct epoll_eventloop_struct *epoll_eventloop)
{

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    pthread_mutex_lock(&epoll_eventloop->mutex);

    /* add at tail */

    epoll_xdata->prev=NULL;
    epoll_xdata->next=NULL;

    if ( ! epoll_eventloop->first ) epoll_eventloop->first=epoll_xdata;

    if ( epoll_eventloop->last ) {

	epoll_eventloop->last->next=epoll_xdata;
	epoll_xdata->prev=epoll_eventloop->last;

    }

    epoll_eventloop->last=epoll_xdata;

    epoll_eventloop->nr++;

    pthread_mutex_unlock(&epoll_eventloop->mutex);

}

void remove_xdata_from_list(struct epoll_extended_data_struct *epoll_xdata, unsigned char lockset, struct epoll_eventloop_struct *epoll_eventloop)
{
    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    if (lockset==0) pthread_mutex_lock(&epoll_eventloop->mutex);

    if ( epoll_eventloop->first==epoll_xdata ) epoll_eventloop->first=epoll_xdata->next;
    if ( epoll_eventloop->last==epoll_xdata ) epoll_eventloop->last=epoll_xdata->prev;

    if ( epoll_xdata->prev ) epoll_xdata->prev->next=epoll_xdata->next;
    if ( epoll_xdata->next ) epoll_xdata->next->prev=epoll_xdata->prev;

    epoll_eventloop->nr--;

    epoll_xdata->prev=NULL;
    epoll_xdata->next=NULL;

    if (lockset==0) pthread_mutex_unlock(&epoll_eventloop->mutex);

}

struct epoll_extended_data_struct *add_to_epoll(int fd, uint32_t events, unsigned char typefd, void *callback, void *data, struct epoll_extended_data_struct *epoll_xdata, struct epoll_eventloop_struct *epoll_eventloop)
{
    struct epoll_event e_event;
    int nreturn=0, res;
    unsigned char created=0;

    logoutput("add_to_epoll: add fd %i",fd);

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    if ( typefd==TYPE_FD_SIGNAL ) {

	if ( epoll_eventloop->xdata_signal ) {

	    logoutput("add_to_epoll: already a signal handler added to eventloop");
	    return NULL;

	}

    }

    if ( ! epoll_xdata ) {

	epoll_xdata=malloc(sizeof(struct epoll_extended_data_struct));

	if (epoll_xdata) epoll_xdata->allocated=1;

    }

    if ( epoll_xdata) {

    	epoll_xdata->fd=fd;
    	epoll_xdata->type_fd=typefd;
    	epoll_xdata->data=data;
    	epoll_xdata->callback=callback;

    } else {

    	goto out;

    }

    e_event.events=events;
    e_event.data.ptr=(void *) epoll_xdata;

    // add this fd to the epoll instance

    res=epoll_ctl(epoll_eventloop->epoll_fd, EPOLL_CTL_ADD, fd, &e_event);

    if ( res==-1 ) {

        if ( epoll_xdata->allocated==1 ) free(epoll_xdata);
        epoll_xdata=NULL;

    } else {

	if ( typefd==TYPE_FD_SIGNAL ) {

	    epoll_eventloop->xdata_signal=epoll_xdata;

	}

    }

    out:

    if ( ! epoll_xdata ) {

	logoutput("add_to_epoll: unable to create/add epoll_xdata");

    }

    return epoll_xdata;

}

int remove_xdata_from_epoll(struct epoll_extended_data_struct *epoll_xdata, struct epoll_eventloop_struct *epoll_eventloop)
{
    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    if ( epoll_eventloop->epoll_fd>0 ) return epoll_ctl(epoll_eventloop->epoll_fd, EPOLL_CTL_DEL, epoll_xdata->fd, NULL);

    return 0;
}

int modify_xdata(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, struct epoll_eventloop_struct *epoll_eventloop)
{

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    if ( epoll_eventloop->epoll_fd>0 ) {
	struct epoll_event e_event;

	e_event.events=events;
	e_event.data.ptr=(void *) epoll_xdata;

	return epoll_ctl(epoll_eventloop->epoll_fd, EPOLL_CTL_MOD, epoll_xdata->fd, &e_event);

    }

    return 0;
}


int remove_fd_from_epoll(int fd, unsigned char lockset, struct epoll_eventloop_struct *epoll_eventloop)
{
    int nreturn=0;
    struct epoll_extended_data_struct *epoll_xdata=NULL;

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    if ( lockset==0 ) pthread_mutex_lock(&epoll_eventloop->mutex);

    epoll_xdata=epoll_eventloop->first;

    while (epoll_xdata) {

        if ( epoll_xdata->fd==fd ) break;

        epoll_xdata=epoll_xdata->next;

    }

    if ( epoll_xdata ) {

        nreturn=remove_xdata_from_epoll(epoll_xdata, epoll_eventloop);

    } else {

        logoutput("fd %i not found on epoll xdata list ", fd);

    }

    if ( lockset==0 ) pthread_mutex_unlock(&epoll_eventloop->mutex);

    return nreturn;

}

struct epoll_extended_data_struct *get_next_xdata(struct epoll_extended_data_struct *epoll_xdata, struct epoll_eventloop_struct *epoll_eventloop)
{
    if ( epoll_xdata ) return epoll_xdata->next;
    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;
    return epoll_eventloop->first;

}

struct epoll_extended_data_struct *scan_eventloop(int fd, unsigned char lockset, struct epoll_eventloop_struct *epoll_eventloop)
{
    struct epoll_extended_data_struct *epoll_xdata=NULL;

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    if (lockset==0) pthread_mutex_lock(&epoll_eventloop->mutex);

    epoll_xdata=epoll_eventloop->first;

    while (epoll_xdata) {

        if ( epoll_xdata->fd==fd ) break;
        epoll_xdata=epoll_xdata->next;

    }

    if (lockset==0) pthread_mutex_unlock(&epoll_eventloop->mutex);

    return epoll_xdata;

}

int init_eventloop(struct epoll_eventloop_struct *epoll_eventloop, unsigned char addsignal, unsigned char addtimer)
{
    int nreturn=0;

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    /* create an epoll instance */

    epoll_eventloop->epoll_fd=epoll_create(MAX_EPOLL_NRFDS);

    if ( epoll_eventloop->epoll_fd==-1 ) {

	nreturn=-errno;
	goto out;

    }

    epoll_eventloop->exit=0;

    pthread_mutex_init(&epoll_eventloop->mutex, NULL);
    pthread_cond_init(&epoll_eventloop->cond, NULL);
    epoll_eventloop->status=EVENTLOOP_STATUS_SETUP;

    if (addsignal==1) {
	sigset_t mainloop_sigset;
	struct epoll_extended_data_struct *epoll_xdata;
	int signal_fd;

	/*
    	    SIGNALS: set the set of signals for signalfd to listen to
	*/

	sigemptyset(&mainloop_sigset);

	sigaddset(&mainloop_sigset, SIGINT);
	sigaddset(&mainloop_sigset, SIGIO);
	sigaddset(&mainloop_sigset, SIGHUP);
	sigaddset(&mainloop_sigset, SIGTERM);
	sigaddset(&mainloop_sigset, SIGPIPE);
	sigaddset(&mainloop_sigset, SIGCHLD);
	sigaddset(&mainloop_sigset, SIGUSR1);

	if (sigprocmask(SIG_BLOCK, &mainloop_sigset, NULL) == -1) {

	    nreturn=-errno;
    	    logoutput("init_eventloop: error sigprocmask");
    	    goto out;

	}

	signal_fd = signalfd(-1, &mainloop_sigset, SFD_NONBLOCK);
	// signal_fd = signalfd(-1, &mainloop_sigset, 0);

	if (signal_fd == -1) {

    	    nreturn=-errno;
    	    logoutput("init_eventloop: unable to create signalfd, error: %i", nreturn);
    	    goto out;

	}

	logoutput("init_eventloop: adding signalfd %i to epoll", signal_fd);

	epoll_xdata=add_to_epoll(signal_fd, EPOLLIN, TYPE_FD_SIGNAL, NULL, NULL, NULL, epoll_eventloop);

	if ( !epoll_xdata ) {

	    nreturn=-EIO;
	    goto out;

	}

	epoll_eventloop->xdata_signal=epoll_xdata;

	add_xdata_to_list(epoll_xdata, epoll_eventloop);

    }

    if (addtimer==1) {
	struct epoll_extended_data_struct *epoll_xdata;
	int timer_fd;

	timer_fd=timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);

	if (timer_fd == -1) {

    	    nreturn=-errno;
    	    logoutput("init_eventloop: unable to create timerfd, error: %i", nreturn);
    	    goto out;

	}

	logoutput("init_eventloop: adding timerfd %i to epoll", timer_fd);

	epoll_xdata=add_to_epoll(timer_fd, EPOLLIN, TYPE_FD_TIMER, NULL, NULL, NULL, epoll_eventloop);

	if ( !epoll_xdata ) {

	    nreturn=-EIO;
	    goto out;

	}

	epoll_eventloop->xdata_timer=epoll_xdata;

	add_xdata_to_list(epoll_xdata, epoll_eventloop);

    }

    out:

    return nreturn;

}

int start_epoll_eventloop(struct epoll_eventloop_struct *epoll_eventloop)
{
    struct epoll_event epoll_events[MAX_EPOLL_NREVENTS];
    int i, res;
    ssize_t readlen;
    struct signalfd_siginfo fdsi;
    int signo, nreturn, nerror;
    struct epoll_extended_data_struct *epoll_xdata;

    logoutput("eventloop: starting epoll wait loop");

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    /* send signal to waiting threads to start creating objects in fs */

    pthread_mutex_lock(&epoll_eventloop->mutex);
    epoll_eventloop->status=EVENTLOOP_STATUS_UP;
    pthread_cond_broadcast(&epoll_eventloop->cond);
    pthread_mutex_unlock(&epoll_eventloop->mutex);

    while (epoll_eventloop->exit==0) {

        int number_of_fds=epoll_wait(epoll_eventloop->epoll_fd, epoll_events, MAX_EPOLL_NREVENTS, -1);

        if (number_of_fds < 0) {

            epoll_eventloop->exit=1;

            logoutput("eventloop: epoll_wait error %i", errno);

            break; /* good way to handle this ??*/

        } else {

            logoutput("mainloop: number of fd's: %i", number_of_fds);

        }


        for (i=0; i<number_of_fds; i++) {


            epoll_xdata=(struct epoll_extended_data_struct *) epoll_events[i].data.ptr;

            /* look what kind of fd this is and launch the custom callback */

	    if ( epoll_xdata->type_fd==TYPE_FD_SIGNAL ) {

                /* some data on signalfd */

                readlen=read(epoll_xdata->fd, &fdsi, sizeof(struct signalfd_siginfo));

                if ( readlen==-1 ) {

                    nerror=errno;

                    if ( nerror==EAGAIN ) {

                        // blocking error: back to the mainloop

                        continue;

                    }

                    logoutput("error %i reading from signalfd......", nerror);

                } else {

                    if ( readlen == sizeof(struct signalfd_siginfo)) {

                        // a signal read: check the signal

                        signo=fdsi.ssi_signo;

                        if ( signo==SIGHUP || signo==SIGINT || signo==SIGTERM ) {

                            logoutput("mainloop: caught signal %i, exit session", signo);

			    epoll_eventloop->exit=1;

                            goto out;

                        } else if ( signo==SIGPIPE ) {

                            logoutput("mainloop: caught signal SIGPIPE, ignoring");

                        } else if ( signo == SIGCHLD) {

                            logoutput("Got SIGCHLD, from pid: %d", fdsi.ssi_pid);

                        } else if ( signo == SIGIO) {

                            logoutput("Got SIGIO.....");

                        } else if ( signo == SIGUSR1) {

                            logoutput("Got SIGUSR1....");


                        } else {

                            logoutput("got unknown signal %i", signo);

                        }

                    } else {

                        logoutput("eventloop: read %i, not %i", readlen, sizeof(struct signalfd_siginfo) );

		    }

                }

	    } else if ( epoll_xdata->type_fd==TYPE_FD_TIMER ) {
		uint64_t expirations;

		res=read(epoll_xdata->fd, &expirations, sizeof(uint64_t));

		if (res>0) {

		    logoutput("eventloop: got a timer event, expirations %li", (long int) expirations);

		    run_expired_timerentry(epoll_xdata->data, epoll_eventloop);

		} else {

		    logoutput("eventloop: error %i reading timerfd", errno);

		}

            } else if ( epoll_xdata->type_fd!=TYPE_FD_NOTSET ) {

                /* process the custom fd */

                /* call the custom function which is defined in epoll_xdata->data
                   with argument epoll_events[i] */

                if ( epoll_xdata->callback ) {

                    /* function defined */

                    res=(*epoll_xdata->callback) (epoll_xdata->fd, epoll_xdata->data, epoll_events[i].events);

		    if ( res==EVENTLOOP_EXIT ) {

			logoutput("eventloop: remove %i from eventloop", epoll_xdata->fd);

			remove_xdata_from_epoll(epoll_xdata, epoll_eventloop);
			remove_xdata_from_list(epoll_xdata, 0, epoll_eventloop);

			if (epoll_eventloop->nr==0) {

			    logoutput("eventloop %i: no more fd's", epoll_eventloop->epoll_fd);

			    /* no more fd's... */
			    epoll_eventloop->exit=1;
			    break;

			} else {

			    logoutput("eventloop %i: remaining number of fd's %i", epoll_eventloop->epoll_fd, epoll_eventloop->nr);

			}

		    }

                } else {

                    logoutput("eventloop: error: callback not defined for %i", epoll_xdata->fd);

                }


            } else {

                // should not happen

        	logoutput("eventloop: error: fd: %i / type %i not reckognized", epoll_xdata->fd, epoll_xdata->type_fd);

            }

        }

    }

    pthread_mutex_lock(&epoll_eventloop->mutex);
    epoll_eventloop->status=EVENTLOOP_STATUS_DOWN;
    pthread_cond_broadcast(&epoll_eventloop->cond);
    pthread_mutex_unlock(&epoll_eventloop->mutex);

    out:

    logoutput("eventloop: exit");

    return nreturn < 0 ? -1 : 0;

}

void destroy_eventloop(struct epoll_eventloop_struct *epoll_eventloop)
{
    struct epoll_extended_data_struct *epoll_xdata=NULL;
    int res;

    logoutput("destroy_eventloop");

    if ( ! epoll_eventloop) epoll_eventloop=&epoll_eventloop_main;

    /* close any remaining fd previously added to the mainloop */

    pthread_mutex_lock(&epoll_eventloop->mutex);

    epoll_xdata=epoll_eventloop->first;

    while (epoll_xdata) {

        epoll_eventloop->first=epoll_xdata->next;
        epoll_xdata->next=NULL;
        epoll_xdata->prev=NULL;

        if ( epoll_eventloop->epoll_fd>0 ) res=epoll_ctl(epoll_eventloop->epoll_fd, EPOLL_CTL_DEL, epoll_xdata->fd, NULL);

	logoutput("destroy_eventloop: close fd: %i", epoll_xdata->fd);

        close(epoll_xdata->fd);
	if (epoll_xdata->allocated==1) free(epoll_xdata);

	epoll_eventloop->nr--;

        epoll_xdata=epoll_eventloop->first;

    }

    close(epoll_eventloop->epoll_fd);
    epoll_eventloop->epoll_fd=0;

    pthread_mutex_lock(&epoll_eventloop->timersmutex);

    if (epoll_eventloop->firsttimer) {
	struct timerentry_struct *timerentry=epoll_eventloop->firsttimer;

	while(timerentry) {

	    epoll_eventloop->firsttimer=timerentry->next;

	    remove_timerentry_from_list(timerentry, epoll_eventloop);

	    if (timerentry->allocated) free(timerentry);

	    timerentry=epoll_eventloop->firsttimer;

	}
    }

    pthread_mutex_unlock(&epoll_eventloop->timersmutex);
    pthread_mutex_destroy(&epoll_eventloop->timersmutex);

    pthread_mutex_unlock(&epoll_eventloop->mutex);
    pthread_mutex_destroy(&epoll_eventloop->mutex);
    pthread_cond_destroy(&epoll_eventloop->cond);

}
