/*
  2010, 2011, 2012 Stef Bon <stefbon@gmail.com>

  Add the fuse channel to the mainloop, init threads and process any fuse event.

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

#include "global-defines.h"

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
#include <syslog.h>

#define LOGGING

#define LOG_LOGAREA LOG_LOGAREA_MAINLOOP

#include "logging.h"
#include "epoll-utils.h"
#include "handlefuseevent.h"

ssize_t maxbuffsize=0;

unsigned char nextfreethread=0;

pthread_mutex_t workerthreads_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t workerthreads_cond=PTHREAD_COND_INITIALIZER;
struct fuse_workerthread_struct *workerthread_queue_first=NULL;
struct fuse_workerthread_struct *workerthread_queue_last=NULL;

struct fuse_workerthread_struct workerthread[NUM_WORKER_THREADS];

unsigned char exitfusesession=0;
static struct fuse_session *fusesession=NULL;
const char *fusemountpoint=NULL;

struct epoll_extended_data_struct xdata_fuse={0, 0, NULL, NULL, NULL, NULL};

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


int read_fuseevent(struct fuse_chan *ch, struct fuse_session *se, struct fuse_workerthread_struct *workerthread)
{
    int nreturn=0, res;


    readbuff:


    res=fuse_session_receive_buf(se, &(workerthread->fbuf), &ch);

    // res=fuse_chan_recv(&ch, workerthread->fbuf.mem, workerthread->fbuf.size);

    logoutput( "thread %i ready receive buf res: %i", workerthread->nr, res);


    if ( res==-EAGAIN ) {

        /* jump back... is this safe and will not result in an endless loop ??*/

        logoutput( "thread %i got a EAGAIN error reading the buffer", workerthread->nr);
        goto readbuff;

    } else if ( res==-EINTR ) {

	nreturn=0;

    } else if ( res<=0 ) {

        if ( res<0 ) {

            logoutput( "thread %i got error %i", workerthread->nr, res);

        } else {

            logoutput( "thread %i got zero reading the buffer", workerthread->nr);

        }

        exitfusesession=1;
        nreturn=res;

    } else {

	/* no error */

	nreturn=res;

        /* show some information about what the call is about */

	if (!(workerthread->fbuf.flags & FUSE_BUF_IS_FD)) {
            struct fuse_in_header *in = (struct fuse_in_header *) workerthread->fbuf.mem;
            char outputstring[32];
            int res2;

	    res2=print_opcode(in->opcode, outputstring, 32);

	    if ( res2>0 ) {

            	logoutput( "thread %i read %i bytes: opcode %i/%s", workerthread->nr, res, in->opcode, outputstring);

	    } else {

            	logoutput( "thread %i read %i bytes: opcode %i", workerthread->nr, res, in->opcode);

	    }

	} else {

            logoutput( "thread %i read %i bytes: opcode not available", workerthread->nr, res);

        }

	workerthread->fbuf.size=res;

    }

    return nreturn;

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

        logoutput( "unable to start thread, argument not set");
        goto out;

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

	    res=pthread_mutex_lock(&workerthreads_mutex);

	    /* wait till this thread is the next free one */

	    while ( workerthread->busy==0 && exitfusesession==0 ) {

		res=pthread_cond_wait(&workerthreads_cond, &workerthreads_mutex);

	    }

	    res=pthread_mutex_unlock(&workerthreads_mutex);

            logoutput( "thread %i waking up", workerthread->nr);

            /* after here settings cleared at (A) are set again */

            workerthread->busy=1;

        }

        // possibly activated to terminate

        if ( exitfusesession==1 ) {

	    logoutput( "thread %i exit", workerthread->nr);

            workerthread->busy=0;
            break;

        }

	res=pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

        fuse_session_process_buf(workerthread->se, &(workerthread->fbuf), workerthread->ch);

	res=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	if ( ! ( workerthread->fbuf.flags & FUSE_BUF_IS_FD ) ) {

	    /* when dealing with a memory buffer reset the buffer */

	    memset(workerthread->fbuf.mem, '\0', workerthread->fbuf.size);
	    workerthread->fbuf.size=maxbuffsize;

	}

        if ( fuse_session_exited(workerthread->se) ) {

            exitfusesession=1;
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

    out:

    if ( permanentthread ) workerthread->threadid=0;

    pthread_exit(NULL);

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

    logoutput("in queue: %s", outputstring);

}

void stopfusethreads()
{
    int i;

    for (i=0; i<NUM_WORKER_THREADS; i++) {

        if ( workerthread[i].threadid>0 ) {

    	    pthread_cancel(workerthread[i].threadid);
    	    workerthread[i].threadid=0;

	}

	if ( ! ( workerthread[i].fbuf.flags & FUSE_BUF_IS_FD ) ) {

	    /* when space is allocated free it */

    	    if ( workerthread[i].fbuf.mem ) {

        	free(workerthread[i].fbuf.mem);
        	workerthread[i].fbuf.mem=NULL;

    	    }

	}

    }

    pthread_cond_destroy(&workerthreads_cond);
    pthread_mutex_destroy(&workerthreads_mutex);

}

void terminatefuse(struct epoll_extended_data_struct *epoll_xdata)
{
    struct fuse_chan *ch=NULL;

    logoutput("terminatefuse");

    /* remove from epoll */

    if ( xdata_fuse.fd>0 ) {

	remove_xdata_from_epoll(&xdata_fuse, 0);

    }

    remove_xdata_from_list(&xdata_fuse);

    /* try to determine the channel */

    if ( ! ch ) ch=(struct fuse_chan *) xdata_fuse.data;

    if ( ! ch ) {

	if ( fusesession ) ch=fuse_session_next_chan(fusesession, NULL);

    }

    if ( fusemountpoint ) {

	fuse_unmount(fusemountpoint, ch);
	xdata_fuse.fd=0;

	fusemountpoint=NULL;
	ch=NULL;

    }

    if ( fusesession ) {

	fuse_session_destroy(fusesession);
	fusesession=NULL;

    }

    stopfusethreads();

}


