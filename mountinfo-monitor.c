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
#include "mountinfo.h"
#include "mountinfo-monitor.h"

/* private structs for internal use only */

struct mountinfo_entry_struct {
    int mountid;
    int parentid;
    struct mountinfo_entry_struct *next;
    struct mountinfo_entry_struct *prev;
    struct mountinfo_entry_struct *s_next;
    struct mountinfo_entry_struct *s_prev;
    struct mount_entry_struct *mount_entry;
};

struct mountinfo_list_struct {
    struct mountinfo_entry_struct *first;
    struct mountinfo_entry_struct *last;
    struct mountinfo_entry_struct *s_first;
    struct mountinfo_entry_struct *s_last;
};

struct mountinfo_list_struct new_mountinfo_list={NULL, NULL, NULL, NULL};
struct mountinfo_list_struct current_mountinfo_list={NULL, NULL, NULL, NULL};

/* internal lists to keep unused entries */

struct mountinfo_entry_struct *mountinfo_list_unused=NULL;

/* various locks for lists */

pthread_mutex_t current_mounts_mutex=PTHREAD_MUTEX_INITIALIZER;

/* variable, mutex and condition to monitor changes */

pthread_mutex_t mountinfo_changed_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t mountinfo_changed_cond=PTHREAD_COND_INITIALIZER;
unsigned char mountinfo_changed=0;

extern struct mount_list_struct added_mounts;
extern struct mount_list_struct removed_mounts;
extern struct mount_list_struct removed_mounts_keep;

static unsigned char firstrun=1;

static void init_mountinfo_entry(struct mountinfo_entry_struct *mountinfo_entry)
{

    mountinfo_entry->mount_entry=NULL;

    mountinfo_entry->mountid=0;
    mountinfo_entry->parentid=0;

    mountinfo_entry->next=NULL;
    mountinfo_entry->prev=NULL;
    mountinfo_entry->s_next=NULL;
    mountinfo_entry->s_prev=NULL;


}

static struct mountinfo_entry_struct *create_mountinfo_entry()
{
    struct mountinfo_entry_struct *mountinfo_entry;

    mountinfo_entry=malloc(sizeof(struct mountinfo_entry_struct));

    return mountinfo_entry;

}

/* note no lock required, there is one thread (main) calling this..*/

static struct mountinfo_entry_struct *get_mountinfo_entry()
{
    struct mountinfo_entry_struct *mountinfo_entry;

    if ( mountinfo_list_unused ) {

        // get from list

        mountinfo_entry=mountinfo_list_unused;
        mountinfo_list_unused=mountinfo_entry->next;

    } else {

        mountinfo_entry=create_mountinfo_entry();

    }

    if ( mountinfo_entry ) init_mountinfo_entry(mountinfo_entry);

    return mountinfo_entry;
}


static void move_to_unused_list_mountinfo(struct mountinfo_entry_struct *mountinfo_entry)
{

    mountinfo_entry->next=NULL;
    mountinfo_entry->prev=NULL;
    mountinfo_entry->s_prev=NULL;
    mountinfo_entry->s_next=NULL;

    mountinfo_entry->next=mountinfo_list_unused;
    mountinfo_list_unused=mountinfo_entry;

}

/* various utilities to get mount_entries from various lists */

struct mount_entry_struct *next_mount_entry_current(struct mount_entry_struct *mount_entry, int direction, unsigned char type)
{
    struct mount_entry_struct *mount_entry_next=NULL;

    if ( direction!=1 && direction!=-1 ) goto out;
    if ( type!=MOUNTENTRY_CURRENT && type!=MOUNTENTRY_CURRENT_SORTED ) goto out;

    if ( ! mount_entry ) {

    	if ( type==MOUNTENTRY_CURRENT ) {

	    if ( direction>0 ) {
		struct mountinfo_entry_struct *mountinfo_entry=current_mountinfo_list.first;

        	if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

	    } else if ( direction<0 ) {
		struct mountinfo_entry_struct *mountinfo_entry=current_mountinfo_list.last;

        	if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

	    }

	} else if ( type==MOUNTENTRY_CURRENT_SORTED ) {

	    if ( direction>0 ) {
		struct mountinfo_entry_struct *mountinfo_entry=current_mountinfo_list.s_first;

        	if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

    	    } else if ( direction<0 ) {
		struct mountinfo_entry_struct *mountinfo_entry=current_mountinfo_list.s_last;

        	if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

	    }

	}

    } else {
	struct mountinfo_entry_struct *mountinfo_entry=(struct mountinfo_entry_struct *) mount_entry->index;

	if ( ! mountinfo_entry ) goto out;

        if ( type==MOUNTENTRY_CURRENT ) {

	    /* take the list as it's read from mountinfo */

	    if ( direction>0 ) {

        	mountinfo_entry=mountinfo_entry->next;

	    } else if ( direction<0 ) {

		mountinfo_entry=mountinfo_entry->prev;

	    }

        } else if ( type==MOUNTENTRY_CURRENT_SORTED ) {

	    /* take the sorted list */

	    if ( direction>0 ) {

        	mountinfo_entry=mountinfo_entry->s_next;

	    } else if ( direction<0 ) {

		mountinfo_entry=mountinfo_entry->s_prev;

	    }

        }

	if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

    }

