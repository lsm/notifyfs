/*
  2010, 2011 Stef Bon <stefbon@gmail.com>

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
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>
#include <sys/time.h>
#include <assert.h>
#include <syslog.h>
#include <time.h>

#include <inttypes.h>

// required??

#include <ctype.h>

#include <sys/types.h>
#include <sys/inotify.h>
#include <pthread.h>


#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

#include "logging.h"
#include "epoll-utils.h"
#include "message.h"
#include "client-io.h"

extern unsigned char loglevel;
extern int logarea;
extern int socket_fd;

char sendbuffer[NOTIFYFS_CLIENT_SENDBUFFERSIZE];

/* send a list command to the server 

*/

void send_list_message(int fd, char *path, char *name, int maxentries, unsigned char typeentry)
{
    struct notifyfs_message_body message;
    int lenpath;

    message.type=NOTIFYFS_MESSAGE_TYPE_LIST;

    /* fill the buffer with path and name (of entry ) */

    lenpath=strlen(path);
    memcpy(sendbuffer, path, lenpath);

    *(sendbuffer+lenpath)='\0';

    if (name) {
	int lenname=strlen(name);

	memcpy(sendbuffer+lenpath+1, name, lenname);

	*(sendbuffer+lenpath+1+lenname)='\0';

    } else {

	*(sendbuffer+lenpath+1)='\0';

    }

    message.body.list_message.client_watch_id=0;
    message.body.list_message.maxentries=maxentries;
    message.body.list_message.typeentry=typeentry;
    message.body.list_message.unique=new_uniquectr();

    logoutput("send_list_message: ctr %li", message.body.list_message.unique);

    send_message(fd, &message, sendbuffer, NOTIFYFS_CLIENT_SENDBUFFERSIZE);

}

int show_menu()
{
    char c;
    int res;

    while (1) {

	fprintf(stdout, "\n");
	fprintf(stdout, "Choices:\n");
	fprintf(stdout, "1. list by path\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "0. exit\n");

	fprintf(stdout, "\n");
	fprintf(stdout, "Choice: ");

	res=scanf("%s", &c);

	if (res==1) {

	    if ( strncmp(&c, "1", 1)==0 ) {
		char path[PATH_MAX];
		char name[255];
		int maxentries=0;
		unsigned char typeentry=0;

		fprintf(stdout, "1\n");
		fprintf(stdout, "\n");

		/* ask for path */

		fprintf(stdout, "Send a list command by path.\n");
		fprintf(stdout, "path : ");

		memset(path, '\0', PATH_MAX);

		res=scanf("%s", path);

		if ( res<0 ) {

		    fprintf(stderr, "\n");
		    fprintf(stderr, "Error %i reading input.\n", res);

		    break;

		}

		fprintf(stdout, "\n");

		fprintf(stdout, "from name : ");

		memset(name, '\0', 255);

		res=scanf("%s", name);

		if ( res<0 ) {

		    fprintf(stderr, "\n");
		    fprintf(stderr, "Error %i reading input.\n", res);

		    break;

		}

		fprintf(stdout, "\n");

		fprintf(stdout, "max nr entries : ");

		res=scanf("%i", &maxentries);

		if ( res<0 ) {

		    fprintf(stderr, "\n");
		    fprintf(stderr, "Error %i reading input.\n", res);
		    break;

		}

		fprintf(stdout, "\n");

		fprintf(stdout, "type entry(directory=1/other=2) : ");

		res=scanf("%i", &typeentry);

		if ( res<0 ) {

		    fprintf(stderr, "\n");
		    fprintf(stderr, "Error %i reading input.\n", res);
		    break;

		}


		fprintf(stdout, "Sending a message.\n");

		send_list_message(socket_fd, path, name, maxentries, typeentry);

	    } else if ( strncmp(&c, "0", 1)==0 ) {

		return -1;

	    }

	} else {

	    if ( errno!=0 ) {

		fprintf(stderr, "\n");
		fprintf(stderr, "Error %i reading input.\n", errno);

	    }

	    break;

	}

    }

    return 0;

}

int start_showmenu_thread(pthread_t *pthreadid)
{
    int nreturn=0;

    nreturn=pthread_create(pthreadid, NULL, (void *) &show_menu, NULL);

    if ( nreturn==-1 ) {

        nreturn=-errno;

	logoutput("Error creating a thread to show menu (error: %i).", abs(nreturn));


    }

    return nreturn;

}



