/*
  2010, 2011 Stef Bon <stefbon@gmail.com>
  fuse-loop-epoll-mt.c
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

#define FUSE_USE_VERSION 26
#define _REENTRANT
#define _GNU_SOURCE

#define PACKAGE_VERSION "1.4"

#include <fuse/fuse_lowlevel.h>
#include <linux/fuse.h>

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
#include "fuse-loop-epoll-mt.h"

int epoll_fd=0;
ssize_t maxbuffsize=0;

struct fuse_workerthread_struct *tmp_workerthread_list=NULL;
pthread_mutex_t temporarythreads_mutex=PTHREAD_MUTEX_INITIALIZER;

unsigned char nextfreethread=0;
unsigned char fuseevent=0;
pthread_mutex_t workerthreads_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t workerthreads_cond=PTHREAD_COND_INITIALIZER;

struct fuse_workerthread_struct *workerthread_queue_first=NULL;
struct fuse_workerthread_struct *workerthread_queue_last=NULL;

struct fuse_workerthread_struct workerthread[NUM_WORKER_THREADS];

unsigned char exitsession=0;

struct epoll_extended_data_struct *epoll_xdata_list=NULL;
pthread_mutex_t epoll_xdata_list_mutex=PTHREAD_MUTEX_INITIALIZER;

typedef struct FUSEOPCODEMAP {
		const char *name;
		int opcode;
		} FUSEOPCODEMAP;

static const FUSEOPCODEMAP fuseopcodemap[] = {
		    {"FUSE_LOOKUP", 1},
		    {"FUSE_FORGET", 2},
		    {"FUSE_GETATTR", 3},
		    {"FUSE_SETATTR", 4},
		    {"FUSE_READLINK", 5},
		    {"FUSE_SYMLINK", 6},
		    {"FUSE_MKNOD", 8},
		    {"FUSE_MKDIR", 9},
		    {"FUSE_UNLINK", 10},
		    {"FUSE_RMDIR", 11},
		    {"FUSE_RENAME", 12},
		    {"FUSE_LINK", 13},
		    {"FUSE_OPEN", 14},
		    {"FUSE_READ", 15},
		    {"FUSE_WRITE", 16},
		    {"FUSE_STATFS", 17},
		    {"FUSE_RELEASE", 18},
		    {"FUSE_FSYNC", 20},
		    {"FUSE_SETXATTR", 21},
		    {"FUSE_GETXATTR", 22},
		    {"FUSE_LISTXATTR", 23},
		    {"FUSE_REMOVEXATTR", 24},
		    {"FUSE_FLUSH", 25},
		    {"FUSE_INIT", 26},
		    {"FUSE_OPENDIR", 27},
		    {"FUSE_READDIR", 28},
		    {"FUSE_RELEASEDIR", 29},
		    {"FUSE_FSYNCDIR", 30},
		    {"FUSE_GETLK", 31},
		    {"FUSE_SETLK", 32},
		    {"FUSE_SETLKW", 33},
		    {"FUSE_ACCESS", 34},
		    {"FUSE_CREATE", 35},
		    {"FUSE_INTERRUPT", 36},
		    {"FUSE_BMAP", 37},
		    {"FUSE_DESTROY", 38},
		    {"FUSE_IOCTL", 39},
		    {"FUSE_POLL", 40},
		    {"FUSE_NOTIFY_REPLY", 41},
		    {"FUSE_BATCH_FORGET", 42}};


static int print_opcode(int opcode, char *string, size_t size)
{
    int i, pos=0, len;

    for (i=0;i<(sizeof(fuseopcodemap)/sizeof(fuseopcodemap[0]));i++) {

        if ( fuseopcodemap[i].opcode==opcode ) {

            len=strlen(fuseopcodemap[i].name);

            if ( pos + len + 1  > size ) {

                pos=-1;
                goto out;

            } else {

                if ( pos>0 ) {

                    *(string+pos)=';';
                    pos++;

                }

                strcpy(string+pos, fuseopcodemap[i].name);
                pos+=len;

            }

	    break;

        }

    }

    out:

    return pos;

}




//
// a thread
// to process the fuse event
// it maybe a permanent or a temporary one
//

static void *thread_to_process_fuse_event(void *threadarg)
{
    struct fuse_workerthread_struct *workerthread;
    bool permanentthread;
    int res;

    workerthread=(struct fuse_workerthread_struct *) threadarg;

    if ( ! workerthread ) {

        writelog(0, "unable to start thread, argument not set");
        goto out;

    } else {

	writelog(0, "starting thread %i", workerthread->nr);

    }

    permanentthread=false;
    if ( workerthread->typeworker==TYPE_WORKER_PERMANENT ) permanentthread=true;
    workerthread->threadid=pthread_self();

    while (1) {

        // if permanent : wait for the condition
        // this is set by the mainloop to inform this thread there is data ready to process

        if ( permanentthread ) {

            // wait for event

            workerthread->busy=0;

            /* clear various settings... (A)*/
            workerthread->se=NULL;
            workerthread->ch=NULL;
            workerthread->read_mutex=NULL;

	    res=pthread_mutex_lock(&workerthreads_mutex);

	    /* wait till this thread is the next free one */

	    while ( workerthread->busy==0 ) {

		res=pthread_cond_wait(&workerthreads_cond, &workerthreads_mutex);

		if ( exitsession==1 ) break;

	    }

	    res=pthread_mutex_unlock(&workerthreads_mutex);

            writelog(0, "thread %i waking up", workerthread->nr);

            /* after here settings cleared at (A) are set again */

            workerthread->busy=2;

        }

        // possibly activated to terminate

        if ( exitsession==1 ) {

            workerthread->busy=0;
            break;

        }

        readbuff:

        if (workerthread->read_mutex) pthread_mutex_lock(workerthread->read_mutex);

        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

        res=fuse_session_receive_buf(workerthread->se, &(workerthread->fbuf), &(workerthread->ch));

        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        writelog(0, "thread %i ready receive buf res: %i", workerthread->nr, res);

        if (workerthread->read_mutex) pthread_mutex_unlock(workerthread->read_mutex);

        if ( res==-EINTR ) {

            /* jump back... is this safe and will not result in an endless loop ??*/

            writelog(0, "thread %i got a EINTR error reading the buffer", workerthread->nr);
            goto readbuff;

        } else if ( res<=0 ) {

            if ( res<0 ) {

                writelog(0, "thread %i got error %i", workerthread->nr, res);

            } else {

                writelog(0, "thread %i got zero reading the buffer", workerthread->nr);

            }

            exitsession=1;
            goto out;

        } else {

            /* show some information about what the call is about */

	    if (!(workerthread->fbuf.flags & FUSE_BUF_IS_FD)) {
                struct fuse_in_header *in = (struct fuse_in_header *) workerthread->fbuf.mem;
                char outputstring[32];

		res=print_opcode(in->opcode, outputstring, 32);

		if ( res>0 ) {

            	    writelog(0, "thread %i read %i bytes: opcode %i/%s", workerthread->nr, res, in->opcode, outputstring);

		} else {

            	    writelog(0, "thread %i read %i bytes: opcode %i/%s", workerthread->nr, res, in->opcode);

		}

	    } else {

                writelog(0, "thread %i read %i bytes: opcode not available", workerthread->nr, res);

            }

        }

        fuse_session_process_buf(workerthread->se, &(workerthread->fbuf), workerthread->ch);

        if ( fuse_session_exited(workerthread->se) ) {

            exitsession=1;
            break;

        }

        if ( ! permanentthread ) {

            break;

        }

	/* ready: put it on tail */

	res=pthread_mutex_lock(&workerthreads_mutex);

	if ( workerthread_queue_last ) {

	    workerthread_queue_last->next=workerthread;
	    workerthread->prev=workerthread_queue_last;

	    workerthread_queue_last=workerthread;

	} else {

	    workerthread_queue_last=workerthread;

	}

	if ( ! workerthread_queue_first ) workerthread_queue_first=workerthread;

	workerthread->busy=0;

	res=pthread_mutex_unlock(&workerthreads_mutex);

    }

    if ( ! permanentthread ) {

        // when temporary thread: free 
        // remove itself from list of temp threads

        pthread_mutex_lock(&temporarythreads_mutex);

        if ( tmp_workerthread_list==workerthread ) {

            tmp_workerthread_list=workerthread->next; /*shift*/
            if ( tmp_workerthread_list ) tmp_workerthread_list->prev=NULL;

        } else {

            if ( workerthread->prev ) workerthread->prev->next=workerthread->next;
            if ( workerthread->next ) workerthread->next->prev=workerthread->prev;

        }

        pthread_mutex_unlock(&temporarythreads_mutex);

        workerthread->prev=NULL;
        workerthread->next=NULL;

        if ( workerthread->fbuf.mem ) {

            free(workerthread->fbuf.mem);
            workerthread->fbuf.mem=NULL;

        }

        workerthread->fbuf.size=0;

        free(workerthread);

    }

    out:

    if ( permanentthread ) workerthread->threadid=0;

    pthread_exit(NULL);

}

