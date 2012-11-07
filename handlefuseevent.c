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

#include <inttypes.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/mount.h>

#include <pthread.h>
#include <syslog.h>

#define LOGGING

#define LOG_LOGAREA LOG_LOGAREA_MAINLOOP

#include "logging.h"
#include "epoll-utils.h"
#include "workerthreads.h"
#include "handlefuseevent.h"

extern struct fuse_lowlevel_ops notifyfs_oper;
extern struct fuse_chan *fuse_kern_chan_new(int fd);
extern struct fuse_args global_fuse_args;

struct fusebuffer_struct {
    char *buff;
    size_t buffsize;
    int lenread;
    struct fusebuffer_struct *next;
};

static struct fusebuffer_struct *firstinqueue=NULL;
static struct fusebuffer_struct *lastinqueue=NULL;
static pthread_mutex_t queue_mutex=PTHREAD_MUTEX_INITIALIZER;
static int nrbuffers=0;

static struct fuse_session *session=NULL;
static struct fuse_chan *chan=NULL;
static int fuse_fd=0;
static struct epoll_extended_data_struct xdata_fuse=EPOLL_XDATA_INIT;
static unsigned char exitsession=0;
static unsigned char fuse_mounted=0;
static char *fuse_mountpoint=NULL;


int mount_notifyfs(char *mountpoint)
{
    const char *devname = "/dev/fuse";
    const char *fstype  = "fuse.notifyfs";
    const char *source  = "notifyfs";
    int fd, res, nreturn=0;
    char vfsdata[128];
    struct stat st;

    fd=open(devname, O_RDWR);

    if (fd == -1) {

	nreturn=-errno;
	goto out;

    }

    res=stat(mountpoint, &st);

    if (res==-1) {

	logoutput("mount_notifyfs: mountpoint does not exist/failed to access(error: %s)", strerror(errno));
	nreturn=-errno;
	goto out;

    }

    snprintf(vfsdata, 128,  "fd=%i,rootmode=%o,user_id=%i,group_id=%i,allow_other", fd, st.st_mode & S_IFMT, getuid(), getgid());

    logoutput("mount_notifyfs: mount %s %s %s %s", source, mountpoint, fstype, vfsdata);

    res=mount(source, mountpoint, fstype, MS_NODEV | MS_NOATIME | MS_NOSUID, vfsdata);

    if (res==-1) {

	nreturn=-errno;

    } else {

	nreturn=fd;

    }

    out:

    return nreturn;

}


/*  initialize a buffer to read the event available in the channel fd
    note the buffsize is normally very big, while the events here with
    notifyfs are simple commands (no writes)
    */

int init_fusebuffer(struct fusebuffer_struct *fusebuffer)
{
    int nreturn=0;
    size_t buffsize=fuse_chan_bufsize(chan);

    logoutput("init_fusebuffer: allocate memory size %zi", buffsize);

    fusebuffer->buff=malloc(buffsize);

    if ( fusebuffer->buff ) {

	memset(fusebuffer->buff, '\0', buffsize);
    	fusebuffer->buffsize=buffsize;
	fusebuffer->lenread=0;
	fusebuffer->next=NULL;

    } else {

	logoutput("init_fusebuffer: error allocate memory");
    	nreturn=-ENOMEM;

    }

    out:

    return nreturn;

}

struct fusebuffer_struct *get_fusebuffer()
{
    struct fusebuffer_struct *fusebuffer=NULL;

    pthread_mutex_lock(&queue_mutex);

    if ( firstinqueue ) {

	fusebuffer=firstinqueue;
	firstinqueue=fusebuffer->next;
	fusebuffer->next=NULL;

	if (lastinqueue==fusebuffer) lastinqueue=firstinqueue;

    } else {

	logoutput("get_fusebuffer: no free fuse buffer available: create one");

	fusebuffer=malloc(sizeof(struct fusebuffer_struct));

	if (fusebuffer) {

	    init_fusebuffer(fusebuffer);

	    if ( ! fusebuffer->buff ) {

		free(fusebuffer);
		fusebuffer=NULL;

		logoutput("get_fusebuffer: error creating a buffer");

	    } else {

		nrbuffers++;

		logoutput("get_fusebuffer: number buffers %i", nrbuffers);

	    }

	}

    }

    pthread_mutex_unlock(&queue_mutex);

    return fusebuffer;

}

void put_fusebuffer(struct fusebuffer_struct *fusebuffer)
{

    pthread_mutex_lock(&queue_mutex);

    fusebuffer->next=NULL;

    if ( lastinqueue ) lastinqueue->next=fusebuffer;
    lastinqueue=fusebuffer;

    if ( ! firstinqueue ) firstinqueue=fusebuffer;

    pthread_mutex_unlock(&queue_mutex);

}

void destroy_fusebuffer(struct fusebuffer_struct *fusebuffer)
{

    if (fusebuffer->buff) {

	free(fusebuffer->buff);
	fusebuffer->buff=NULL;

    }

    free(fusebuffer);

}

void destroy_fusebuffers()
{
    struct fusebuffer_struct *fusebuffer;

    pthread_mutex_lock(&queue_mutex);

    fusebuffer=firstinqueue;

    while(fusebuffer) {

	firstinqueue=fusebuffer->next;

	destroy_fusebuffer(fusebuffer);

	fusebuffer=firstinqueue;

    }

    pthread_mutex_unlock(&queue_mutex);

    lastinqueue=NULL;

}

