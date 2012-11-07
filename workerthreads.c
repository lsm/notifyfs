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

#include "global-defines.h"

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

#include <sys/wait.h>
#include <pthread.h>
#include <syslog.h>

#define LOGGING
#define LOG_LOGAREA LOG_LOGAREA_MAINLOOP

#include "logging.h"
#include "epoll-utils.h"
#include "workerthreads.h"

#ifndef NOTIFYFS_NUMBERTHREADS
#define NOTIFYFS_NUMBERTHREADS	9
#endif

struct workerthread_struct *firstinqueue;
struct workerthread_struct *lastinqueue;

static pthread_mutex_t queue_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond=PTHREAD_COND_INITIALIZER;

static pthread_mutex_t work_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond=PTHREAD_COND_INITIALIZER;

static pthread_mutex_t addremovethreads_mutex=PTHREAD_MUTEX_INITIALIZER;

unsigned numberthreads=0;
unsigned threadindex=0;

struct workerthread_struct *get_thread_from_queue(unsigned char removethread)
{
    struct workerthread_struct *workerthread=NULL;
    int res;

    res=pthread_mutex_lock(&queue_mutex);

    /* there is no thread available... just wait here... 
	possibly add a timeout here....*/

    while ( ! firstinqueue ) {

	res=pthread_cond_wait(&queue_cond, &queue_mutex);

    }

    if ( firstinqueue ) {

	workerthread=firstinqueue;
	firstinqueue=workerthread->next;

	workerthread->next=NULL;

	if (lastinqueue==workerthread) lastinqueue=NULL;

    } else {

	logoutput("process_fuse_event: no free thread available");

    }

    if (removethread==1) numberthreads--;

    res=pthread_mutex_unlock(&queue_mutex);

    return workerthread;

}

void put_thread_to_queue(struct workerthread_struct *workerthread, unsigned char newthread)
{
    int res;

    res=pthread_mutex_lock(&queue_mutex);

    workerthread->next=NULL;
    workerthread->work=0;

    if ( lastinqueue ) lastinqueue->next=workerthread;
    lastinqueue=workerthread;

    if ( ! firstinqueue ) {

	firstinqueue=workerthread;

	/* back on queue: send a signal in case it's the first and someone is waiting */

	res=pthread_cond_broadcast(&queue_cond);

    }

    if (newthread==1) {

	numberthreads++;
	threadindex++;
	workerthread->nr=threadindex;

    }

    res=pthread_mutex_unlock(&queue_mutex);

}

//
// a thread
// to process the fuse event
// it maybe a permanent or a temporary one
//

void thread_to_process_events(void *threadarg)
{
    struct workerthread_struct *workerthread;
    int res;

    workerthread=(struct workerthread_struct *) threadarg;

    if ( ! workerthread ) {

        logoutput1( "unable to start thread, argument not set");
        return;

    }

    workerthread->threadid=pthread_self();

    while (1) {

    	workerthread->work=0;

	/* wait till this thread has to do some work */

	res=pthread_mutex_lock(&work_mutex);

	while ( workerthread->work==0 ) {

	    res=pthread_cond_wait(&work_cond, &work_mutex);

	}

	res=pthread_mutex_unlock(&work_mutex);


	/* do work assigned to this thread */

	res=pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

	if (workerthread->processevent_cb) {

	    (* workerthread->processevent_cb) (workerthread->data);

	}

	res=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	/* put back on queue */

	put_thread_to_queue(workerthread, 0);

    }

}

void signal_workerthread(struct workerthread_struct *workerthread)
{

	pthread_mutex_lock(&work_mutex);

	workerthread->work=1;

	pthread_cond_broadcast(&work_cond);
	pthread_mutex_unlock(&work_mutex);

}



/* create and add threads to the global list of threads */

int add_workerthreads(unsigned number)
{
    struct workerthread_struct *workerthread=NULL;
    int nreturn=0, i, res;

    for (i=0; i<number; i++) {

	nreturn=0;

	/* create the workerthread struct */

	workerthread=malloc(sizeof(struct workerthread_struct));

	if ( ! workerthread ) {

	    nreturn=-ENOMEM;
	    continue;

	}

	workerthread->work=0;
	workerthread->threadid=0;
	workerthread->nr=0;
	workerthread->processevent_cb=NULL;
	workerthread->data=NULL;
	workerthread->next=NULL;

	put_thread_to_queue(workerthread, 1);

	/* start thread */

	res=pthread_create(&workerthread->threadid, NULL, (void *) &thread_to_process_events, (void *) workerthread);

	if ( res!=0 ) {

    	    logoutput( "Error creating a new thread (nr: %i, error: %i).", i, res);
    	    nreturn=res;
	    break;

	}

    }

    return nreturn;

}

/* remove/destroy threads from the queue*/

void remove_workerthreads(unsigned number)
{
    struct workerthread_struct *workerthread=NULL;
    int i;

    for (i=0; i<number; i++) {

	workerthread=get_thread_from_queue(1);

	if ( workerthread ) {

    	    if ( workerthread->threadid>0 ) pthread_cancel(workerthread->threadid);
    	    workerthread->threadid=0;

	    free(workerthread);

	}

    }

}

void remove_all_workerthreads()
{

    logoutput("remove_all_workerthreads");

    while(1) {

	struct workerthread_struct *workerthread=get_thread_from_queue(1);

	if ( workerthread ) {

    	    if ( workerthread->threadid>0 ) pthread_cancel(workerthread->threadid);
    	    workerthread->threadid=0;

	    free(workerthread);

	} else {

	    break;

	}

    }

}


void start_workerthreads()
{

    add_workerthreads(NOTIFYFS_NUMBERTHREADS);

}
