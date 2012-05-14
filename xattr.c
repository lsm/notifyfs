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
#include "client.h"
#include "entry-management.h"
#include "path-resolution.h"
#include "mountinfo.h"
#include "watches.h"
#include "changestate.h"
#include "xattr.h"


extern struct notifyfs_options_struct notifyfs_options;

int setwatch_testmode(struct call_info_struct *call_info, const char *name, const char *value)
{
    struct effective_watch_struct *effective_watch;
    int res=0, nreturn=0, nvalue;
    int oldmask=0, newmask=0;
    struct notifyfs_inode_struct *inode;

    /* in testmode the value is an integer: the new mask */

    nvalue=atoi(value);

    if ( nvalue<0 ) {

        logoutput1("setxattr: invalid value %i", nvalue);
        nreturn=-EINVAL;
        goto out;

    }

    logoutput("setxattr: value found %i", nvalue);

    inode=call_info->entry->inode;

    /* check a watch is already present for this client 
           somehow lock the watches set in this inode ... */

    logoutput("setxattr: looking for effective_watch");

    effective_watch=inode->effective_watch;

    if ( ! effective_watch ) {

        if ( nvalue>0 ) {

            effective_watch=get_effective_watch();

            if ( ! effective_watch ) {

                nreturn=-ENOMEM;
                goto out;

            }

	    effective_watch->id=new_watchid();

            effective_watch->inode=inode;
            inode->effective_watch=effective_watch;

            add_effective_watch_to_list(effective_watch);

	    res=set_mount_entry_effective_watch(call_info, effective_watch);

        } else {

            /* no watches, and set to zero: no action */

            nreturn=-EINVAL;
            goto out;

        }

    }

    /* lock the effective watch */

    res=lock_effective_watch(effective_watch);

    logoutput("setxattr: testmode: setting the watch");

    /* when backend not set get it */

    if ( effective_watch->typebackend==FSEVENT_BACKEND_METHOD_NOTSET ) {

        set_backend(call_info, effective_watch);

        logoutput("setxattr: setting backend to %i", effective_watch->typebackend);

    }

    oldmask=effective_watch->mask;
    newmask=nvalue;

    if ( newmask==0 ) {

	/* here deal with newmask==0 means a remove */

	del_watch_backend(effective_watch);

    } else {

        if ( newmask != oldmask ) {

	    effective_watch->mask=newmask;

	    if ( effective_watch->inode->status==FSEVENT_INODE_STATUS_OK ) {

    		set_watch_backend(effective_watch, newmask, 1);

	    }

	}

    }

    unlockwatch:

    res=unlock_effective_watch(effective_watch);

    out:

    return nreturn;


}

