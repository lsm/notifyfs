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

int epoll_fd=0;

struct epoll_extended_data_struct *epoll_xdata_list=NULL;
pthread_mutex_t epoll_xdata_list_mutex=PTHREAD_MUTEX_INITIALIZER;

int add_to_epoll(int fd, uint32_t events, unsigned char typefd, void *callback, void *data)
{
    struct epoll_extended_data_struct *epoll_xdata;
    struct epoll_event *event;
    int nreturn=0, res;

    writelog(1, "add_to_epoll: add fd %i",fd);

    pthread_mutex_lock(&epoll_xdata_list_mutex);

    epoll_xdata=malloc(sizeof(struct epoll_extended_data_struct));

    if ( epoll_xdata) {

        epoll_xdata->fd=fd;
        epoll_xdata->type_fd=typefd;

        epoll_xdata->data=data;
        epoll_xdata->callback=callback;

        pthread_mutex_init(&epoll_xdata->read_mutex, NULL);

    } else {

        nreturn=-ENOMEM;
        goto unlock;

    }

    event=malloc(sizeof(struct epoll_event));

    if ( event ) {

        event->events=events;

        // mutual links
        event->data.ptr=(void *) epoll_xdata;
        epoll_xdata->event=event;


    } else {

        free(epoll_xdata);
        nreturn=-ENOMEM;
        goto unlock;

    }

    // add this fd to the epoll instance

    res=epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, event);

    if ( res==-1 ) {

        nreturn=-errno;

        free(event);
        free(epoll_xdata);
        goto unlock;

    }

    // add epoll_xdata to global list

    if ( epoll_xdata_list ) epoll_xdata_list->prev=epoll_xdata;
    epoll_xdata->next=epoll_xdata_list;
    epoll_xdata_list=epoll_xdata;

    unlock:

    pthread_mutex_unlock(&epoll_xdata_list_mutex);

    out:

    writelog(2, "add_to_epoll: return %i", nreturn);

    return nreturn;

}

int remove_xdata_from_epoll(struct epoll_extended_data_struct *epoll_xdata, unsigned char lockset)
{
    int nreturn=0;

    if (lockset==0) pthread_mutex_lock(&epoll_xdata_list_mutex);

    nreturn=epoll_ctl(epoll_fd, EPOLL_CTL_DEL, epoll_xdata->fd, NULL);

    if ( nreturn==0 ) {

        if ( epoll_xdata_list == epoll_xdata ) {

            epoll_xdata_list=epoll_xdata->next;
            if ( epoll_xdata_list ) epoll_xdata_list->prev=NULL;

        } else {

            if ( epoll_xdata->prev ) epoll_xdata->prev->next=epoll_xdata->next;
            if ( epoll_xdata->next ) epoll_xdata->next->prev=epoll_xdata->prev;

        }

        if ( epoll_xdata->event ) free(epoll_xdata->event);
        free(epoll_xdata);

    } else {

        writelog(2,"error when removing fd %i (type: %i) from epoll", epoll_xdata->fd, epoll_xdata->type_fd);

    }

    if (lockset==0) pthread_mutex_unlock(&epoll_xdata_list_mutex);

    return nreturn;

}


int remove_fd_from_epoll(int fd)
{
    int nreturn=0;
    struct epoll_extended_data_struct *epoll_xdata=NULL;

    // look for epoll_xdata

    pthread_mutex_lock(&epoll_xdata_list_mutex);

    if ( epoll_xdata_list ) {

        epoll_xdata=epoll_xdata_list;

        while (epoll_xdata) {

            if ( epoll_xdata->fd==fd ) break;

            epoll_xdata=epoll_xdata->next;

        }

    }

    if ( epoll_xdata ) {

        nreturn=remove_xdata_from_epoll(epoll_xdata, 1);

    } else {


        writelog(2, "fd %i not found on epoll xdata list ", fd);

    }

    pthread_mutex_unlock(&epoll_xdata_list_mutex);

    return nreturn;

}

//
// function to scan the current list of epoll_xdata a specific fd is already on list
//

unsigned char scan_epoll_list(int fd)
{
    struct epoll_extended_data_struct *epoll_xdata=NULL;
    unsigned char nfound=0;

    pthread_mutex_lock(&epoll_xdata_list_mutex);

    if ( epoll_xdata_list ) {

        epoll_xdata=epoll_xdata_list;

        while (epoll_xdata) {

            if ( epoll_xdata->fd==fd ) {

                nfound=1;
                break;

            }

            epoll_xdata=epoll_xdata->next;

        }

    }

    pthread_mutex_unlock(&epoll_xdata_list_mutex);

    return nfound;

}

int init_mainloop()
{

    // create an epoll instance

    epoll_fd=epoll_create(MAX_EPOLL_NRFDS);

    if ( epoll_fd==-1 ) epoll_fd=-errno;

    return epoll_fd;

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
    struct epoll_extended_data_struct *epoll_xdata;



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

    res=add_to_epoll(signal_fd, EPOLLIN, TYPE_FD_SIGNAL, NULL, NULL);
    if ( res<0 ) goto out;


    writelog(0, "mainloop: starting epoll wait loop");


    while (1) {

        int number_of_fds=epoll_wait(epoll_fd, epoll_events, MAX_EPOLL_NREVENTS, -1);

        if (number_of_fds < 0) {

            nreturn=-errno;

            writelog(0, "mainloop: epoll_wait error");

            goto out; /* good way to handle this ??*/

        } else {

            writelog(1, "mainloop: number of fd's: %i", number_of_fds);

        }


        for (i=0; i<number_of_fds; i++) {


            epoll_xdata=(struct epoll_extended_data_struct *) epoll_events[i].data.ptr;

            /* look what kind of fd this is and launch the custom callback */


	     if ( epoll_xdata->type_fd==TYPE_FD_SIGNAL ) {

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

                            writelog(1, "Got SIGIO.....");

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

                    res=(*epoll_xdata->callback) (&epoll_events[i]);

                } else {

                    writelog(0, "mainloop: error: callback not defined for %i", epoll_xdata->fd);

                }

            } else {

                // should not happen

                writelog(0, "mainloop: error: type fd %i not reckognized", epoll_xdata->type_fd);

            }

        }

    }


    out:

    // free epoll data, associated event and close fd

    if ( epoll_xdata_list ) {

        epoll_xdata=epoll_xdata_list;

        // no need to lock here: there are not other threads anymore

        while (epoll_xdata) {

            epoll_xdata_list=epoll_xdata->next;

            res=epoll_ctl(epoll_fd, EPOLL_CTL_DEL, epoll_xdata->fd, NULL);

            close(epoll_xdata->fd);

            if ( epoll_xdata->event ) free(epoll_xdata->event);
            free(epoll_xdata);

            epoll_xdata=epoll_xdata_list;

        }

    }

    close(epoll_fd);

    return nreturn < 0 ? -1 : 0;

}