    out:

    return mount_entry_next;

}


/* this is the function which reads the mountinfo file after a change has been detected
   it reads line for line, and for every line adds a mountinfo_entry and a mount_entry*/


int get_new_mount_list(struct mountinfo_list_struct *mi_list)
{
    char line[PATH_MAX];
    FILE *fp;
    int mountid, parentid, major, minor;
    char encoded_rootpath[PATH_MAX], encoded_mountpoint[PATH_MAX];
    char fstype[64], mountsource[64], superoptions[256];
    char *mountpoint, *sep, *rootpath;
    struct mount_entry_struct *mount_entry;
    struct mountinfo_entry_struct *mi_entry, *mi_entry_prev;
    int nreturn=0, difference;

    logoutput("get_new_mount_list");

    fp=fopen(MOUNTINFO_FILE, "r");

    if ( fp ) {

	while ( ! feof(fp) ) {

            memset(line, '\0', PATH_MAX);

	    if ( fgets(line, PATH_MAX, fp)==NULL ) continue;

            if ( strlen(line)==0 ) continue;

	    if ( sscanf(line, "%i %i %i:%i %s %s", &mountid, &parentid, &major, &minor, encoded_rootpath, encoded_mountpoint) != 6 ) {

                printf("error sscanf\n");
		continue;

	    }

            mountpoint=g_strcompress(encoded_mountpoint);
            rootpath=g_strcompress(encoded_rootpath);

            sep=strstr(line, " - ");

            if ( ! sep ) continue;

            if ( sscanf(sep+3, "%s %s %s", fstype, mountsource, superoptions) != 3 ) continue;


            /* get a new mountinfo_entry */

            mi_entry=get_mountinfo_entry();

            if ( ! mi_entry ) continue;

            mi_entry->mountid=mountid;
            mi_entry->parentid=parentid;

            /* add mountinfo_entry to list */

            mi_entry->next=NULL;
            mi_entry->prev=NULL;

            if ( ! mi_list->first ) mi_list->first=mi_entry;

            if ( ! mi_list->last ) {

                mi_list->last=mi_entry;

            } else {

                mi_list->last->next=mi_entry;
                mi_entry->prev=mi_list->last;
                mi_list->last=mi_entry;

            }

            /* get a new mount_entry */

            mount_entry=get_mount_entry();

            if ( ! mount_entry ) continue;

            mi_entry->mount_entry=mount_entry;
            mount_entry->index=(void *) mi_entry;

            mount_entry->mountpoint=mountpoint;
            mount_entry->rootpath=rootpath;
            strcpy(mount_entry->fstype, fstype);
            strcpy(mount_entry->mountsource, mountsource);
            strcpy(mount_entry->superoptions, superoptions);
            mount_entry->major=major;
            mount_entry->minor=minor;

            /* when not / this is the source of the bind mount */

            if ( strcmp(rootpath, "/") != 0 ) mount_entry->isbind=1;

            if ( strcmp(fstype, "autofs")==0 ) {

                mount_entry->isautofs=1;
                mount_entry->autofs_indirect=0;

                if ( strstr(superoptions, "indirect") ) mount_entry->autofs_indirect=1;

            }

            if ( parentid==1 ) {

                /* this mount_entry is always the first in mountinfo file
                   there are other ways possible to do this, but it works */

		if ( ! rootmount_isset() ) set_rootmount(mount_entry);
                mount_entry->isroot=1;

            }

            /* insert in sorted by using bubblesort (=walking back, compare and move if necessary) */

            if ( ! mi_list->s_first ) mi_list->s_first=mi_entry;

            if ( ! mi_list->s_last ) {

                mi_list->s_last=mi_entry;

            } else {

                /* walk back starting at the last and compare */

                mi_entry_prev=mi_list->s_last;

                while(1) {

                    difference=compare_mount_entries(mi_entry_prev->mount_entry, mi_entry->mount_entry);

                    if ( difference>0 ) {

                        if ( mi_entry_prev->s_prev ) {

                            mi_entry_prev=mi_entry_prev->s_prev;

                        } else {

                            /* there is no prev: at the first */

                            break;

                        }

                    } else {

                        /* not bigger */

                        break;

                    }

                }

                if ( difference>0 ) {

                    /* at the first (and still bigger): insert at begin */

                    mi_entry->s_next=mi_entry_prev;
                    mi_entry_prev->s_prev=mi_entry;

                    mi_entry->s_prev=NULL;
                    mi_list->s_first=mi_entry;

                } else {

                    if ( mi_entry_prev!=mi_list->s_last) {

                        /* insert mount_entry AFTER mi_entry_prev */

                        mi_entry->s_prev=mi_entry_prev;
                        mi_entry->s_next=mi_entry_prev->s_next;

                        mi_entry_prev->s_next->s_prev=mi_entry;
                        mi_entry_prev->s_next=mi_entry;

                    } else {

                        /* no bigger: just add */

                        mi_entry->s_prev=mi_list->s_last;
                        mi_list->s_last->s_next=mi_entry;

                        mi_entry->s_next=NULL;
                        mi_list->s_last=mi_entry;

                    }

                }

            }

        }

        fclose(fp);

    }

    return nreturn;

}

