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
#include <assert.h>

#include <inttypes.h>
#include <ctype.h>
#include <sys/types.h>

#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include <semaphore.h>
#include <syslog.h>

#include "global-defines.h"

#define LOG_LOGAREA LOG_LOGAREA_MAINLOOP

#define LOGGING

#include "logging.h"
#include "epoll-utils.h"

struct epoll_xdata_list_struct {
	struct epoll_extended_data_struct *first;
	struct epoll_extended_data_struct *last;
	int nr;
	pthread_mutex_t mutex;
};

int epoll_fd=0;
unsigned char exitmainloop=0;
int exitmainloop_error=0;

struct epoll_xdata_list_struct epoll_xdata_list= {NULL, NULL, 0, PTHREAD_MUTEX_INITIALIZER};

void add_xdata_to_list(struct epoll_extended_data_struct *epoll_xdata)
{

    pthread_mutex_lock(&epoll_xdata_list.mutex);

    /* add at tail */

    epoll_xdata->prev=NULL;
    epoll_xdata->next=NULL;

    if ( ! epoll_xdata_list.first ) epoll_xdata_list.first=epoll_xdata;

    if ( epoll_xdata_list.last ) {

	epoll_xdata_list.last->next=epoll_xdata;
	epoll_xdata->prev=epoll_xdata_list.last;

    }

    epoll_xdata_list.last=epoll_xdata;

    epoll_xdata_list.nr++;

    pthread_mutex_unlock(&epoll_xdata_list.mutex);

}

void remove_xdata_from_list(struct epoll_extended_data_struct *epoll_xdata)
{

    pthread_mutex_lock(&epoll_xdata_list.mutex);

    if ( epoll_xdata_list.first==epoll_xdata ) epoll_xdata_list.first=epoll_xdata->next;
    if ( epoll_xdata_list.last==epoll_xdata ) epoll_xdata_list.last=epoll_xdata->prev;

    if ( epoll_xdata->prev ) epoll_xdata->prev->next=epoll_xdata->next;
    if ( epoll_xdata->next ) epoll_xdata->next->prev=epoll_xdata->prev;

    epoll_xdata_list.nr--;

    epoll_xdata->prev=NULL;
    epoll_xdata->next=NULL;

    pthread_mutex_unlock(&epoll_xdata_list.mutex);

}

struct epoll_extended_data_struct *add_to_epoll(int fd, uint32_t events, unsigned char typefd, void *callback, void *data, struct epoll_extended_data_struct *epoll_xdata)
{
    struct epoll_event e_event;
    int nreturn=0, res;
    unsigned char created=0;

    writelog(1, "add_to_epoll: add fd %i",fd);

    if ( ! epoll_xdata ) {

	epoll_xdata=malloc(sizeof(struct epoll_extended_data_struct));

    }

    if ( epoll_xdata) {

    	epoll_xdata->fd=fd;
    	epoll_xdata->type_fd=typefd;

    	epoll_xdata->data=data;
    	epoll_xdata->callback=callback;

	created=1;

    } else {

    	goto out;

    }


    e_event.events=events;
    e_event.data.ptr=(void *) epoll_xdata;

    // add this fd to the epoll instance

    res=epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &e_event);

    if ( res==-1 ) {

        if ( created ) free(epoll_xdata);
        epoll_xdata=NULL;

    }

    out:

    if ( ! epoll_xdata ) {

	writelog(2, "add_to_epoll: unable to create/add epoll_xdata");

    }

    return epoll_xdata;

}

int remove_xdata_from_epoll(struct epoll_extended_data_struct *epoll_xdata, unsigned char lockset)
{

    writelog(1,  "remove_xdata_from_epoll: remove fd %i",epoll_xdata->fd);

    if ( epoll_fd>0 ) {
	return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, epoll_xdata->fd, NULL);

    }

    return 0;
}

