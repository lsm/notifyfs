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

#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/inotify.h>

#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

#include <fuse/fuse_lowlevel.h>

#include "entry-management.h"
#include "path-resolution.h"
#include "logging.h"
#include "testfs.h"

#include "utils.h"
#include "client.h"
#include "options.h"
#include "mountstatus.h"
#include "watches.h"

#include "message.h"

#include "changestate.h"

extern struct testfs_options_struct notifyfs_options;
extern struct fuse_chan *testfs_chan;

pthread_mutex_t changestate_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t changestate_condition=PTHREAD_COND_INITIALIZER;

struct call_info_struct *changestate_call_info_first=NULL;
struct call_info_struct *changestate_call_info_last=NULL;


/* 	compare two stats 
	returns when a single difference is found...
	possibly it does the same when comparing only the mtime and ctime values...
	*/


unsigned char compare_attributes(struct stat *st1, struct stat *st2)
{
    unsigned char statchanged=FSEVENT_FILECHANGED_NONE;

    /* modification time eg file size , contents of the file, or in case of a directory, the number of entries 
       has changed... */

    if ( st1->st_mtime != st2->st_mtime ) {

	statchanged|=FSEVENT_FILECHANGED_FILE;

    }

#ifdef  __USE_MISC

    /* time defined as timespec */

    if ( st1->st_mtim.tv_nsec != st2->st_mtim.tv_nsec ) {

	statchanged|=FSEVENT_FILECHANGED_FILE;

    }

#else

    if ( st1->st_mtimensec != st2->st_mtimensec ) {

	statchanged|=FSEVENT_FILECHANGED_FILE;

    }


#endif


    /* metadata changed like file owner, permissions, mode AND eventual extended attributes */

    if ( st1->st_ctime != st2->st_ctime ) {

	statchanged|=FSEVENT_FILECHANGED_METADATA;

    }

#ifdef  __USE_MISC

    /* time defined as timespec */

    if ( st1->st_ctim.tv_nsec != st2->st_ctim.tv_nsec ) {

	statchanged|=FSEVENT_FILECHANGED_METADATA;

    }

#else

    if ( st1->st_ctimensec != st2->st_ctimensec ) {

	statchanged|=FSEVENT_FILECHANGED_METADATA;

    }


#endif


    return statchanged;

}

/* function which determines and reads the changes when an event is reported 
   20120401: only called after inotify event, todo also call at other backends

   when something happens on a watch, this function reads the changes from the underlying fs and 
   store/cache that in notifyfs, and compare that with what it has in it's cache
   if there is a difference send a messages about that to clients....

   more todo: read the xattr
   when handling also the xattr an administration for these is also required

   */


unsigned char determinechanges(struct call_info_struct *call_info, int mask)
{
    int res;
    unsigned char filechanged=FSEVENT_FILECHANGED_NONE;

    if ( call_info->entry ) {

	if ( ! call_info->path ) {

	    res=determine_path(call_info, TESTFS_PATH_FORCE);
	    if (res<0) goto out;

	}

	if ( mask & IN_ATTRIB ) {
	    struct stat st;
	    unsigned char statchanged=FSEVENT_FILECHANGED_NONE;

	    /* attributes have changed: read from underlying fs and compare */

	    res=lstat(call_info->path, &st);

	    statchanged=compare_attributes(&(call_info->entry->inode->st), &st);

	    if ( statchanged != FSEVENT_FILECHANGED_NONE ) {

		copy_stat(&(call_info->entry->inode->st), &st);

		filechanged|=statchanged;

	    }

	    /* here also the xattr ?? */

	}

    }

    out:

    return filechanged;

}

/* function to read the contents of a directory when a watch is set on that directory 

   this is handy when the mount is going to sleep mode
   maybe also handy for caching

   in future this may be handy for sending messages to the user about changes
   for example a SMB share is unmounted,
   while it's unmounted changes are made, on files a watch has been set upon from this user/host
   when is's mounted again, this function compares the previous content of a directory with the current on

   this function does that by reading the contents of this directory, and compare every entry found with it
   has found earlier

*/