/* function to set various variables when a mount is added:
   - parent
   - unique ctr
   - is autofs mounted, and if so, it's direct or indirect
*/

void set_attributes(struct mount_list_struct *mount_list)
{
    struct mountinfo_entry_struct *mi_entry, *mi_entry_tmp;
    struct mount_entry_struct *mount_entry;

    if ( ! mount_list ) mount_list=&added_mounts;

    mount_entry=mount_list->first;

    while(mount_entry) {

	logoutput("set_attributes: mount_entry %s", mount_entry->mountpoint);

	/* when dealing with a remount attributes are already set */

	// if (mount_entry->remount==1) goto next;

        mount_entry->parent=NULL;

	/* look for parent, take the normal index, this is the same as in mountinfo, cause
           there the parent is always before this entry */

        if ( is_rootmount(mount_entry)==0 ) {

	    mi_entry=(struct mountinfo_entry_struct *) mount_entry->index;

            mi_entry_tmp=mi_entry->prev;

            while(mi_entry_tmp) {

                if (mi_entry_tmp->mountid==mi_entry->parentid) {

                    mount_entry->parent=mi_entry_tmp->mount_entry;

		    logoutput("set parent: parent of %s is %s", mount_entry->mountpoint, mount_entry->parent->mountpoint);

                    break;

                } else if (mi_entry_tmp->parentid==mi_entry->parentid) {

		    if ( mi_entry_tmp->mount_entry->parent ) {

                	mount_entry->parent=mi_entry_tmp->mount_entry->parent;

			logoutput("set parent: parent of %s is %s", mount_entry->mountpoint, mount_entry->parent->mountpoint);

			break;

		    }

                }

                mi_entry_tmp=mi_entry_tmp->prev;

            }

	    if ( ! mount_entry->parent ) mount_entry->parent=get_rootmount();

        }

	mount_entry->autofs_mounted=0;

        if ( mount_entry->parent ) {

            /* test it's mounted by the autofs */

            if ( mount_entry->parent->isautofs==1 && mount_entry->isautofs==0 ) mounted_by_autofs(mount_entry);

        }

	if ( mount_entry->autofs_mounted==1 ) {

	    logoutput("mount_entry %s is mounted by autofs", mount_entry->mountpoint);

	}

	/* set unique id */

	if ( mount_entry->unique==0 ) mount_entry->unique=get_uniquectr();

	next:

        mount_entry=mount_entry->next;

    }

}


/* function which is called after a change is notified on the mountinfo "file"
  the purpose is to find out what has changed

  the output is returned via the parameters *mounts, *added_mounts and *removed_mounts:
. *mounts point to the current mount entries, where the next is pointed to by the next/prev fields
. *added_mounts point to the added mount entries, which is a subset of the current mount entries list
  the next in this list is pointed to by the changed_next/changed_prev fields
. *removed_mounts point to the removed mount entries, which is a subset of the previes mount entries list
  also here the next and prev in this group are pointed to by the changed_next/changed_prev fields

*/

