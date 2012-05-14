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

#include "global-defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <sys/epoll.h>
#include <sys/types.h>
#include <errno.h>
#include <stdbool.h>
#include <strings.h>

#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <pthread.h>

#include <glib.h>

#define LOG_LOGAREA LOG_LOGAREA_MOUNTMONITOR

#include "logging.h"
#include "epoll-utils.h"
#include "mountinfo.h"
#include "utils.h"

/* internal lists to keep unused entries */

struct mount_entry_struct *mount_list_unused=NULL;

/* added, removed and removed but to keep mount entries */

struct mount_list_struct added_mounts={NULL, NULL};
struct mount_list_struct removed_mounts={NULL, NULL};
struct mount_list_struct removed_mounts_keep={NULL, NULL};

/* one global lock */

pthread_mutex_t mounts_mutex=PTHREAD_MUTEX_INITIALIZER;

/* callbacks */

struct mountinfo_cb_struct mountinfo_cb={
                                         .onupdate=NULL,
                                         .next_in_current=NULL,
                                         .next_in_changed=&next_mount_entry_changed, 
                                         .lock=&lock_mountlist,
                                         .unlock=&unlock_mountlist};

struct mount_entry_struct *rootmount=NULL;

static unsigned char firstrun=1;
static unsigned long long uniquectr=0; /* no need to lock, there is one thread handling this */
static unsigned long long generation=0;

void register_mountinfo_callback(unsigned char type, void *callback)
{

    if ( type==MOUNTINFO_CB_ONUPDATE ) {

	mountinfo_cb.onupdate=callback;

    } else if ( type==MOUNTINFO_CB_NEXT_IN_CURRENT ) {

	mountinfo_cb.next_in_current=callback;

    } else if ( type==MOUNTINFO_CB_NEXT_IN_CHANGED ) {

	mountinfo_cb.next_in_changed=callback;

    } else if ( type==MOUNTINFO_CB_LOCK ) {

	mountinfo_cb.lock=callback;

    } else if ( type==MOUNTINFO_CB_UNLOCK ) {

	mountinfo_cb.unlock=callback;

    }

}

void run_callback_onupdate(unsigned char firstrun)
{

    if ( mountinfo_cb.onupdate ) {

	logoutput("run_callback_onupdate: callback defined");

	(*mountinfo_cb.onupdate) (firstrun);

    } else {

	logoutput("run_callback_onupdate: callback not defined");

    }

    logoutput("run_callback_onupdate: ready");

}


unsigned long long get_uniquectr()
{
    return uniquectr++;
}

unsigned long long generation_id()
{
    return generation;
}

void increase_generation_id()
{
    generation++;
}


/* compare entries 
* if b is "bigger" then a this function returns a positive value (+1)
* if a is "bigger" then it returns -1
* if equal: 0

  the first item to compare: mountpoint
  the second               : mountsource
  the third                : filesystem
*/

int compare_mount_entries(struct mount_entry_struct *a, struct mount_entry_struct *b)
{
    int nreturn=0;

    nreturn=g_strcmp0(a->mountpoint, b->mountpoint);

    if ( nreturn==0 ) {

        nreturn=g_strcmp0(a->mountsource, b->mountsource);

        if ( nreturn==0 ) nreturn=g_strcmp0(a->fstype, b->fstype);

    }

    return nreturn;

}

void init_mount_entry(struct mount_entry_struct *mount_entry)
{
    mount_entry->mountpoint=NULL;
    memset(mount_entry->fstype, '\0', 64);
    memset(mount_entry->mountsource, '\0', 64);
    memset(mount_entry->superoptions, '\0', 256);
    mount_entry->rootpath=NULL;

    mount_entry->index=NULL;

    mount_entry->isroot=0;
    mount_entry->isbind=0;
    mount_entry->major=0;
    mount_entry->minor=0;
    mount_entry->isautofs=0;
    mount_entry->autofs_indirect=0;
    mount_entry->autofs_mounted=0;
    mount_entry->status=0;
    mount_entry->remount=0;
    mount_entry->processed=0;

    mount_entry->unique=0;
    mount_entry->generation=0;

    mount_entry->next=NULL;
    mount_entry->prev=NULL;
    mount_entry->parent=NULL;

    mount_entry->data0=NULL;
    mount_entry->data1=NULL;

}

struct mount_entry_struct *create_mount_entry()
{
    struct mount_entry_struct *mount_entry;

    mount_entry=malloc(sizeof(struct mount_entry_struct));

    return mount_entry;

}

/* note no lock required, there is one thread calling this..*/

struct mount_entry_struct *get_mount_entry()
{
    struct mount_entry_struct *mount_entry;

    if ( mount_list_unused ) {

        // get from list

        mount_entry=mount_list_unused;
        mount_list_unused=mount_entry->next;

    } else {

        mount_entry=create_mount_entry();

    }