void sync_directory_entries(struct call_info_struct *call_info, unsigned char atinit)
{
    DIR *dp=NULL;

    if ( call_info->entry->child ) {
	struct testfs_entry_struct *child_entry=call_info->entry->child;

	while (child_entry) {

	    child_entry->synced=0;
	    child_entry=child_entry->dir_next;

	}

    }

    dp=opendir(call_info->path);

    if ( dp ) {
	struct dirent *de;
	char *name;
	struct testfs_entry_struct *entry;
	pathstring path;
	int lenpath=strlen(call_info->path), res, lenname=0;
	unsigned char entrycreated=0;
	struct stat st;

	/* create a base path of this directory */

	memcpy(path, call_info->path, lenpath);
	*(path+lenpath)='/';

	while(1) {

	    de=readdir(dp);

	    if ( ! de ) break;

	    /* here check the entry exist... */

	    name=de->d_name;
	    entrycreated=0;
	    lenname=strlen(name);

	    /* add this entry to the base path */

	    memcpy(path+lenpath+1, name, lenname);
	    *(path+lenpath+1+lenname)='\0';

	    /* read the stat */

	    res=stat(path, &st);

	    if ( res!=-1 ) {

		/* compare the cached and the new stat */

		/* the diff in stat will result in an internal inotify event : IN_ATTRIB */

		entry=find_entry(call_info->entry->inode->ino, name);

		if ( ! entry ) {

		    entry=create_entry(call_info->entry, name, NULL);

		    /* here handle the not creating of the entry .... */

		    entrycreated=1;

		    /* insert at begin */

		    entry->dir_prev=NULL;

		    if ( call_info->entry->child ) call_info->entry->child->dir_prev=entry;
		    entry->dir_next=call_info->entry->child;
		    call_info->entry->child=entry;

		    add_to_name_hash_table(entry);

		}

		entry->synced=1;

		memcpy(path+lenpath+1, name, lenname);
		*(path+lenpath+1+lenname)='\0';

		res=stat(path, &st);

		if ( res==-1 ) {

		    /* huh??? */

		    entry->synced=0;

		} else {


		    if ( atinit==0 ) {

			/* compare the cached and the new stat */

			/*
			the diff in stat will result in inotify event : 
			- IN_ATTRIB and or IN_MODIFY
			if entry is created here then will result in inotify event :
			- IN_CREATE (or IN_MOVE_TO, but it's not possible to say here what's causing this....)
			*/

			if ( entrycreated==1 ) {

			    logoutput("entry %s is new", name);

			    /* what to do here: if anyone is interested: send a message */

			} else {
			    unsigned char takeaction=1;
			    unsigned char statchanged=0;


			    if ( entry->inode ) {

				/* if there is a watch attached to this entry, this has to take action on this */

				if ( entry->inode->effective_watch ) takeaction=0;

			    }

			    if ( takeaction==1 ) {

				/* here something like:
				res=compare_attributes(&(call_info->entry->inode->st), &st);
				if ( res!=0 ) ... action
				res=readxattrlist(call_info, &xattrlist);
				res=compare_xattributes(call_info->entry->inode->somexattrlist, xattrlist);
				if ( res!=0 ) ..
				*/

				statchanged=compare_attributes(&(call_info->entry->inode->st), &st);

				if ( statchanged != FSEVENT_FILECHANGED_NONE ) {

				    copy_stat(&(call_info->entry->inode->st), &st);

				}

				copy_stat(&(call_info->entry->inode->st), &st);

			    }

			}

		    }

		}

	    }

	}

	closedir(dp);

    }

    /* here check entries which are not detected with the opendir
       these are obviously deleted/moved */

    if ( call_info->entry->child ) {
	struct testfs_entry_struct *child_entry=call_info->entry->child, *next_entry;

	while (child_entry) {

	    if ( child_entry->synced==0 ) {

		next_entry=child_entry->dir_next;

		/* here: the entry is deleted */
		/*- IN_DELETE (or IN_MOVE, but it's not possible to say here what's causing this....)*/

		logoutput("entry %s is removed", child_entry->name);

		/* remove */

		if ( call_info->entry->child==child_entry) call_info->entry->child=next_entry;
		if ( child_entry->dir_prev ) child_entry->dir_prev->dir_next=next_entry;
		if ( next_entry ) next_entry->dir_prev=child_entry->dir_prev;

		remove_entry_from_name_hash(child_entry);

		if ( child_entry->inode ) child_entry->inode->status=FSEVENT_INODE_STATUS_REMOVE;

		remove_entry(child_entry);

	    } else if ( ! child_entry->inode ) {

		assign_inode(child_entry);

		add_to_inode_hash_table(child_entry->inode);

	    }

	    child_entry=next_entry;

	}

    }

}

