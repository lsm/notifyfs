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

#include <glib.h>
#include <mntent.h>

#define LOG_LOGAREA LOG_LOGAREA_MOUNTMONITOR

#include "logging.h"
#include "mountinfo.h"

struct mountinfo_list_struct new_mountinfo_list;
struct mountinfo_list_struct current_mountinfo_list;

struct mountinfo_entry_struct *mountinfo_list_unused=NULL;
struct mount_entry_struct *mount_list_unused=NULL;

struct mount_list_struct added_mounts;
struct mount_list_struct removed_mounts;
struct mount_list_struct removed_mounts_keep;

pthread_mutex_t current_mounts_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t changed_mounts_mutex=PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t mountinfo_changed_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t mountinfo_changed_cond=PTHREAD_COND_INITIALIZER;
unsigned char mountinfo_changed=0;

pthread_mutex_t fstab_changed_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t fstab_changed_cond=PTHREAD_COND_INITIALIZER;
unsigned char fstab_changed=0;

struct fstab_list_struct *fstab_list_first=NULL;
struct fstab_list_struct *fstab_list_last=NULL;

struct fstab_list_struct *fstab_list_unused=NULL;

struct mount_entry_struct *root_mount=NULL;
extern struct notifyfs_entry_struct *root_entry;



/*
* compare entries 
* if b is "bigger" then a this function returns a positive value (+1)
* if a is "bigger" then it returns -1
* if equal: 0
*/

static int compare_entries(struct mount_entry_struct *a, struct mount_entry_struct *b)
{
    int nreturn=0;

    nreturn=g_strcmp0(a->mountpoint, b->mountpoint);

    if ( nreturn==0 ) {

        nreturn=g_strcmp0(a->mountsource, b->mountsource);

        if ( nreturn==0 ) nreturn=g_strcmp0(a->fstype, b->fstype);

    }

    return nreturn;

}

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

static void init_mount_entry(struct mount_entry_struct *mount_entry)
{
    mount_entry->mountpoint=NULL;
    memset(mount_entry->fstype, '\0', 64);
    memset(mount_entry->mountsource, '\0', 64);
    memset(mount_entry->superoptions, '\0', 256);
    mount_entry->rootpath=NULL;

    mount_entry->mountinfo_entry=NULL;

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

    mount_entry->next=NULL;
    mount_entry->prev=NULL;
    mount_entry->parent=NULL;

    mount_entry->entry=NULL;
    mount_entry->client=NULL;
}

static struct mount_entry_struct *create_mount_entry()
{
    struct mount_entry_struct *mount_entry;

    mount_entry=malloc(sizeof(struct mount_entry_struct));

    return mount_entry;

}

/* note no lock required, there is one thread calling this..*/

static struct mount_entry_struct *get_mount_entry()
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

static void move_to_unused_list_mount(struct mount_entry_struct *mount_entry)
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

struct mount_entry_struct *get_next_mount_entry(struct mount_entry_struct *mount_entry, unsigned char type)
{
    struct mount_entry_struct *mount_entry_next=NULL;