    if ( mount_entry ) init_mount_entry(mount_entry);

    return mount_entry;
}

void move_to_unused_list_mount(struct mount_entry_struct *mount_entry)
{

    if (mount_entry->mountpoint) {

        free(mount_entry->mountpoint);
        mount_entry->mountpoint=NULL;

    }

    if (mount_entry->rootpath) {

        free(mount_entry->rootpath);
        mount_entry->rootpath=NULL;

    }

    mount_entry->prev=NULL;
    mount_entry->next=mount_list_unused;
    mount_list_unused=mount_entry;

}

/* various utilities to get mount_entries from various lists */

struct mount_entry_struct *next_mount_entry_changed(struct mount_entry_struct *mount_entry, int direction, unsigned char type)
{
    struct mount_entry_struct *mount_entry_next=NULL;

    if ( ! mount_entry ) {

        if ( type==MOUNTENTRY_ADDED ) {

	    if ( direction==1 ) {

        	mount_entry_next=added_mounts.first;

	    } else {

		mount_entry_next=added_mounts.last;

	    }

        } else if ( type==MOUNTENTRY_REMOVED ) {

	    if ( direction==1 ) {

        	mount_entry_next=removed_mounts.first;

	    } else {

		mount_entry_next=removed_mounts.last;

	    }

        } else if ( type==MOUNTENTRY_REMOVED_KEEP ) {

	    if ( direction==1 ) {

        	mount_entry_next=removed_mounts_keep.first;

	    } else {

        	mount_entry_next=removed_mounts_keep.last;

	    }

        }

    } else {

        if ( type==MOUNTENTRY_ADDED || type==MOUNTENTRY_REMOVED || type==MOUNTENTRY_REMOVED_KEEP ) {

	    if ( direction==1 ) {

        	mount_entry_next=mount_entry->next;

	    } else {

		mount_entry_next=mount_entry->prev;

	    }

        }

    }

    return mount_entry_next;

}

struct mount_entry_struct *get_next_mount_entry(struct mount_entry_struct *mount_entry, int direction, unsigned char type)
{

    if ( type==MOUNTENTRY_ADDED || type==MOUNTENTRY_REMOVED || type==MOUNTENTRY_REMOVED_KEEP ) {

	mount_entry=next_mount_entry_changed(mount_entry, direction, type);

    } else if ( type==MOUNTENTRY_CURRENT || type==MOUNTENTRY_CURRENT_SORTED ) {

	if ( mountinfo_cb.next_in_current ) {

	    mount_entry=(* mountinfo_cb.next_in_current) (mount_entry, direction, type);

	} else {

	    mount_entry=NULL;

	}

    }

    return mount_entry;

}


int lock_mountlist()
{

    return pthread_mutex_lock(&mounts_mutex);

}

int unlock_mountlist()
{

    return pthread_mutex_unlock(&mounts_mutex);

}

/* adding and removing to/from a list */

void add_mount_to_list(struct mount_list_struct *mount_list, struct mount_entry_struct *mount_entry)
{

    if ( ! mount_list->first ) mount_list->first=mount_entry;

    if ( ! mount_list->last ) {

        mount_list->last=mount_entry;

    } else {

        mount_list->last->next=mount_entry;
        mount_entry->prev=mount_list->last;
        mount_list->last=mount_entry;

    }

}

void remove_mount_from_list(struct mount_list_struct *mount_list, struct mount_entry_struct *mount_entry)
{
    if ( mount_entry->prev ) mount_entry->prev->next=mount_entry->next;
    if ( mount_entry->next ) mount_entry->next->prev=mount_entry->prev;

    if ( mount_entry==mount_list->last ) mount_list->last=mount_entry->prev;
    if ( mount_entry==mount_list->first ) mount_list->first=NULL;

}


/* */

void test_mounted_by_autofs(struct mount_entry_struct *mount_entry)
{

    logoutput(" path %s", mount_entry->mountpoint);

    /* direct or indirect ??*/

    if ( mount_entry->parent->autofs_indirect==1 ) {

        /* indirect */

        /* only mounted by autofs if the mountpath's do not differ too much: only one subdir */

	if ( issubdirectory(mount_entry->mountpoint, mount_entry->parent->mountpoint, 0)>0 ) {
	    char *slashfound=strrchr(mount_entry->mountpoint, '/');

            if ( slashfound ) {

                if ( slashfound==mount_entry->mountpoint+strlen(mount_entry->parent->mountpoint) ) {

                    /* only if this mountdirectory is a subdirectory (and not deeper!) of the autofs managed mountpoint */

                    logoutput("test_mounted_by_autofs: parent is autofs/indirect");

                    mount_entry->autofs_mounted=1;

                }

            }

        }

    } else {

        /* direct */

        if ( issubdirectory(mount_entry->mountpoint, mount_entry->parent->mountpoint, 1)==1 ) {

	    logoutput("test_mounted_by_autofs: parent is autofs/direct");

            mount_entry->autofs_mounted=1;

        }

    }

}