/*
   function to change the state of an effective watch 
   one of the possibilities is of course the removal, but also possible is
   the setting into sleep when the underlying fs is unmounted or
   waking it up when previously is has been set to sleep and the underlying
   fs is mounted
   this setting to sleep and waking up again is typically the case with autofs 
   managed filesystems
   - remove: remove the effective_watch, and the notify backend method
   - sleep: change the state of the effective_watch into sleep, and remove the notify backend method
   - wakeup: change the state of the effective_watch into active, and set the notify backend method
   this function calls change_state_watches to make changes effective to the individual watches

*/

int changestate_effective_watch(struct testfs_inode_struct *inode, unsigned char lockset, unsigned char typeaction, char *path)
{
    int nreturn=0;
    int res;

    if ( inode->effective_watch ) {
        struct effective_watch_struct *effective_watch=inode->effective_watch;

        /* try to lock the effective watch */

        res=lock_effective_watch(effective_watch);

        /* after here: the effective watch is locked by this thread */

        /* correct backend: backend and mount status and specific */

        if ( typeaction==WATCH_ACTION_REMOVE || typeaction==WATCH_ACTION_SLEEP ) {

	    /* when remove or sleep: remove the backend */

	    del_watch_at_backend(effective_watch);

        } else if ( typeaction==WATCH_ACTION_WAKEUP ) {

            /* when wake up and or set: set the backend */

	    set_watch_at_backend(effective_watch, effective_watch->mask);

        }

        if ( typeaction==WATCH_ACTION_REMOVE ) {

            /* remove from global list */

            remove_effective_watch_from_list(effective_watch, 0);

            /* detach from inode */

            inode->effective_watch=NULL;
            effective_watch->inode=NULL;

	    res=unlock_effective_watch(effective_watch);

            /* free and destroy */

            pthread_mutex_destroy(&effective_watch->lock_mutex);
            pthread_cond_destroy(&effective_watch->lock_condition);

            free(effective_watch);

        } else {

	    res=unlock_effective_watch(effective_watch);

	}

    }

    return nreturn;

}

static void changestate_recursive(struct testfs_entry_struct *entry, char *path, int pathlen, unsigned char typeaction)
{
    int res, nlen, pathlen_orig=pathlen;

    logoutput("changestate_recursive: entry %s, action %i", entry->name, typeaction);

    if ( entry->child ) {
        struct testfs_entry_struct *child_entry=entry->child, *tmp_entry;
        /* check every child */

        while (child_entry) {

	    /* keep the next entry, since child_entry can be removed */

	    tmp_entry=child_entry->dir_next;

            /* here something like copy child_entry->name to path */

            *(path+pathlen)='/';
            pathlen++;
            nlen=strlen(child_entry->name);
            memcpy(path+pathlen, child_entry->name, nlen);
            pathlen+=nlen;

            changestate_recursive(child_entry, path, pathlen, typeaction ); /* recursive */

	    /* reset the path */

	    pathlen=pathlen_orig;
	    *(path+pathlen)='\0';

            child_entry=tmp_entry;

        }

    }

    /* when here the directory is empty */

    if ( typeaction==FSEVENT_INODE_ACTION_REMOVE ) {

	if ( entry->inode ) {

	    if ( entry->inode->effective_watch ) res=changestate_effective_watch(entry->inode, 0, WATCH_ACTION_REMOVE, path);

	    if ( entry->parent && entry->parent->inode ) {

        	/* send invalidate entry */
        	/* use notify_delete or notify_inval_entry ?? */

        	res=fuse_lowlevel_notify_delete(testfs_chan, entry->parent->inode->ino, entry->inode->ino, entry->name, strlen(entry->name));

	    }

	    entry->inode->status=FSEVENT_INODE_STATUS_REMOVE;

	}

	if ( entry->parent ) {

            /* remove from child list parent if not detached already */

            if ( entry->parent->child==entry ) entry->parent->child=entry->dir_next;
            if ( entry->dir_prev ) entry->dir_prev->dir_next=entry->dir_next;
            if ( entry->dir_next ) entry->dir_next->dir_prev=entry->dir_prev;

	}

	remove_entry_from_name_hash(entry);
	remove_entry(entry);

    } else if ( typeaction==FSEVENT_INODE_ACTION_SLEEP ) {

	if ( entry->inode ) {

	    if ( entry->inode->effective_watch ) res=changestate_effective_watch(entry->inode, 0, WATCH_ACTION_SLEEP, path);

	    entry->inode->status=FSEVENT_INODE_STATUS_SLEEP;

	}

    } else if ( typeaction==FSEVENT_INODE_ACTION_WAKEUP ) {

	if ( entry->inode ) {

	    if ( entry->inode->effective_watch ) res=changestate_effective_watch(entry->inode, 0, WATCH_ACTION_WAKEUP, path);

	    entry->inode->status=FSEVENT_INODE_STATUS_OK;

	}

    } else if ( typeaction==FSEVENT_INODE_ACTION_CREATE ) {

	/* this will never happen, cause an inode is never created this way */

	if ( entry->inode ) entry->inode->status=FSEVENT_INODE_STATUS_OK;

    }

}