int start_temporary_thread(struct fuse_chan *ch, struct fuse_session *se, pthread_mutex_t *read_mutex)
{
    struct fuse_workerthread_struct *tmp_workerthread=NULL;
    bool threadfound=false;
    pthread_t pthreadid;
    int nreturn=0, res;


    while ( ! threadfound) {

        // no free thread found in the pool
        // start a temp thread especially for this purpose

        writelog(0, "process_fuse_event: no free thread found, starting a temporary thread");

        tmp_workerthread=malloc(sizeof(struct fuse_workerthread_struct));

        if ( tmp_workerthread ) {

            tmp_workerthread->busy=0;
            tmp_workerthread->nr=0;
            tmp_workerthread->typeworker=TYPE_WORKER_TEMPORARY;
            tmp_workerthread->se=se;
            tmp_workerthread->ch=ch;
            tmp_workerthread->read_mutex=read_mutex;

            /*  create the buffer per thread:
                every thread gets it's own buffer
                use the new buffer definition per 201201, a fd 
                is also possible */

            tmp_workerthread->fbuf.mem=malloc(maxbuffsize);

            if ( ! tmp_workerthread->fbuf.mem ) {

                nreturn=-ENOMEM;
                goto out;

            }

            tmp_workerthread->fbuf.size=maxbuffsize;
            tmp_workerthread->fbuf.flags=0;
            tmp_workerthread->fbuf.fd=0;
            tmp_workerthread->fbuf.pos=0;

            // insert at beginning of list of temporary threads

            pthread_mutex_lock(&temporarythreads_mutex);

            tmp_workerthread->prev=NULL;
            if ( tmp_workerthread_list ) tmp_workerthread_list->prev=tmp_workerthread;
            tmp_workerthread->next=tmp_workerthread_list;
            tmp_workerthread_list=tmp_workerthread;

            pthread_mutex_unlock(&temporarythreads_mutex);

            // start temporary thread
            threadfound=true;

            res=pthread_create(&pthreadid, NULL, thread_to_process_fuse_event, (void *) tmp_workerthread);

            if ( res!=0 ) {

                // very robust action: free everything

                free(tmp_workerthread);

                writelog(0, "process_fuse_event: cannot start temp thread");

                threadfound=false;

            }

        } else {

            writelog(0, "process_fuse_event: cannot allocate space for temp thread");

        }

    }

    out:

    return nreturn;

}