unsigned char mount_is_up(struct mount_entry_struct *mount_entry)
{
    return ( mount_entry->status==MOUNT_STATUS_UP ) ? 1 : 0;
}

unsigned char mounted_by_autofs(struct mount_entry_struct *mount_entry)
{
    return ( mount_entry->autofs_mounted==1 ) ? 1 : 0;
}

void set_rootmount(struct mount_entry_struct *mount_entry)
{
    rootmount=mount_entry;
}

unsigned char is_rootmount(struct mount_entry_struct *mount_entry)
{
    if (rootmount) return (rootmount==mount_entry) ? 1 : 0;

    return 0;
}

struct mount_entry_struct *get_rootmount()
{
    return rootmount;
}

unsigned char rootmount_isset()
{
    return (rootmount) ? 1 : 0;
}

/* function which returns the mount entry where the entry pointed to by path
   is part of */

struct mount_entry_struct *get_mount(char *path)
{
    int res=0;
    struct mount_entry_struct *mount_entry=NULL, *me_loop;

    res=lock_mountlist();

    mount_entry=get_rootmount();

    /* check trivial case */

    if ( strcmp(path, "/")==0 ) goto unlock;

    /* the callback to walk through current mounts sorted should be set */

    if ( ! mountinfo_cb.next_in_current ) goto unlock;

    me_loop=(* mountinfo_cb.next_in_current) (NULL, 1, MOUNTENTRY_CURRENT_SORTED);

    while (me_loop) {

	if ( strcmp(me_loop->mountpoint, "/")==0 ) {

	    /* everything is a subdirectory of root... it will mess things up
               ignore it */

	    goto next;

	}

	if ( g_strcmp0(me_loop->mountpoint, path)>0 ) {

	    /* mountpoint is bigger: no chance path is a subdirectory AND
               the rest of the list - since it's sorted this way - also,
               so it's safe to stop here..*/

	    break;

	}

	/* test path is a subdirectory of mountpoint (and the maybe the same) */

	res=issubdirectory(path, me_loop->mountpoint, 1);

	if ( res==1 ) {

	    /* paths are the same, take that one */

	    mount_entry=me_loop;
	    break;

	} else if ( res==2 ) {

	    /* subdirectory, the best match so far, so take that one, but not stop
               there maybe better still  */

	    mount_entry=me_loop;

	}

	next:

	me_loop=(* mountinfo_cb.next_in_current) (me_loop, 1, MOUNTENTRY_CURRENT_SORTED);;

    }

    unlock:

    res=unlock_mountlist();

    return mount_entry;

}

void logoutput_list(unsigned char type, unsigned char lockset)
{
    int res=0;
    struct mount_entry_struct *mount_entry=NULL;

    if (lockset==0) res=lock_mountlist();

    if ( res==0 ) {

        mount_entry=get_next_mount_entry(NULL, 1, type);

        if ( mount_entry ) {

            if ( type==MOUNTENTRY_CURRENT ) {

                logoutput("Current mounts:");

            } else if ( type==MOUNTENTRY_ADDED ) {

                logoutput("Added mounts:");

            } else if ( type==MOUNTENTRY_REMOVED ) {

                logoutput("Removed mounts:");

            } else if ( type==MOUNTENTRY_REMOVED_KEEP ) {

                logoutput("Removed mounts kept:");

            } else if ( type==MOUNTENTRY_CURRENT_SORTED ) {

                logoutput("Current mounts sorted:");

            }


        } else {

            if ( type==MOUNTENTRY_CURRENT ) {

                logoutput("No current mounts.");

            } else if ( type==MOUNTENTRY_ADDED ) {

                logoutput("No added mounts.");

            } else if ( type==MOUNTENTRY_REMOVED ) {

                logoutput("No removed mounts.");

            } else if ( type==MOUNTENTRY_REMOVED_KEEP ) {

                logoutput("No removed(kept) mounts.");

            } else if ( type==MOUNTENTRY_CURRENT_SORTED ) {

                logoutput("No current mounts sorted.");

            } 

        }

        while (mount_entry) {

            if ( mount_entry->isbind==1 ) {

                logoutput("(bind) %s on %s type %s", mount_entry->rootpath, mount_entry->mountpoint, mount_entry->fstype);

            } else if ( mount_entry->autofs_mounted==1 ) {

                logoutput("(mounted by autofs) %s on %s type %s", mount_entry->mountsource, mount_entry->mountpoint, mount_entry->fstype);

            } else {

                logoutput("%s on %s type %s", mount_entry->mountsource, mount_entry->mountpoint, mount_entry->fstype);

            }

            mount_entry=get_next_mount_entry(mount_entry, 1, type);

        }

    }

    if (lockset==0) res=unlock_mountlist(type);

}
