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
#include <errno.h>
#include <err.h>

#include <inttypes.h>
#include <ctype.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/param.h>
#include <sys/inotify.h>

#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_XATTR

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "notifyfs.h"
#include "entry-management.h"
#include "path-resolution.h"
#include "xattr.h"


extern struct notifyfs_options_struct notifyfs_options;

int setxattr4workspace(struct call_info_struct *call_info, const char *name, const char *value)
{
    int nvalue, nreturn=-ENOATTR;

    if ( isrootinode(call_info->entry->inode)==1 ) {

	// setting system values only on root entry

	if ( strcmp(name, "logging")==0 ) {

	    nvalue=atoi(value);

	    if ( nvalue>=0 ) {

		logoutput1("setxattr: value found %i", nvalue);

		loglevel=nvalue;
		nreturn=0;

	    } else {

		nreturn=-EINVAL;

	    }

            goto out;

	}

	if ( strcmp(name, "logarea")==0 ) {

	    nvalue=atoi(value);

	    if ( nvalue>=0 ) {

		logoutput1("setxattr: value found %i", nvalue);

		logarea=nvalue;
		nreturn=0;

	    } else {

		nreturn=-EINVAL;

	    }

            goto out;

	}

    }

    out:

    return nreturn;

}

static void fill_in_simpleinteger(struct xattr_workspace_struct *xattr_workspace, int somenumber)
{
    char smallstring[10];

    xattr_workspace->nlen=snprintf(smallstring, 9, "%i", somenumber);

    if ( xattr_workspace->size>0 ) {

	if ( xattr_workspace->size > xattr_workspace->nlen ) {

	    xattr_workspace->value=malloc(xattr_workspace->size);

	    if ( ! xattr_workspace->value ) {

		xattr_workspace->nerror=-ENOMEM;

	    } else {

		memcpy(xattr_workspace->value, &smallstring, xattr_workspace->nlen);
		*((char *) xattr_workspace->value+xattr_workspace->nlen) = '\0';

	    }

	}

    }

}


static void fill_in_simplestring(struct xattr_workspace_struct *xattr_workspace, char *somestring)
{
    xattr_workspace->nlen=strlen(somestring);

    if ( xattr_workspace->size>0 ) {

	if ( xattr_workspace->size > xattr_workspace->nlen ) {

	    xattr_workspace->value=malloc(xattr_workspace->size);

	    if ( ! xattr_workspace->value ) {

		xattr_workspace->nerror=-ENOMEM;

	    } else {

		memcpy(xattr_workspace->value, somestring, xattr_workspace->nlen);
		*((char *) xattr_workspace->value+xattr_workspace->nlen) = '\0';

	    }

	}

    }

}

void getxattr4workspace(struct call_info_struct *call_info, const char *name, struct xattr_workspace_struct *xattr_workspace)
{

    xattr_workspace->nerror=-ENOATTR; /* start with attr not found */


    logoutput2("getxattr4workspace, name: %s, size: %i", name, xattr_workspace->size);

    if ( isrootinode(call_info->entry->inode)==1 ) {

	// only the system related

	if ( strcmp(name, "logging")==0 ) {

            logoutput3("getxattr4workspace, found: logging");

	    xattr_workspace->nerror=0;

	    fill_in_simpleinteger(xattr_workspace, (int) loglevel);

            return;

	} else	if ( strcmp(name, "logarea")==0 ) {

            logoutput3("getxattr4workspace, found: logarea");

	    xattr_workspace->nerror=0;

	    fill_in_simpleinteger(xattr_workspace, (int) logarea);

            return;

	} else if ( strcmp(name, "socket")==0 ) {

            logoutput3("getxattr4workspace, found: socket");

	    xattr_workspace->nerror=0;

	    fill_in_simplestring(xattr_workspace, notifyfs_options.socket);

            return;

        } else if ( strcmp(name, "accessmode")==0 ) {

            logoutput3("getxattr4workspace, found: accessmode");

	    xattr_workspace->nerror=0;

	    fill_in_simpleinteger(xattr_workspace, notifyfs_options.accessmode);

            return;

        }

    }

}

//
// generalized utility to add a xattr name to list, used by listxattr4workspace
//

static int add_xattr_to_list(struct xattr_workspace_struct *xattr_workspace, char *list)
{
    unsigned nlenxattr=0;

    nlenxattr=strlen(xattr_workspace->name);

    // logoutput2("add_xattr_to_list, name : %s, size: %zd, len so far: %i", xattr_workspace->name, xattr_workspace->size, xattr_workspace->nlen);

    if ( xattr_workspace->size==0 ) {

	// just increase

	xattr_workspace->nlen+=nlenxattr+1;

    } else {

	// check the value fits (including the trailing \0)

	if ( xattr_workspace->nlen+nlenxattr+1 <= xattr_workspace->size ) {

	    memcpy(list+xattr_workspace->nlen, xattr_workspace->name, nlenxattr);

	    xattr_workspace->nlen+=nlenxattr;

	    *(list+xattr_workspace->nlen)='\0';

	    xattr_workspace->nlen+=1;

	} else {

	    // does not fit... return the size, calling proc will detect this is bigger than size

	    xattr_workspace->nlen+=nlenxattr+1;

	}

    }

    return xattr_workspace->nlen;

}


int listxattr4workspace(struct call_info_struct *call_info, char *list, size_t size)
{
    unsigned nlenlist=0;
    struct xattr_workspace_struct *xattr_workspace;


    logoutput2("listxattr4workspace");


    xattr_workspace=malloc(sizeof(struct xattr_workspace_struct));

    if ( ! xattr_workspace ) {

	nlenlist=-ENOMEM;
	goto out;

    }

    memset(xattr_workspace, 0, sizeof(struct xattr_workspace_struct));

    xattr_workspace->name=malloc(LINE_MAXLEN);

    if ( ! xattr_workspace->name ) {

	nlenlist=-ENOMEM;
	free(xattr_workspace);
	goto out;

    }

    xattr_workspace->size=size;
    xattr_workspace->nerror=0;
    xattr_workspace->value=NULL;
    xattr_workspace->nlen=0;

    if ( ! list ) size=0;


    // system related attributes, only available on root

    if ( isrootinode(call_info->entry->inode)==1 ) {

	// level of logging

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_logging", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

	// area to log

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_logarea", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

	// socket

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_socket", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

	// accessmode

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_accessmode", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

    }

    out:

    return nlenlist;

}