    if ( ! mount_entry ) {

        if ( type==MOUNTENTRY_CURRENT ) {
            struct mountinfo_entry_struct *mountinfo_entry=current_mountinfo_list.first;

            if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

        } else if ( type==MOUNTENTRY_NEW ) {
            struct mountinfo_entry_struct *mountinfo_entry=new_mountinfo_list.first;

            if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

        } else if ( type==MOUNTENTRY_ADDED ) {

            mount_entry_next=added_mounts.first;

        } else if ( type==MOUNTENTRY_REMOVED ) {

            mount_entry_next=removed_mounts.first;

        } else if ( type==MOUNTENTRY_REMOVED_KEEP ) {

            mount_entry_next=removed_mounts_keep.first;

        } else if ( type==MOUNTENTRY_CURRENT_SORTED ) {
            struct mountinfo_entry_struct *mountinfo_entry=current_mountinfo_list.s_first;

            if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

        } else if ( type==MOUNTENTRY_NEW_SORTED ) {
            struct mountinfo_entry_struct *mountinfo_entry=new_mountinfo_list.s_first;

            if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

        } 


    } else {

        if ( type==MOUNTENTRY_CURRENT || type==MOUNTENTRY_NEW ) {
            struct mountinfo_entry_struct *mountinfo_entry=NULL;

            mountinfo_entry=mount_entry->mountinfo_entry->next;

            if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

        } else if ( type==MOUNTENTRY_CURRENT_SORTED || type==MOUNTENTRY_NEW_SORTED ) {
            struct mountinfo_entry_struct *mountinfo_entry=NULL;

            mountinfo_entry=mount_entry->mountinfo_entry->s_next;

            if ( mountinfo_entry ) mount_entry_next=mountinfo_entry->mount_entry;

        } else if ( type==MOUNTENTRY_ADDED || type==MOUNTENTRY_REMOVED || type==MOUNTENTRY_REMOVED_KEEP ) {

            mount_entry_next=mount_entry->next;

        }

    }

    return mount_entry_next;

}


int lock_mountlist(unsigned char type)
{
    int res=0;

    if ( type==MOUNTENTRY_REMOVED || type==MOUNTENTRY_ADDED || type==MOUNTENTRY_REMOVED_KEEP ) {

        res=pthread_mutex_lock(&changed_mounts_mutex);

    } else if ( type==MOUNTENTRY_CURRENT || type==MOUNTENTRY_NEW || type==MOUNTENTRY_CURRENT_SORTED || type==MOUNTENTRY_NEW_SORTED ) {

        res=pthread_mutex_lock(&current_mounts_mutex);

    }

    return res;
}

int unlock_mountlist(unsigned char type)
{
    int res=0;

    if ( type==MOUNTENTRY_REMOVED || type==MOUNTENTRY_ADDED || type==MOUNTENTRY_REMOVED_KEEP ) {

        res=pthread_mutex_unlock(&changed_mounts_mutex);

    } else if ( type==MOUNTENTRY_CURRENT || type==MOUNTENTRY_NEW || type==MOUNTENTRY_CURRENT_SORTED || type==MOUNTENTRY_NEW_SORTED) {

        res=pthread_mutex_unlock(&current_mounts_mutex);

    }

    return res;
}