/* function which reads data available on fuse chan (=fd) into a buffer 
*/

static int read_fuse_event(struct fusebuffer_struct *fusebuffer)
{
    int nreturn=0, res;
    unsigned char count=0;

    readbuff:

    res=fuse_chan_recv(&chan, fusebuffer->buff, fusebuffer->buffsize);

    if ( res==-EAGAIN ) {

        /* jump back... to prevent an endless loop build in a counter */

	logoutput("read_fuse_event: error EAGAIN");

	if (count<5) {

	    count++;
    	    goto readbuff;

	}

    } else if ( res==-EINTR ) {

	logoutput("read_fuse_event: error EINTR");
	nreturn=0;

    } else if ( res<=0 ) {

	logoutput("read_fuse_event: error %i", res);

	if (res==0) exitsession=1;
        nreturn=res;

    } else {

	/* no error */

	nreturn=res;
	fusebuffer->lenread=res;

    }

    out:

    return nreturn;

}

//
// a thread
// to process the fuse event
// it maybe a permanent or a temporary one
//

static void process_fusebuffer(void *data)
{
    struct fusebuffer_struct *fusebuffer=(struct fusebuffer_struct *) data;

    fuse_session_process(session, fusebuffer->buff, fusebuffer->lenread, chan);

    /* put fusebuffer back on queue */

    fusebuffer->lenread=0;
    put_fusebuffer(fusebuffer);

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

int process_fuse_event(int fd, void *data, uint32_t events)
{
    int nreturn=0, res;

    if ( events & (EPOLLERR | EPOLLHUP) ) {

        logoutput( "process_fuse_event: event %i causes exit", events);
        exitsession=1;

    } else if ( ! (events & EPOLLIN) ) {

	/* only react on incoming events */
	/* huh?? */

        logoutput( "process_fuse_event: fd %i not available for read", fd);

    } else {
	struct fusebuffer_struct *fusebuffer=NULL;
	int lenread;

	fusebuffer=get_fusebuffer();

	if ( ! fusebuffer) {

	    logoutput( "process_fuse_event: unable to allocate fusebuffer");
	    goto out;

	}

	/* read the buffer */

	lenread=read_fuse_event(fusebuffer);

	if (exitsession==1) {

	    fusebuffer->lenread=0;
	    put_fusebuffer(fusebuffer);
	    goto out;

	} else if (lenread>0) {
	    struct workerthread_struct *workerthread=NULL;

	    /* get a thread to do the work */

	    workerthread=get_thread_from_queue(0);

	    if ( ! workerthread ) {

		logoutput( "process_fuse_event: unable to get a workerthread");
		goto out;

	    }

	    /* assign the right callbacks and data */

	    workerthread->processevent_cb=process_fusebuffer;
	    workerthread->data=(void *) fusebuffer;

	    /* send signal to start */

	    signal_workerthread(workerthread);

	}

    }

    out:

    if ( exitsession==1 ) nreturn=EVENTLOOP_EXIT;

    return nreturn;

}


void finish_fuse()
{

    if (session) {

	fuse_session_destroy(session);
	session=NULL;
	chan=NULL;

    } else if (chan) {

	fuse_chan_destroy(chan);
	chan=NULL;

    }

    if (fuse_mounted==1) {

	fuse_unmount(fuse_mountpoint, chan);
	fuse_mounted=0;
	fuse_mountpoint=NULL;

    }

    destroy_fusebuffers();

}


int initialize_fuse(char *mountpoint)
{
    int nreturn=0;
    struct epoll_extended_data_struct *epoll_xdata;
    char *arg;
    int argc=0;

    logoutput("initialize_fuse: mountpoint %s", mountpoint);

    fuse_fd=mount_notifyfs(mountpoint);

    if (fuse_fd<=0) {

	logoutput("initialize_fuse: unable to mount mountpoint %s", mountpoint);
	nreturn=fuse_fd;
	goto error;

    }

    fuse_mounted=1;
    fuse_mountpoint=mountpoint;

    chan=fuse_kern_chan_new(fuse_fd);

    if ( ! chan ) {

	logoutput("initialize_fuse: unable to add fuse channel to mountpoint %s", mountpoint);
	nreturn=-EIO;
	goto error;

    } else {

	logoutput("initialize_fuse: added fuse channel to mountpoint %s", mountpoint);

    }

    arg=global_fuse_args.argv;

    while(1) {

	if (arg) {

	    logoutput("initialize_fuse: argument %s", arg);

	    argc++;
	    arg=global_fuse_args.argv[argc];

	} else {

	    break;

	}

    }

    session=fuse_lowlevel_new(&global_fuse_args, &notifyfs_oper, sizeof(notifyfs_oper), NULL);

    if ( ! session ) {

	logoutput("initialize_fuse: unable to add fuse session to mountpoint %s", mountpoint);
	nreturn=-EIO;
	goto error;

    } else {

	logoutput("initialize_fuse: added fuse session to mountpoint %s", mountpoint);

    }

    fuse_session_add_chan(session, chan);

    epoll_xdata=add_to_epoll(fuse_fd, EPOLLIN, TYPE_FD_FUSE, &process_fuse_event, NULL, &xdata_fuse, NULL);

    if ( ! epoll_xdata ) {

	logoutput("manage_mount: cannot add %s to eventloop", mountpoint);
	nreturn=-EIO;
	goto error;

    }

    add_xdata_to_list(epoll_xdata, NULL);

    return nreturn;

    error:

    finish_fuse();

    return nreturn;

}