/*
 * process an event on a fuse fd
 * main function is to read the event and start a new thread to handle the event
 * first walk through the permanent worker threads for a free one
 * if not found one create a temporary worker thread
 *
 * parameters:
 * . event
 */

int process_fuse_event(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo)
{
    int nreturn=0, res, lenread=0;
    struct fuse_session *se=NULL;
    struct fuse_chan *ch=NULL;

    if ( signo>0 ) {

	/* terminate the fuse part */

	logoutput("process_fuse_event: received a signal from mainloop to terminate, doing nothing now");

	// if ( fusesession ) terminatefuse(epoll_xdata);

    } else if ( ! fusesession ) {

	logoutput("process_fuse_event: no session available..error");

    } else if ( events & (EPOLLERR | EPOLLHUP) ) {

        logoutput( "process_fuse_event: event %i causes exit", events);

        setmainloop_exit(0);
        goto out;

    } else if ( ! (events & EPOLLIN) ) {

	/* only react on incoming events */

	/* huh?? */

        logoutput( "process_fuse_event: fd %i not available for read", epoll_xdata->fd);
        goto out;

    }

    ch=(struct fuse_chan *) epoll_xdata->data;
    se=fuse_chan_session(ch);

    /* look for a free thread */

    res=pthread_mutex_lock(&workerthreads_mutex);

    if ( ! workerthread_queue_first ) {

	/* there is no thread available... just wait here... 
	   possibly add a timeout here*/

	while ( ! workerthread_queue_first ) {

	    res=pthread_cond_wait(&workerthreads_cond, &workerthreads_mutex);

	}

    }


    if ( workerthread_queue_first ) {

	/* here for testing... */

	log_threadsqueue();

	/* take the first */

	nextfreethread=workerthread_queue_first->nr;

        workerthread[nextfreethread].se=se;
        workerthread[nextfreethread].ch=ch;

        /* reset the buffer */

        workerthread[nextfreethread].fbuf.size=maxbuffsize;

	logoutput("trying to set thread %i to work", nextfreethread);

	/* read the buffer */

	lenread=read_fuseevent(ch, se, &workerthread[nextfreethread]);

	if ( exitfusesession==1 ) {

	    if ( lenread<=0 ) {

		setmainloop_exit(lenread);

	    } else {

    		setmainloop_exit(0);

	    }

    	    goto out;

	} else if ( lenread>0 ) {

	    workerthread[nextfreethread].busy=1;

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

	}

    }

    res=pthread_mutex_unlock(&workerthreads_mutex);

    out:

    logoutput("process_fuse_event: nreturn %i", nreturn);

    return nreturn;

}

int addfusechannelstomainloop(struct fuse_session *se, const char *mountpoint)
{
    struct fuse_chan *ch = NULL;
    int nreturn=0;

    if ( fusesession || fusemountpoint ) {

	logoutput("add fuse channels to mainloop: error, fusesession and/or mountpoint already defined");
	goto out;

    } else {

	fusesession=se;
	fusemountpoint=mountpoint;

    }

    ch = fuse_session_next_chan(fusesession, NULL);

    while (ch) {
        size_t buffsize=fuse_chan_bufsize(ch);
        int fd, fdflags, res;
        struct epoll_extended_data_struct *epoll_xdata;

        if ( buffsize > maxbuffsize ) maxbuffsize=buffsize;

        fd=fuse_chan_fd(ch);

	/* set to nonblocking */

	fdflags=fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fdflags | O_NONBLOCK);

        logoutput("add fuse channels to mainloop: adding fuse fd %i to mainloop", fd);

        epoll_xdata=add_to_epoll(fd, EPOLLIN, TYPE_FD_FUSE, &process_fuse_event, (void *) ch, &xdata_fuse);

	if ( epoll_xdata ) {

	    add_xdata_to_list(epoll_xdata);

	} else {

	    logoutput("add fuse channels to mainloop: error adding fuse to mainloop");
	    nreturn=-EIO;
	    goto out;

	}

	/* walk through every channel for this session --- ahum there is only one channel per session --- */

        ch = fuse_session_next_chan(fusesession, ch);

    }

    logoutput("mainloop: setting maximum buffer size: %i", maxbuffsize);

    out:

    return nreturn;

}

int startfusethreads()
{
    int i, nreturn=0, res;
    pthread_t pthreadid;

    /* init and fire up the different fuse worker threads */

    for (i=0; i<NUM_WORKER_THREADS; i++) {


        workerthread[i].busy=0;
        workerthread[i].nr=i;
        workerthread[i].typeworker=TYPE_WORKER_PERMANENT;

        /* create the buffer per thread:
           every thread gets it's own buffer
           use the new buffer definition per 201201, a fd 
           is also possible */

	/* here where to set the flags..... */

	workerthread[i].fbuf.flags=0;

	if ( ! ( workerthread[i].fbuf.flags & FUSE_BUF_IS_FD ) ) {
    	    workerthread[i].fbuf.mem=malloc(maxbuffsize);

    	    if ( ! workerthread[i].fbuf.mem ) {

        	nreturn=-ENOMEM;
        	goto out;

    	    }

	    memset(workerthread[i].fbuf.mem, '\0', maxbuffsize);
    	    workerthread[i].fbuf.size=maxbuffsize;

	}

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

            logoutput( "Error creating a new thread (nr: %i, error: %i).", i, res);
            nreturn=res;

            goto out;

        }

    }

    out:

    return nreturn;

}