void logoutput_list(unsigned char type, unsigned char lockset)
{
    int res=0;
    struct mount_entry_struct *mount_entry=NULL;

    if (lockset==0) res=lock_mountlist(type);

    if ( res==0 ) {

        mount_entry=get_next_mount_entry(NULL, type);

        if ( mount_entry ) {

            if ( type==MOUNTENTRY_CURRENT ) {

                logoutput("Current mounts:");

            } else if ( type==MOUNTENTRY_NEW ) {

                logoutput("New mounts:");

            } else if ( type==MOUNTENTRY_ADDED ) {

                logoutput("Added mounts:");

            } else if ( type==MOUNTENTRY_REMOVED ) {

                logoutput("Removed mounts:");

            } else if ( type==MOUNTENTRY_REMOVED_KEEP ) {

                logoutput("Removed mounts kept:");

            } else if ( type==MOUNTENTRY_CURRENT_SORTED ) {

                logoutput("Current mounts sorted:");

            } else if ( type==MOUNTENTRY_NEW_SORTED ) {

                logoutput("New mounts sorted:");

            }


        } else {

            if ( type==MOUNTENTRY_CURRENT ) {

                logoutput("No current mounts.");

            } else if ( type==MOUNTENTRY_NEW ) {

                logoutput("No new mounts.");

            } else if ( type==MOUNTENTRY_ADDED ) {

                logoutput("No added mounts.");

            } else if ( type==MOUNTENTRY_REMOVED ) {

                logoutput("No removed mounts.");

            } else if ( type==MOUNTENTRY_REMOVED_KEEP ) {

                logoutput("No removed(kept) mounts.");

            } else if ( type==MOUNTENTRY_CURRENT_SORTED ) {

                logoutput("No current mounts sorted.");

            } else if ( type==MOUNTENTRY_NEW_SORTED ) {

                logoutput("No new mounts sorted.");

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

            mount_entry=get_next_mount_entry(mount_entry, type);

        }

    }

    if (lockset==0) res=unlock_mountlist(type);

}

struct mount_entry_struct *get_root_mount()
{
    struct mount_entry_struct *mount_entry=NULL;
    int res;

    res=lock_mountlist(MOUNTENTRY_CURRENT);

    mount_entry=get_next_mount_entry(NULL, MOUNTENTRY_CURRENT);

    res=unlock_mountlist(MOUNTENTRY_CURRENT);

    return mount_entry;
}

/* adding and removing to/from a list */

static void add_mount_to_list(struct mount_list_struct *mount_list, struct mount_entry_struct *mount_entry)
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

static void remove_mount_from_list(struct mount_list_struct *mount_list, struct mount_entry_struct *mount_entry)
{
    if ( mount_entry->prev ) mount_entry->prev->next=mount_entry->next;
    if ( mount_entry->next ) mount_entry->next->prev=mount_entry->prev;

    if ( mount_entry==mount_list->last ) mount_list->last=mount_entry->prev;
    if ( mount_entry==mount_list->first ) mount_list->first=NULL;

}



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

    fp=fopen(MOUNTINFO, "r");

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
            mount_entry->mountinfo_entry=mi_entry;

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
                   so there are other ways possible, but it works */

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

                    difference=compare_entries(mi_entry_prev->mount_entry, mi_entry->mount_entry);

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

    logoutput("get_new_mount_list return: %i", nreturn);

    return nreturn;

}

void test_mounted_by_autofs(struct mount_entry_struct *mount_entry)
{

    /* direct or indirect */

    if ( mount_entry->parent->autofs_indirect==1 ) {
        int nlen1=0, nlen2=0;
        const char *mountpoint1, *mountpoint2;

        /* indirect */

        /* only mounted by autofs if the mountpath's do not differ too much: only one subdir */

        mountpoint1=(const char *)mount_entry->parent->mountpoint;
        mountpoint2=(const char *)mount_entry->mountpoint;

        nlen1=strlen(mountpoint1);
        nlen2=strlen(mountpoint2);

        if ( nlen2>nlen1 ) {

            if ( strncmp(mountpoint2, mountpoint1, nlen1)==0 ) {

                if ( strncmp(mountpoint2+nlen1, "/", 1)==0 ) {

                    char *slashfound=strrchr(mountpoint2, '/');

                    if ( slashfound ) {

                        if ( slashfound==mountpoint2+nlen1 ) {

                            /* only if this mountdirectory is a subdirectory (and not deeper!) of the autofs managed mountpoint */

                            mount_entry->autofs_mounted=1;

                        }

                    }

                }

            }

        }

    } else {
        const char *mountpoint1, *mountpoint2;

        /* direct */

        mountpoint1=(const char *)mount_entry->parent->mountpoint;
        mountpoint2=(const char *)mount_entry->mountpoint;

        if ( strcmp(mountpoint1, mountpoint2)==0 ) {

            mount_entry->autofs_mounted=1;

        }

    }

}