int remove_fd_from_epoll(int fd, unsigned char lockset)
{
    int nreturn=0;
    struct epoll_extended_data_struct *epoll_xdata=NULL;

    if ( lockset==0 ) pthread_mutex_lock(&epoll_xdata_list.mutex);

    epoll_xdata=epoll_xdata_list.first;

    while (epoll_xdata) {

        if ( epoll_xdata->fd==fd ) break;

        epoll_xdata=epoll_xdata->next;

    }

    if ( epoll_xdata ) {

        nreturn=remove_xdata_from_epoll(epoll_xdata, 1);

    } else {

        writelog(2, "fd %i not found on epoll xdata list ", fd);

    }

    if ( lockset==0 ) pthread_mutex_unlock(&epoll_xdata_list.mutex);

    return nreturn;

}

struct epoll_extended_data_struct *get_next_epoll_xdata(struct epoll_extended_data_struct *epoll_xdata)
{
    if ( epoll_xdata ) return epoll_xdata->next;

    return epoll_xdata_list.first;

}

//
// function to scan the current list of epoll_xdata a specific fd is already on list
//

unsigned char scan_epoll_list(int fd)
{
    struct epoll_extended_data_struct *epoll_xdata=NULL;

    pthread_mutex_lock(&epoll_xdata_list.mutex);

    epoll_xdata=epoll_xdata_list.first;

    while (epoll_xdata) {

        if ( epoll_xdata->fd==fd ) break;
        epoll_xdata=epoll_xdata->next;

    }

    pthread_mutex_unlock(&epoll_xdata_list.mutex);

    return (epoll_xdata) ? 1 : 0;

}

int init_mainloop()
{

    /* create an epoll instance */

    epoll_fd=epoll_create(MAX_EPOLL_NRFDS);

    if ( epoll_fd==-1 ) epoll_fd=-errno;

    return epoll_fd;

}

void setmainloop_exit(int nerror)
{
    exitmainloop=1;
    exitmainloop_error=nerror;

}

void send_callbacks_signal(int signo)
{
    struct epoll_extended_data_struct *epoll_xdata, *next_xdata;
    int res;

    logoutput("send_callbacks_signal");

    pthread_mutex_lock(&epoll_xdata_list.mutex);

    epoll_xdata=epoll_xdata_list.first;

    while(epoll_xdata) {

	next_xdata=epoll_xdata->next;

        if ( epoll_xdata->callback ) res=(*epoll_xdata->callback) (epoll_xdata, 0, signo);

	epoll_xdata=next_xdata;

    }

    pthread_mutex_unlock(&epoll_xdata_list.mutex);

}