/* function to test queueing a change state entry is necessary 

   note the queue has to be locked 
    TODO: take in account the type of the action, sleep versus remove, what to
    do when both in queue
   */

static unsigned char queue_changestate(struct testfs_entry_struct *entry, char *path1)
{
    unsigned char doqueue=0;

    logoutput("queue_changestate: entry %s and path %s", entry->name, path1);

    if ( ! changestate_call_info_first ) {

        /* queue is empty: put it on queue */

        doqueue=1;

    } else {
        struct call_info_struct *call_info_tmp=changestate_call_info_last;
        char *path2;
        int len1=strlen(path1), len2;

        doqueue=1;

        /* walk through queue to check there is a related call already there */

        while(call_info_tmp) {

            path2=call_info_tmp->path;
            len2=strlen(path2);

            if ( len2>len1 ) {

                /* test path2 is a subdirectory of path1 */

                if ( call_info_tmp!=changestate_call_info_first ) {

                    if ( strncmp(path2+len1, "/", 1)==0 && strncmp(path1, path2, len1)==0 ) {

                        /* here replace the already pending entry 2 remove by this one...
                        do this only when it's not the first!!*/

                        call_info_tmp->entry2remove=entry;
                        strcpy(path2, (const char *) path1); /* this possible cause len(path2)>len(path1) HA !!!! */
                        doqueue=0;

                        break;

                    }

                }

            } else {

                /* len2<=len1, test path1 is a subdirectory of path2 */

		if ( strcmp(path1, path2)==0 ) {

		    /* already queued: the same */

		    doqueue=0;

		    break;

		} else if ( strncmp(path1+len2, "/", 1)==0 && strncmp(path2, path1, len2)==0 ) {

                    /* ready: already queued... */
                    doqueue=0;

                    break;

                }

            }

            call_info_tmp=call_info_tmp->prev;

        }

    }

    return doqueue;

}

/* wait for the call_info to be the first in remove entry queue, and process when it's the first 
  the lock has to be set
*/

static void wait_in_queue_and_process(struct call_info_struct *call_info, unsigned char typeaction)
{
    int res;
    pathstring path;

    if ( call_info != changestate_call_info_first ) {

        /* wait till it's the first */

        while ( call_info != changestate_call_info_first ) {

            res=pthread_cond_wait(&changestate_condition, &changestate_mutex);

        }

    }

    /* call_info is the first in the queue, release the queue for other threads and go to work */

    res=pthread_mutex_unlock(&changestate_mutex);

    memset(path, '\0', sizeof(pathstring));
    strcpy(path, call_info->path);

    changestate_recursive(call_info->entry2remove, path, strlen(path), typeaction);

    /* remove from queue... */

    res=pthread_mutex_lock(&changestate_mutex);

    changestate_call_info_first=call_info->next;

    if ( changestate_call_info_first ) changestate_call_info_first->prev=NULL;

    call_info->next=NULL;
    call_info->prev=NULL;

    if ( changestate_call_info_last==call_info ) changestate_call_info_last=changestate_call_info_first;

    /* signal other threads if the queue is not empty */

    if ( changestate_call_info_first ) res=pthread_cond_broadcast(&changestate_condition);

}