void set_parents_raw(struct mountinfo_list_struct *mi_list)
{
    struct mountinfo_entry_struct *mi_entry, *mi_entry_tmp;
    struct mount_entry_struct *mount_entry;

    if ( ! mi_list ) mi_list=&current_mountinfo_list;

    mi_entry=mi_list->first;

    if ( mi_entry ) {

        mi_entry=mi_entry->next;

        while (mi_entry) {

            mount_entry=mi_entry->mount_entry;
            mount_entry->parent=NULL;
            mi_entry_tmp=mi_entry->prev;

            while (mi_entry_tmp) {

                if ( mi_entry_tmp->mountid==mi_entry->parentid ) {

                    mount_entry->parent=mi_entry_tmp->mount_entry;

                    break;

                } else if ( mi_entry_tmp->parentid==mi_entry->parentid ) {

                    mount_entry->parent=mi_entry_tmp->mount_entry->parent;

                    break;

                }

                mi_entry_tmp=mi_entry_tmp->prev;

            }

            if ( mount_entry->parent ) {

                /* test it's mounted by the autofs */

                mount_entry->autofs_mounted=0;

                if ( mount_entry->parent->isautofs==1 && mount_entry->isautofs==0 ) test_mounted_by_autofs(mount_entry);

            }

            mi_entry=mi_entry->next;

        }

    }

}