int setwatch_client(struct call_info_struct *call_info, const char *name, const char *value)
{
    struct effective_watch_struct *effective_watch;
    struct watch_struct *watch;
    int res=0, nreturn;
    int oldmask=0, newmask=0, client_watch_id, client_watch_mask;
    struct notifyfs_inode_struct *inode;
    struct client_struct *client=NULL;
    char *separator;
    char maskvalue[10], idvalue[10];

    client=lookup_client(call_info->ctx->pid, 0);

    /* only clients can set a mask */

    if ( ! client ) {

        nreturn=-EACCES;
        goto out;

    }

    call_info->client=client;

    /* the value is like "client_watch_id:client_mask" */

    separator=strchr(value, ':');

    if ( ! separator ) {

	/* no separator */
	/* always require a separator, also when it's an update/remove when theoratically it's doesn't
           have to */

	nreturn=-EINVAL;
	goto out;

    }

    if ( separator-value>10 ) {

	/* first field too long */

	nreturn=-EINVAL;
	goto out;

    }

    if ( value + strlen(value) - separator > 10 ) {

	/* second field too long */

	nreturn=-EINVAL;
	goto out;

    }

    memcpy(idvalue, value, separator-value);
    memcpy(maskvalue, separator+1, value+strlen(value)-separator-1);

    client_watch_id=atoi(idvalue);
    client_watch_mask=atoi(maskvalue);

    if ( client_watch_id<0 ) {

        logoutput1("setxattr: invalid client id %i", client_watch_id);
        nreturn=-EINVAL;
        goto out;

    }

    if ( client_watch_mask<0 ) {

        logoutput1("setxattr: invalid new mask %i", client_watch_mask);
        nreturn=-EINVAL;
        goto out;

    }

    logoutput("setxattr: mask found %i for client id %i", client_watch_mask, client_watch_id);

    res=lock_client(client);
    if ( client->status_app!=NOTIFYFS_CLIENTSTATUS_UP ) goto unlockclient;


    /* 	check a watch is already present for this client 
	somehow lock the watches set in this inode ... */


    logoutput("setxattr: looking for effective_watch");

    inode=call_info->entry->inode;
    effective_watch=inode->effective_watch;

    if ( ! effective_watch ) {

        if ( client_watch_mask>0 ) {

            effective_watch=get_effective_watch();

            if ( ! effective_watch ) {

                nreturn=-ENOMEM;
                goto unlockclient;

            }

	    effective_watch->id=new_watchid();

            effective_watch->inode=inode;
            inode->effective_watch=effective_watch;

            add_effective_watch_to_list(effective_watch);

	    res=set_mount_entry_effective_watch(call_info, effective_watch);

    	} else {

            /* no watches, and set to zero: no action */

            nreturn=-EINVAL;
            goto unlockclient;

        }

    }

    /* lock the effective watch */

    res=lock_effective_watch(effective_watch);

    /* after here: there is an effective watch, and is locked by this thread */

    watch=effective_watch->watches;

    while (watch) {

        if (watch->client==client) break;
        watch=watch->next_per_watch;

    }

    if ( ! watch ) {

        if ( client_watch_mask==0 ) {

            /* no action: client has no watch set here  */

            logoutput("setxattr: no action: no watch found..");
            nreturn=-EINVAL;
            goto unlockwatch;

        } else {

            /* create a new watch for inode/client */

            logoutput("setxattr: no watch found for this client.... creating one ");

            nreturn=add_new_client_watch(effective_watch, client_watch_mask, client_watch_id, client);

	    if ( nreturn<0 ) goto unlockwatch;

            /* recalculate the effective mask */

            oldmask=effective_watch->mask;
            newmask=calculate_effmask(effective_watch, 1);

            /* add events to monitor the move and delete actions */

            newmask|=(IN_DELETE_SELF | IN_MOVE_SELF);

            if ( effective_watch->typebackend==FSEVENT_BACKEND_METHOD_NOTSET ) {

                set_backend(call_info, effective_watch);

            }

            if ( newmask != oldmask ) {

		effective_watch->mask=newmask;

		if ( effective_watch->inode->status==FSEVENT_INODE_STATUS_OK ) {

        	    set_watch_backend(effective_watch, newmask, 1);

		}

	    }

        }

    } else { /* watch found */

        if ( client_watch_mask==0 ) {

            /* remove watch */
            /* from effective watch */

            logoutput("setxattr: watch found, removing it");
            nreturn=0;

	    remove_client_watch_from_inode(watch);

            oldmask=effective_watch->mask;

            if ( effective_watch->nrwatches>0 ) {

                newmask=calculate_effmask(effective_watch, 1);

                /* add events to monitor the move and delete actions of watch self */

                newmask|=(IN_DELETE_SELF | IN_MOVE_SELF);

		if ( newmask != oldmask ) {

		    effective_watch->mask=newmask;

		    if ( effective_watch->inode->status==FSEVENT_INODE_STATUS_OK ) {

        		set_watch_backend(effective_watch, newmask, 1);

		    }

		}

            } else {

		/* when there are no watches any more */

                effective_watch->mask=0;
                newmask=0;

		del_watch_backend(effective_watch);

	    }

            /* from client */

	    remove_client_watch_from_client(watch);

            free(watch);


        } else {

            /* watch exists and new mask>0 */

            logoutput("setxattr: watch found, changing the mask");
            nreturn=0;

            if ( watch->mask!=client_watch_mask ) {

                /* only action when the new value is different from the old one */

                watch->mask=client_watch_mask;
                oldmask=effective_watch->mask;

                newmask=calculate_effmask(effective_watch, 1);

                /* add events to monitor the move and delete actions */

                newmask|=(IN_DELETE_SELF | IN_MOVE_SELF);

		if ( oldmask!=newmask ) {

		    effective_watch->mask=newmask;

		    if ( effective_watch->inode->status==FSEVENT_INODE_STATUS_OK ) {

			set_watch_backend(effective_watch, newmask, 1);

		    }

		}

            } else {

                logoutput("setxattr: no action, new mask %i is the same as the current", client_watch_mask);

            }

        }

	nreturn=0;

    }


    unlockwatch:

    if ( effective_watch->nrwatches==0 ) {

	/* remove the watch */

	if (effective_watch->inode) {

	    effective_watch->inode->effective_watch=NULL;
	    effective_watch->inode=NULL;

	}

	if ( effective_watch->path ) {

	    free(effective_watch->path);
	    effective_watch->path=NULL;

	}

	remove_effective_watch_from_list(effective_watch, 0);
	move_effective_watch_to_unused(effective_watch);

    }

    res=unlock_effective_watch(effective_watch);

    unlockclient:

    res=unlock_client(client);

    out:

    return nreturn;

}



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

	if ( strcmp(name, "filesystems")==0 ) {

	    nvalue=atoi(value);

	    if ( nvalue>=0 ) {

		logoutput1("setxattr: value found %i", nvalue);

		notifyfs_options.filesystems=nvalue;
		nreturn=0;

	    } else {

		nreturn=-EINVAL;

	    }

            goto out;

	}

    }

    /* set a mask 
       client is identified by the pid of the calling context (ctx->pid)
       depends on the accessmode access is permitted or denied
       posssible to remove a mask by setting value to 0

       question: possible to allow the setting of the mask not only by clients

    */

    if ( strcmp(name, "mask")==0 ) {

        if ( notifyfs_options.testmode==1 ) {

	    nreturn=setwatch_testmode(call_info, name, value);

	} else {

	    nreturn=setwatch_client(call_info, name, value);

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

        } else if ( strcmp(name, "filesystems")==0 ) {

            logoutput3("getxattr4workspace, found: filesystems");

	    xattr_workspace->nerror=0;

	    fill_in_simpleinteger(xattr_workspace, notifyfs_options.filesystems);

            return;

        } 

    }

    if ( strcmp(name, "effmask")==0 ) {

        /* effective mask: the "total" mask */

        logoutput3("getxattr4workspace, found: effmask");

        xattr_workspace->nerror=0;

        if ( call_info->entry->inode->effective_watch ) {

            fill_in_simpleinteger(xattr_workspace, call_info->entry->inode->effective_watch->mask);

        } else {

            fill_in_simpleinteger(xattr_workspace, 0);

        }

        return;

    } else if ( strcmp(name, "client_watch_mask")==0 ) {
        struct client_struct *client=lookup_client(call_info->ctx->pid, 0);

        logoutput3("getxattr4workspace, found: client_watch_mask");

        if ( client ) {
            int client_watch_mask=0;

            /* mask: the mask of the watch per connected client */

            xattr_workspace->nerror=0;

            if ( call_info->entry->inode->effective_watch ) {

                client_watch_mask=get_clientmask(call_info->entry->inode->effective_watch, client->pid, 0);

            }

            fill_in_simpleinteger(xattr_workspace, client_watch_mask);

        }

        return;

    } else if ( strcmp(name, "client_watch_id")==0 ) {
        struct client_struct *client=lookup_client(call_info->ctx->pid, 0);

        logoutput3("getxattr4workspace, found: client_watch_id");

        if ( client ) {
            int client_watch_id=0;

            /* id: the id of the watch per connected client */

            xattr_workspace->nerror=0;

            if ( call_info->entry->inode->effective_watch ) {

                client_watch_id=get_clientid(call_info->entry->inode->effective_watch, client->pid, 0);

            }

            fill_in_simpleinteger(xattr_workspace, client_watch_id);

        }

        return;

    } else if ( strcmp(name, "nrwatches")==0 ) {

        logoutput3("getxattr4workspace, found: nrwatches");

        xattr_workspace->nerror=0;

        if ( call_info->entry->inode->effective_watch ) {

            fill_in_simpleinteger(xattr_workspace, call_info->entry->inode->effective_watch->nrwatches);

        } else {

            fill_in_simpleinteger(xattr_workspace, 0);

        }

        return;

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
    struct client_struct *client=lookup_client(call_info->ctx->pid, 0);


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

	// filesystems

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_filesystems", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

	// accessmode

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_accessmode", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

    }

    // effmask

    memset(xattr_workspace->name, '\0', LINE_MAXLEN);
    snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_effmask", XATTR_SYSTEM_NAME);

    nlenlist=add_xattr_to_list(xattr_workspace, list);
    if ( size > 0 && nlenlist > size ) goto out;

    // nrwatches

    memset(xattr_workspace->name, '\0', LINE_MAXLEN);
    snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_nrwatches", XATTR_SYSTEM_NAME);

    nlenlist=add_xattr_to_list(xattr_workspace, list);
    if ( size > 0 && nlenlist > size ) goto out;

    if ( client ) {

	// mask

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_client_watch_mask", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

	// client_watch_id

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_client_watch_id", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;



    }

    out:

    return nlenlist;

}