void log_threadsqueue()
{
    struct fuse_workerthread_struct *workerthread;
    char outputstring[64], *tmpchar;
    int res, len=64;

    workerthread=workerthread_queue_first;

    tmpchar=&outputstring[0];

    while (workerthread) {

	res=snprintf(tmpchar, len, "%i:", workerthread->nr);

	len-=res;
	tmpchar+=res;

	workerthread=workerthread->next;

    }

    writelog(1, "in queue: %s", outputstring);

}


/*
 * process an event on a fuse fd
 * main function is to read the event and start a new thread to handle the event
 * first walk through the permanent worker threads for a free one
 * if not found one create a temporary worker thread
 *
 * parameters:
 * . events: event occured, like EPOLLIN
 * . ch: fuse channel event is available on
 */

static int process_fuse_event(uint32_t events, struct fuse_chan *ch, struct fuse_session *se, pthread_mutex_t *read_mutex)
{
    int nreturn=0, res;
    unsigned char ncount=0;
    bool threadfound=false;


    // check first it's not exit

    if ( fuse_session_exited(se) || exitsession==1) {

        exitsession=1;

        goto out;

    }

    // only react on incoming events

    if ( events & (EPOLLERR | EPOLLHUP) ) {

        writelog(0, "process_fuse_event: event %i causes exit", events);

        exitsession=1;
        goto out;

    } else if ( ! (events & EPOLLIN) ) {

        writelog(0, "process_fuse_event: no input event %i", events);
        goto out;

    }


    /* look for a free thread */

    threadfound=false;

    res=pthread_mutex_lock(&workerthreads_mutex);

    if ( workerthread_queue_first ) {

	/* here for testing... */

	log_threadsqueue();

	/* take the first */

	nextfreethread=workerthread_queue_first->nr;
	workerthread[nextfreethread].busy=1;

        writelog(1, "trying to set thread %i to work", nextfreethread);

        workerthread[nextfreethread].se=se;
        workerthread[nextfreethread].ch=ch;
        workerthread[nextfreethread].read_mutex=read_mutex;

        /* reset the buffer */

        workerthread[nextfreethread].fbuf.size=maxbuffsize;

	/* remove from queue */

	if ( workerthread_queue_first->next ) {

	    workerthread_queue_first=workerthread_queue_first->next;
	    workerthread_queue_first->prev=NULL;

	} else {

	    workerthread_queue_first=NULL;
	    workerthread_queue_last=NULL;

	}

	workerthread[nextfreethread].next=NULL;
	workerthread[nextfreethread].prev=NULL;

        /* use a condition variable in stead of semaphore:
           send a broadcast */

	res=pthread_cond_broadcast(&workerthreads_cond);

	threadfound=true;

    }

    res=pthread_mutex_unlock(&workerthreads_mutex);

    if ( ! threadfound ) {

        // no free thread found in the pool
        // start a temp thread especially for this purpose

        res=start_temporary_thread(ch, se, read_mutex);
        if ( res<0 ) nreturn=res;

    }

    out:

    writelog(3, "process_fuse_event: nreturn %i", nreturn);

    return nreturn;

}


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
    int nreturn=0;

    // create an epoll instance

    epoll_fd=epoll_create(MAX_EPOLL_NRFDS);

    if ( epoll_fd==-1 ) nreturn=-errno;

    return nreturn;

}