void set_parents_added(struct mount_list_struct *mount_list)
{
    struct mountinfo_entry_struct *mi_entry, *mi_entry_tmp;
    struct mount_entry_struct *mount_entry;

    if ( ! mount_list ) mount_list=&added_mounts;

    mount_entry=mount_list->first;

    while(mount_entry) {

        mi_entry=mount_entry->mountinfo_entry;
        mount_entry->parent=NULL;

        if ( mi_entry ) {

            mi_entry_tmp=mi_entry->prev;

            while(mi_entry_tmp) {

                if (mi_entry_tmp->mountid==mi_entry->parentid) {

                    mount_entry->parent=mi_entry_tmp->mount_entry;

                    break;

                } else if (mi_entry_tmp->parentid==mi_entry->parentid) {

                    mount_entry->parent=mi_entry_tmp->mount_entry->parent;

                    break;

                }

                mi_entry_tmp=mi_entry_tmp->prev;

            }

        }

        if ( mount_entry->parent ) {

            /* test it's mounted by the autofs */

            mount_entry->autofs_mounted=0;

            if ( mount_entry->parent->isautofs==1 && mount_entry->isautofs==0 ) test_mounted_by_autofs(mount_entry);

        }

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

static void *handle_change_mounttable()
{
    struct mount_entry_struct *mount_entry;
    struct mountinfo_entry_struct *mi_entry, *mi_entry_2remove, *mi_entry_new, *mi_entry_tmp1, *mi_entry_tmp2;
    int nreturn=0, res;
    unsigned char doinit=0;

    /* start loop to wait for a change */

    while(1) {

        res=pthread_mutex_lock(&mountinfo_changed_mutex);

        while(mountinfo_changed==0) {

            res=pthread_cond_wait(&mountinfo_changed_cond, &mountinfo_changed_mutex);

	    if ( mountinfo_changed==2 ) doinit=1;

        }

        mountinfo_changed=0;

        res=pthread_mutex_unlock(&mountinfo_changed_mutex);

        logoutput("handle_change_mounttable");

        /* set locks to protect the mount list and changed and removed */

        res=pthread_mutex_lock(&current_mounts_mutex);
        res=pthread_mutex_lock(&changed_mounts_mutex);

        /* clean old list removed mounts: move to unused */

        mount_entry=removed_mounts.first;

        if ( mount_entry ) {

            while ( mount_entry ) {

                removed_mounts.first=mount_entry->next;
                move_to_unused_list_mount(mount_entry);
                mount_entry=removed_mounts.first;

            }

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

        /* from here the current list should be in current_mountinfo_list and 
           new_mountinfo_list should be empty */

        // logoutput_list(MOUNTENTRY_CURRENT, 1);
        // logoutput_list(MOUNTENTRY_CURRENT_SORTED, 1);

        /* get a new list and compare that with the old one */

        nreturn=get_new_mount_list(&new_mountinfo_list);

        // logoutput_list(MOUNTENTRY_NEW, 1);
        // logoutput_list(MOUNTENTRY_NEW_SORTED, 1);


        if ( nreturn==0 ) {

            /* walk through both lists and notice the differences */

            mi_entry=current_mountinfo_list.s_first;
            mi_entry_new=new_mountinfo_list.s_first;

            while (1) {

                mi_entry_2remove=NULL;

                if ( mi_entry_new && mi_entry ) {

                    res=compare_entries(mi_entry->mount_entry, mi_entry_new->mount_entry);

                    // logoutput("comparing (res:%i)mount %s and %s", res, mi_entry->mount_entry->mountpoint, mi_entry_new->mount_entry->mountpoint);

                } else if ( mi_entry_new && ! mi_entry ) {

                    // logoutput("found new mount %s", mi_entry_new->mount_entry->mountpoint);

                    res=1; 

                } else if ( ! mi_entry_new && mi_entry ) {

                    // logoutput("found current mount %s", mi_entry->mount_entry->mountpoint);

                    res=-1;

                } else {

                    // logoutput("no mounts anymore to compare");

                    break;

                }

                if ( res==0 ) {

                    /* the same:
                       - move mount_entry from current list to new list */

                    mi_entry_tmp1=mi_entry_new->s_next;
                    mi_entry_tmp2=mi_entry->s_next;

                    mount_entry=mi_entry_new->mount_entry;
                    mount_entry->mountinfo_entry=NULL;
                    move_to_unused_list_mount(mount_entry);

                    mount_entry=mi_entry->mount_entry;
                    mount_entry->mountinfo_entry=mi_entry_new;
                    mi_entry_new->mount_entry=mount_entry;
                    mi_entry->mount_entry=NULL;

                    /* step in both lists */

                    mi_entry_2remove=mi_entry;

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
                    mount_entry->mountinfo_entry=NULL;
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

        /* set the parents: only required for the new ones */

        set_parents_added(&added_mounts);

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

                res=compare_entries(mount_entry, mount_entry_new);

                if ( res==0 ) {

                    /* the same:
                       move from removed_mounts_keep to added_mounts */

                    me_tmp1=mount_entry->next;
                    me_tmp2=mount_entry_new->next;

                    remove_mount_from_list(&removed_mounts_keep, mount_entry);

                    mount_entry->prev=mount_entry_new->prev;
                    mount_entry->next=mount_entry_new->next;

                    mount_entry->mountinfo_entry=mount_entry_new->mountinfo_entry;
                    mount_entry->mountinfo_entry->mount_entry=mount_entry;
                    mount_entry->remount=1;			/* dealing with a remount */
                    mount_entry->processed=0;
                    mount_entry->status=MOUNT_STATUS_UP; 	/* mount is up again */

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

                    /* increase in added_mounts */

                    mount_entry_new=mount_entry_new->next;

                }

            }

        }

        res=pthread_mutex_unlock(&changed_mounts_mutex);
        res=pthread_mutex_unlock(&current_mounts_mutex);

        /* log based on condition */

        logoutput_list(MOUNTENTRY_ADDED, 0);
        logoutput_list(MOUNTENTRY_REMOVED, 0);
        logoutput_list(MOUNTENTRY_REMOVED_KEEP, 0);

        /* do something with the changed and the removed:
           update the fs */

        res=update_notifyfs(&added_mounts, &removed_mounts, &removed_mounts_keep);

    }

}

unsigned char mount_is_up(struct mount_entry_struct *mount_entry)
{
    unsigned char nreturn=0;

    if ( mount_entry->status==MOUNT_STATUS_UP ) nreturn=1;

    return nreturn;

}


unsigned char mounted_by_autofs(struct mount_entry_struct *mount_entry)
{
    unsigned char nreturn=0;

    if ( mount_entry->autofs_mounted==1 ) nreturn=1;

    return nreturn;

}

int start_mountmonitor_thread(pthread_t *pthreadid)
{
    int nreturn=0;


    nreturn=pthread_create(pthreadid, NULL, handle_change_mounttable, NULL);

    if ( nreturn==-1 ) {

        nreturn=-errno;

	logoutput("Error creating a thread to monitor mounts (error: %i).", abs(nreturn));


    }

    return nreturn;

}

void signal_mountmonitor(struct epoll_event *e_event)
{
    int res;

    if ( ! e_event ) {

        /* called from main: init */

        current_mountinfo_list.first=NULL;
        current_mountinfo_list.last=NULL;
        current_mountinfo_list.s_first=NULL;
        current_mountinfo_list.s_last=NULL;

        new_mountinfo_list.first=NULL;
        new_mountinfo_list.last=NULL;
        new_mountinfo_list.s_first=NULL;
        new_mountinfo_list.s_last=NULL;

        added_mounts.first=NULL;
        added_mounts.last=NULL;

        removed_mounts.first=NULL;
        removed_mounts.last=NULL;

        removed_mounts_keep.first=NULL;
        removed_mounts_keep.last=NULL;

    }

    res=pthread_mutex_lock(&mountinfo_changed_mutex);

    if ( ! e_event ) {

	mountinfo_changed=2;

    } else {

	mountinfo_changed=1;

    }

    /* pthread_cond_signal is also possible here */

    res=pthread_cond_broadcast(&mountinfo_changed_cond);

    res=pthread_mutex_unlock(&mountinfo_changed_mutex);

}

/* FSTAB functions */

struct fstab_list_struct *get_fstab_list()
{
    struct fstab_list_struct *fstab_list;

    if ( ! fstab_list_unused ) {

	fstab_list=malloc(sizeof(struct fstab_list_struct));

    } else {

	fstab_list=fstab_list_unused;

	if ( fstab_list->next ) {

	    fstab_list_unused=fstab_list->next;

	    fstab_list->next=NULL;
	    fstab_list->prev=NULL;

	}

    }

    if ( fstab_list ) fstab_list->fstab_entry=NULL;

    return fstab_list;

}

void move_fstab_list_to_unused(struct fstab_list_struct *fstab_list)
{

    if ( fstab_list_unused ) fstab_list_unused->prev=fstab_list;
    fstab_list->next=fstab_list_unused;
    fstab_list_unused=fstab_list;

}

void read_fstab()
{
    FILE *fp;
    int res, nrlines;
    struct mntent fstab_entry, *tmp_entry;
    struct fstab_list_struct *fstab_list;
    char *fstab_buff, *tmp;
    struct stat st;
    size_t size;

    /* try to guess the number of fstab entries */

    res=lstat(FSTAB, &st);

    if ( res==-1 ) {

	/* huh, no fstab?? */
	return;

    }

    /* the bare minimum of a valid line in fstab is 10 characters long: 
       fs: >=3 
       sep: 1
       mountpoint: >= 1
       sep: 1
       fstype: >=3
       sep: 1
       options: >=0
       eol: >=0 */


    nrlines=(st.st_size / 10 ) + 1;

    size=sizeof(struct mntent) * nrlines;

    fstab_buff=malloc(size);

    if ( ! fstab_buff ) {

	/* no memory */

	return;

    }

    tmp=fstab_buff;

    fstab_list_first=NULL;
    fstab_list_last=NULL;

    fp=setmntent(FSTAB, "r");

    if ( fp ) {

	while ( ! feof(fp)) {

	    tmp_entry=getmntent_r(fp, &fstab_entry, tmp, fstab_buff+size-tmp);

	    if ( tmp_entry ) {

		logoutput1("found in fstab: %s on %s type %s options %s", 
		           tmp_entry->mnt_fsname, tmp_entry->mnt_dir, tmp_entry->mnt_type, tmp_entry->mnt_opts);

		/* add to list */

		fstab_list=get_fstab_list();

		if ( fstab_list ) {

		    if ( ! fstab_list_first ) fstab_list_first=fstab_list;
		    if ( ! fstab_list_last ) {

			fstab_list_last=fstab_list;

		    } else {

			fstab_list_last->next=fstab_list;
			fstab_list->prev=fstab_list_last;
			fstab_list_last=fstab_list;

		    }

		    fstab_list->fstab_entry=tmp_entry;

		}

	    }

	}

	endmntent(fp);

    }

}