static void handle_change_mounttable()
{
    struct mount_entry_struct *mount_entry;
    struct mountinfo_entry_struct *mi_entry, *mi_entry_2remove, *mi_entry_new, *mi_entry_tmp1, *mi_entry_tmp2;
    int nreturn=0, res;

    /* start loop to wait for a change */

    while(1) {

        res=pthread_mutex_lock(&mountinfo_changed_mutex);

        while(mountinfo_changed==0) {

            res=pthread_cond_wait(&mountinfo_changed_cond, &mountinfo_changed_mutex);

        }

        mountinfo_changed=0;
	increase_generation_id();

        res=pthread_mutex_unlock(&mountinfo_changed_mutex);

        logoutput("handle_change_mounttable");

        /* set locks to protect the mount list and changed and removed */

	res=lock_mountlist();

        /* clean old list removed mounts: move to unused */

        mount_entry=removed_mounts.first;

        while ( mount_entry ) {

            removed_mounts.first=mount_entry->next;
            move_to_unused_list_mount(mount_entry);
            mount_entry=removed_mounts.first;

        }

        removed_mounts.first=NULL;
        removed_mounts.last=NULL;

        /* clean current changes, like added */

        mi_entry=current_mountinfo_list.first;

        while ( mi_entry ) {

            mount_entry=mi_entry->mount_entry;

            if ( mount_entry ) {

                mount_entry->next=NULL;
                mount_entry->prev=NULL;

		mount_entry->status=MOUNT_STATUS_UP;

            }

            mi_entry=mi_entry->next;

        }

        added_mounts.first=NULL;
        added_mounts.last=NULL;

        /* get a new list and compare that with the old one */

        nreturn=get_new_mount_list(&new_mountinfo_list);

        if ( nreturn==0 ) {

            /* walk through both lists and notice the differences */

            mi_entry=current_mountinfo_list.s_first;
            mi_entry_new=new_mountinfo_list.s_first;

            while (1) {

                mi_entry_2remove=NULL;

                if ( mi_entry_new && mi_entry ) {

                    res=compare_mount_entries(mi_entry->mount_entry, mi_entry_new->mount_entry);

                } else if ( mi_entry_new && ! mi_entry ) {

                    res=1;

                } else if ( ! mi_entry_new && mi_entry ) {

                    res=-1;

                } else {

		    /* no more entries on both lists */

                    break;

                }

                if ( res==0 ) {

                    /* the same:
                       - keep it and move mount_entry from current list to new list */

                    mi_entry_tmp1=mi_entry_new->s_next;
                    mi_entry_tmp2=mi_entry->s_next;

		    /* forget the mount entry on the new list */

                    mount_entry=mi_entry_new->mount_entry;
                    mount_entry->index=NULL;
                    move_to_unused_list_mount(mount_entry);

		    /* move mount entry from current to new list */

                    mount_entry=mi_entry->mount_entry;
                    mount_entry->index=(struct mountinfo_entry_struct *) mi_entry_new;
                    mi_entry_new->mount_entry=mount_entry;
                    mi_entry->mount_entry=NULL;

		    /* update the generation */

		    mount_entry->generation=generation_id();

                    /* step in both lists */

                    mi_entry_2remove=mi_entry; /* mi_entry from current list is of no use anymore */

                    mi_entry_new=mi_entry_tmp1;
                    mi_entry=mi_entry_tmp2;

                } else if ( res<0 ) {

                    /* current is "smaller" then new : means removed :
                       - insert in circular list of removed (at tail!) */

                    mi_entry_tmp2=mi_entry->s_next;

                    mount_entry=mi_entry->mount_entry;

                    if ( mount_entry->autofs_mounted==0 ) {

                        add_mount_to_list(&removed_mounts, mount_entry);
                        mount_entry->status=MOUNT_STATUS_REMOVE;

                    } else {

                        add_mount_to_list(&removed_mounts_keep, mount_entry);
                        mount_entry->status=MOUNT_STATUS_SLEEP;

                    }

                    mi_entry->mount_entry=NULL;
                    mount_entry->index=NULL;
                    mount_entry->remount=0;
                    mount_entry->processed=0;

                    mi_entry_2remove=mi_entry;

                    /* step in current list */

                    mi_entry=mi_entry_tmp2;


                } else if (res>0 ) {

                    /* new is "smaller" then current : means added
                       - insert in circular list of added (at tail!) */

                    mi_entry_tmp1=mi_entry_new->s_next;

                    mount_entry=mi_entry_new->mount_entry;

                    add_mount_to_list(&added_mounts, mount_entry);
                    mount_entry->status=MOUNT_STATUS_UP;

		    mount_entry->generation=generation_id();

                    /* step in new list */

                    mi_entry_new=mi_entry_tmp1;

                }

                /* move mountinfo_entry to unused */

                if ( mi_entry_2remove ) move_to_unused_list_mountinfo(mi_entry_2remove);

            }


            /* here: current_mountinfo_list should be empty */

            current_mountinfo_list.first=new_mountinfo_list.first;
            current_mountinfo_list.last=new_mountinfo_list.last;
            current_mountinfo_list.s_first=new_mountinfo_list.s_first;
            current_mountinfo_list.s_last=new_mountinfo_list.s_last;

            new_mountinfo_list.first=NULL;
            new_mountinfo_list.last=NULL;
            new_mountinfo_list.s_first=NULL;
            new_mountinfo_list.s_last=NULL;

        }

        /* here replace the added mounts by those found in removed_mounts_keep */

        if ( added_mounts.first && removed_mounts_keep.first ) {
            struct mount_entry_struct *mount_entry_new, *me_tmp1, *me_tmp2;

            mount_entry_new=added_mounts.first;
            mount_entry=removed_mounts_keep.first;

            while(mount_entry_new && mount_entry) {

                if ( mount_entry_new->autofs_mounted==0 ) {

                    /* not an fs mounted by autofs */

                    mount_entry_new=mount_entry_new->next;
                    continue;

                }

                res=compare_mount_entries(mount_entry, mount_entry_new);

                if ( res==0 ) {

                    /* the same:
                       move from removed_mounts_keep to added_mounts */

                    me_tmp1=mount_entry->next;
                    me_tmp2=mount_entry_new->next;

                    remove_mount_from_list(&removed_mounts_keep, mount_entry);

                    mount_entry->prev=mount_entry_new->prev;
                    mount_entry->next=mount_entry_new->next;

                    mount_entry->index=mount_entry_new->index;

		    mi_entry=(struct mountinfo_entry_struct *) mount_entry->index;
                    mi_entry->mount_entry=mount_entry;

                    mount_entry->remount=1;			/* dealing with a remount */
                    mount_entry->processed=0;
                    mount_entry->status=MOUNT_STATUS_UP; 	/* mount is up again */

		    mount_entry->generation=mount_entry_new->generation;

                    if ( added_mounts.first==mount_entry_new ) added_mounts.first=mount_entry;
                    if ( added_mounts.last==mount_entry_new ) added_mounts.last=mount_entry;

                    move_to_unused_list_mount(mount_entry_new);

                    /* increase both */

                    mount_entry_new=me_tmp2;
                    mount_entry=me_tmp1;


                } else if ( res<0 ) {

                    /* increase in removed_mounts_keep */

                    mount_entry=mount_entry->next;

                } else if ( res>0 ) {

                    /* increase in added_mounts: a really added mount, not a remount */

                    mount_entry_new=mount_entry_new->next;

                }

            }

        }

        /* set the parents: only required for the new ones
           test it's mounted by autofs
           and set the unique ctr */

        set_attributes(&added_mounts);

        res=unlock_mountlist();

	//update_notifyfs(&added_mounts, &removed_mounts, &removed_mounts_keep);

	run_callback_onupdate(firstrun);

	firstrun=0;

    }

}

int start_mountmonitor(pthread_t *pthreadid, void *callback_onupdate)
{
    int nreturn=0;

    /* assign the callback to walk through the current mounts */

    register_mountinfo_callback(MOUNTINFO_CB_NEXT_IN_CURRENT, &next_mount_entry_current);
    register_mountinfo_callback(MOUNTINFO_CB_ONUPDATE, callback_onupdate);


    nreturn=pthread_create(pthreadid, NULL, (void *) &handle_change_mounttable, NULL);

    if ( nreturn==-1 ) {

        nreturn=-errno;

	logoutput("Error creating a thread to monitor mounts (error: %i).", abs(nreturn));


    }

    return nreturn;

}

void signal_mountmonitor()
{
    int res;

    res=pthread_mutex_lock(&mountinfo_changed_mutex);

    mountinfo_changed=1;

    /* pthread_cond_signal is also possible here */

    res=pthread_cond_broadcast(&mountinfo_changed_cond);
    res=pthread_mutex_unlock(&mountinfo_changed_mutex);

}