int fuse_session_loop_epoll_mt(struct fuse_session *se)
{
    int signal_fd, fuse_fd;
    struct epoll_event epoll_events[MAX_EPOLL_NREVENTS];
    int i, res, ncount, ntries1;
    struct fuse_chan *ch = NULL;
    ssize_t readlen;
    struct signalfd_siginfo fdsi;
    int signo, nreturn, nerror;
    sigset_t fuse_sigset;
    struct epoll_extended_data_struct *epoll_xdata;
    pthread_t pthreadid;

    // FUSE: walk through all the channels and add the associated fd to epoll

    ch = fuse_session_next_chan(se, NULL);

    while (ch) {
        size_t buffsize=fuse_chan_bufsize(ch);

        if ( buffsize > maxbuffsize ) maxbuffsize=buffsize;

        // determine the fd of the channel

        fuse_fd=fuse_chan_fd(ch);

        writelog(1, "mainloop: adding fuse fd %i to epoll", fuse_fd);

        res=add_to_epoll(fuse_fd, EPOLLIN, TYPE_FD_FUSE, NULL, (void *) ch);

        ch = fuse_session_next_chan(se, ch);

    }


    /*
        SIGNALS: set the set of signals for signalfd to listen to 
    */

    sigemptyset(&fuse_sigset);

    sigaddset(&fuse_sigset, SIGINT);
    sigaddset(&fuse_sigset, SIGIO);
    sigaddset(&fuse_sigset, SIGHUP);
    sigaddset(&fuse_sigset, SIGTERM);
    sigaddset(&fuse_sigset, SIGPIPE);
    sigaddset(&fuse_sigset, SIGCHLD);
    sigaddset(&fuse_sigset, SIGUSR1);

    signal_fd = signalfd(-1, &fuse_sigset, SFD_NONBLOCK);

    if (signal_fd == -1) {

        nreturn=-errno;
        writelog(1, "mainloop: unable to create signalfd, error: %i", nreturn);
        goto out;

    }

    if (sigprocmask(SIG_BLOCK, &fuse_sigset, NULL) == -1) {

        writelog(1, "mainloop: error sigprocmask");
        goto out;

    }

    writelog(1, "mainloop: adding signalfd %i to epoll", signal_fd);

    res=add_to_epoll(signal_fd, EPOLLIN, TYPE_FD_SIGNAL, NULL, NULL);
    if ( res<0 ) goto out;


    /* init and fire up the different fuse worker threads */

    for (i=0; i<NUM_WORKER_THREADS; i++) {


        workerthread[i].busy=0;
        workerthread[i].nr=i;
        workerthread[i].typeworker=TYPE_WORKER_PERMANENT;

        /* create the buffer per thread:
           every thread gets it's own buffer
           use the new buffer definition per 201201, a fd 
           is also possible */

        workerthread[i].fbuf.mem=malloc(maxbuffsize);

        if ( ! workerthread[i].fbuf.mem ) {

            nreturn=-ENOMEM;
            goto out;

        }

        workerthread[i].fbuf.size=maxbuffsize;
        workerthread[i].fbuf.flags=0;
        workerthread[i].fbuf.fd=0;
        workerthread[i].fbuf.pos=0;

	/* add thread to queue (no need to lock)*/

	if ( ! workerthread_queue_first ) workerthread_queue_first=&(workerthread[i]);

	if ( ! workerthread_queue_last ) {

	    workerthread_queue_last=&(workerthread[i]);

	} else {

	    workerthread_queue_last->next=&(workerthread[i]);
	    workerthread[i].prev=workerthread_queue_last;

	    workerthread_queue_last=&(workerthread[i]);

	}

        res=pthread_create(&pthreadid, NULL, thread_to_process_fuse_event, (void *) &workerthread[i]);

        if ( res!=0 ) {

            // error

            writelog(0, "Error creating a new thread (nr: %i, error: %i).", i, res);

            goto out;

        }

    }


    writelog(0, "mainloop: starting epoll wait loop");


    while (!fuse_session_exited(se) && exitsession!=1) {

        if ( exitsession==1 ) goto out;


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

            /* look what kind of fd this is */

            if ( epoll_xdata->type_fd==TYPE_FD_FUSE ) {

                /* it's a fuse thing */

                ch = (struct fuse_chan *) epoll_xdata->data;

                if ( ch ) {

                    res=process_fuse_event(epoll_events[i].events, ch, se, &(epoll_xdata->read_mutex));

                    if ( res<0 || exitsession==1 ) {

                        fuse_session_exit(se);
                        exitsession=1;
                        goto out;

                    }

                } else {

                    writelog(0, "mainloop: error: unable to determine the channel");

                }

            } else if ( epoll_xdata->type_fd==TYPE_FD_SIGNAL ) {


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

                            exitsession=1;
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


            } else if ( epoll_xdata->type_fd>TYPE_FD_CUSTOMMIN && epoll_xdata->type_fd<TYPE_FD_CUSTOMMAX ) {

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

    exitsession=1; /* possibly has been set to 1 before, make sure its 1 */

    // send waiting workerthreads a signal
    // they will exit cause exitsession==1

    if ( tmp_workerthread_list ) {

        struct fuse_workerthread_struct *tmp_workerthread=tmp_workerthread_list;

        while ( tmp_workerthread ) {

            tmp_workerthread_list=tmp_workerthread->next;

            pthread_cancel(tmp_workerthread->threadid);

            free(tmp_workerthread);

            tmp_workerthread=tmp_workerthread_list;

        }

    }

    // stop any thread if still active

    for (i=0; i<NUM_WORKER_THREADS; i++) {

        if ( workerthread[i].threadid>0 ) pthread_cancel(workerthread[i].threadid);

    }

    pthread_cond_destroy(&workerthreads_cond);
    pthread_mutex_destroy(&workerthreads_mutex);


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

    // TODO: destroy any mutex and condition variable

    fuse_session_reset(se);

    return nreturn < 0 ? -1 : 0;

}
