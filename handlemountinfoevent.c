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

#define LOG_LOGAREA LOG_LOGAREA_MAINLOOP

#include "logging.h"
#include "epoll-utils.h"
#include "workerthreads.h"
#include "mountinfo.h"
#include "mountinfo-monitor.h"
#include "handlemountinfoevent.h"

static struct workerthreads_queue_struct *global_workerthreads_queue=NULL;

static void process_mountinfo(void *data)
{

    /* call the function handle_change_mounttable to read the changes */

    handle_change_mounttable();

}

/*
 * process an event the mountinfo
*/

int process_mountinfo_event(int fd, void *data, uint32_t events)
{
    int nreturn=0;

    if (fd>0) {
	struct workerthread_struct *workerthread=NULL;

	/* get a thread to do the work */

	workerthread=get_workerthread(global_workerthreads_queue);

	if ( ! workerthread ) {

	    logoutput( "process_fuse_event: unable to get a workerthread");
	    goto out;

	}

	/* assign the right callbacks and data */

	workerthread->processevent_cb=process_mountinfo;
	workerthread->data=NULL;

	/* send signal to start */

	signal_workerthread(workerthread);

    } else {

	/* called without a valid fd (at init) */

	process_mountinfo(NULL);

    }

    out:

    return nreturn;

}

void init_handlemountinfoevent(struct workerthreads_queue_struct *workerthreads_queue)
{

    global_workerthreads_queue=workerthreads_queue;

}