int epoll_mainloop()
{
    int signal_fd;
    struct epoll_event epoll_events[MAX_EPOLL_NREVENTS];
    int i, res;
    ssize_t readlen;
    struct signalfd_siginfo fdsi;
    int signo, nreturn, nerror;
    sigset_t mainloop_sigset;
    struct epoll_extended_data_struct *epoll_xdata, xdata_signal;

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

    signal_fd = signalfd(-1, &mainloop_sigset, SFD_NONBLOCK);
    // signal_fd = signalfd(-1, &mainloop_sigset, 0);

    if (signal_fd == -1) {

        nreturn=-errno;
        writelog(1, "mainloop: unable to create signalfd, error: %i", nreturn);
        goto out;

    }

    if (sigprocmask(SIG_BLOCK, &mainloop_sigset, NULL) == -1) {

        writelog(1, "mainloop: error sigprocmask");
        goto out;

    }

    writelog(1, "mainloop: adding signalfd %i to epoll", signal_fd);

    epoll_xdata=add_to_epoll(signal_fd, EPOLLIN | EPOLLPRI, TYPE_FD_SIGNAL, NULL, NULL, &xdata_signal);
    if ( !epoll_xdata ) goto out;

    add_xdata_to_list(epoll_xdata);

    writelog(0, "mainloop: starting epoll wait loop");

    while (exitmainloop==0) {


        int number_of_fds=epoll_wait(epoll_fd, epoll_events, MAX_EPOLL_NREVENTS, -1);

        if (number_of_fds < 0) {

            exitmainloop_error=-errno;
            exitmainloop=1;

            writelog(0, "mainloop: epoll_wait error %i", errno);

            break; /* good way to handle this ??*/

        } else {

            writelog(0, "mainloop: number of fd's: %i", number_of_fds);

        }


        for (i=0; i<number_of_fds; i++) {


            epoll_xdata=(struct epoll_extended_data_struct *) epoll_events[i].data.ptr;

            /* look what kind of fd this is and launch the custom callback */

	    if ( epoll_xdata->fd==signal_fd ) {

                /* some data on signalfd */

                readlen=read(signal_fd, &fdsi, sizeof(struct signalfd_siginfo));

                if ( readlen==-1 ) {

                    nerror=errno;

                    if ( nerror==EAGAIN ) {

                        // blocking error: back to the mainloop

                        continue;

                    }

                    writelog(0, "error %i reading from signalfd......", nerror);

                } else {

                    if ( readlen == sizeof(struct signalfd_siginfo)) {

                        // a signal read: check the signal

                        signo=fdsi.ssi_signo;

                        if ( signo==SIGHUP || signo==SIGINT || signo==SIGTERM ) {

                            writelog(0, "mainloop: caught signal %i, exit session", signo);

                            goto out;

                        } else if ( signo==SIGPIPE ) {

                            writelog(0, "mainloop: caught signal SIGPIPE, ignoring");

                        } else if ( signo == SIGCHLD) {

                            writelog(0, "Got SIGCHLD, from pid: %d", fdsi.ssi_pid);

                        } else if ( signo == SIGIO) {

                            writelog(0, "Got SIGIO.....");

                        } else if ( signo == SIGUSR1) {

                            writelog(0, "Got SIGUSR1, send every callback this signal");

			    send_callbacks_signal(signo);

                        } else {

                            writelog(0, "got unknown signal %i", signo);

                        }

                    }

                }


            } else if ( epoll_xdata->type_fd!=TYPE_FD_NOTSET ) {

                /* process the custom fd */

                /* call the custom function which is defined in epoll_xdata->data
                   with argument epoll_events[i] */

                if ( epoll_xdata->callback ) {

                    /* function defined */

                    writelog(0, "mainloop: custom fd %i", epoll_xdata->fd);

                    res=(*epoll_xdata->callback) (epoll_xdata, epoll_events[i].events, 0);

                } else {

                    writelog(0, "mainloop: error: callback not defined for %i", epoll_xdata->fd);

                }


            } else {

                // should not happen

                writelog(0, "mainloop: error: fd: %i / type %i not reckognized", epoll_xdata->fd, epoll_xdata->type_fd);
                exitmainloop=1;

            }

        }

    }

    out:

    /* send the callbacks a signal */

    send_callbacks_signal(1);

    /* remove signal from mainloop and close signal_fd*/

    res=remove_xdata_from_epoll(&xdata_signal, 0);
    close(signal_fd);
    signal_fd=0;
    remove_xdata_from_list(&xdata_signal);

    /* close epoll */

    close(epoll_fd);
    epoll_fd=0;

    return nreturn < 0 ? -1 : 0;

}

void destroy_mainloop()
{
    struct epoll_extended_data_struct *epoll_xdata=NULL;
    int res;

    logoutput("destroy_mainloop");

    /* close any remaining fd previously added to the mainloop */

    pthread_mutex_lock(&epoll_xdata_list.mutex);

    epoll_xdata=epoll_xdata_list.first;

    while (epoll_xdata) {

        epoll_xdata_list.first=epoll_xdata->next;
        epoll_xdata->next=NULL;
        epoll_xdata->prev=NULL;

        if ( epoll_fd>0 ) res=epoll_ctl(epoll_fd, EPOLL_CTL_DEL, epoll_xdata->fd, NULL);

	logoutput("destroy_mainloop: close fd: %i", epoll_xdata->fd);

        close(epoll_xdata->fd);
        free(epoll_xdata);

	epoll_xdata_list.nr--;

        epoll_xdata=epoll_xdata_list.first;

    }

    pthread_mutex_destroy(&epoll_xdata_list.mutex);

}