/* important function to react on signals which imply that the state of entries and watches 
   has changed

   this is called by the fuse threads, but also from the mainloop, which reacts on inotify events,
   and mountinfo when mounts are removed or added

   to do so a queue is used
   every call is added to the tail, and when it has become the first, it's processed
   every time the first call is removed, and thus the next call has become the first, 
   a broadcast is done to the other waiting threads
   the one who has become first is taking action

   very nice of this queue is the ability to check other pending calls
   when there is a related request present in the queue, it's not necessary to queue this request again
   when in a tree different watches are set, and the whole tree is removed this will result in a burst
   of inotify events: this queue will filter out doubles
   */

void changestate(struct call_info_struct *call_info, unsigned char typeaction)
{
    int res;
    unsigned char doqueue=1;

    call_info->entry2remove=call_info->entry;
    call_info->next=NULL;
    call_info->prev=NULL;

    if ( call_info->entry ) {

	if ( call_info->entry->name ) {

    	    logoutput("changestate: processing %s, entry %s", call_info->path, call_info->entry->name);

	} else {

	    logoutput("changestate: processing %s, entry **NO NAME**", call_info->path);

	}

    } else {

        logoutput("changestate: processing %s, entry **NOT SET**", call_info->path);

    }

    /* lock the remove entry queue */

    res=pthread_mutex_lock(&changestate_mutex);

    doqueue=queue_changestate(call_info->entry, call_info->path);

    if ( doqueue==1 ) {

        if ( ! changestate_call_info_first ) {

            /* queue is empty: put it on queue */

            changestate_call_info_first=call_info;
            changestate_call_info_last=call_info;

        } else {

            /* add at tail of queue */

            changestate_call_info_last->next=call_info;
            call_info->prev=changestate_call_info_last;
            call_info->next=NULL;
            changestate_call_info_last=call_info;

        }

	if ( typeaction==FSEVENT_ACTION_TREE_UP ) {

    	    wait_in_queue_and_process(call_info, FSEVENT_INODE_ACTION_WAKEUP);

	} else if ( typeaction==FSEVENT_ACTION_TREE_REMOVE ) {

	    wait_in_queue_and_process(call_info, FSEVENT_INODE_ACTION_REMOVE);

	}

    }

    res=pthread_mutex_unlock(&changestate_mutex);

}

/* function which creates a path to an entry 

    it does this by testing every subpath and create that

    */

struct testfs_entry_struct *create_fs_path(char *path2create)
{
    char *path, *slash, *name;
    struct testfs_inode_struct *pinode; 
    struct testfs_entry_struct *pentry, *entry;
    unsigned char fullpath=0;
    int nreturn=0, res;
    pathstring tmppath;
    struct stat st;

    pentry=get_rootentry();
    pinode=pentry->inode;

    strcpy(tmppath, path2create);
    path=tmppath;

    logoutput("create_fs_path: creating %s", tmppath);

    /*  translate path into entry 
        suppose here safe that entry is a subdir of root entry...*/


    while(1) {

        /*  walk through path from begin to end and 
            check every part */

        slash=strchr(path, '/');

        if ( slash==tmppath ) {

            /* ignore the starting slash*/

            path++;

            /* if nothing more (==only a slash) stop here */

            if (strlen(path)==0) {

		entry=pentry;
        	break;

	    }

            continue;

        }

        if ( ! slash ) {

            fullpath=1;

        } else {

	    /* replace slash with a string terminator */

            *slash='\0';

        }

        name=path;

        if ( name ) {

            entry=find_entry(pinode->ino, name);

            if ( ! entry ) {

        	/* check the stat */

        	res=lstat(tmppath, &st);

        	if ( res==-1 ) {

            	    /* what to do here?? the path does not exist */
        	    entry=NULL;
            	    goto out;

        	}

                entry=create_entry(pentry, name, NULL);

                if (entry) {

                    assign_inode(entry);

                    /* here also a check the inode is assigned ..... */

                    /*  is there a function to signal kernel an inode and entry are created 
                        like fuse_lowlevel_notify_new_inode */

                    add_to_inode_hash_table(entry->inode);
                    add_to_name_hash_table(entry);
                    copy_stat(&(entry->inode->st), &st);


                } else {

                    entry=NULL;
                    goto out;

                }

            }

        }

        if ( fullpath==1 ) {

            break;

        } else {

            /* make slash a slash again (was turned into a \0) */
            *slash='/';
            path=slash+1;
            pentry=entry;
            pinode=entry->inode;

        }

    }

    out:

    return entry;

}

