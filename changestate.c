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

#include <fuse/fuse_lowlevel.h>

#include "entry-management.h"
#include "path-resolution.h"
#include "logging.h"
#include "notifyfs.h"

#include "utils.h"
#include "options.h"
#include "client.h"
#include "mountinfo.h"
#include "watches.h"
#include "epoll-utils.h"

#include "message.h"
#include "path-resolution.h"

#include "changestate.h"

#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

extern struct notifyfs_options_struct notifyfs_options;
extern struct fuse_chan *notifyfs_chan;

pthread_mutex_t changestate_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t changestate_condition=PTHREAD_COND_INITIALIZER;

struct call_info_struct *changestate_call_info_first=NULL;
struct call_info_struct *changestate_call_info_last=NULL;

/* function to read the contents of a directory when a watch is set on that directory 

   a. this function is called when a watch is set. this function reads the contents of the directory to 
   get a copy of every inode/entry of the underlying fs. This is usefull when an event arrives from 
   inotify or backend. It is required to be compared with the cached values first.

   b. this function can also be called when a autofs managed mounted is mounted again, after previously
   unmounted. In the meantime, while it has been unmounted, changes on that fs can occur. When mounted again
   this function compares the new state with the last known and send messages when changes found.

   only do this for autofs mounted filesystems??


*/

void sync_directory_entries(char *path, struct effective_watch_struct *effective_watch, unsigned char atinit, unsigned char lockset)
{
    DIR *dp=NULL;
    struct notifyfs_entry_struct *entry=NULL;
    int res, lenpath;
    pathstring tmppath;

    logoutput("sync_directory_entries: directory %s", effective_watch->path);

    if ( effective_watch->inode ) entry=effective_watch->inode->alias;

    if ( ! entry ) return;

    if ( lockset==0 ) res=lock_effective_watch(effective_watch);

    if ( entry->child ) {
	struct notifyfs_entry_struct *child_entry=entry->child;

	while (child_entry) {

	    child_entry->synced=0;
	    child_entry=child_entry->dir_next;

	}

    }

    lenpath=strlen(path);

    dp=opendir(tmppath);

    if ( dp ) {
	struct dirent *de;
	char *name;
	struct notifyfs_entry_struct *tmp_entry;
	int res, lenname=0;
	unsigned char entrycreated=0, docompare=1;
	struct stat st;

	*(path+lenpath)='/';

	while(1) {

	    de=readdir(dp);

	    if ( ! de ) break;

	    /* here check the entry exist... */

	    name=de->d_name;
	    lenname=strlen(name);

	    if ( strcmp(name, ".")==0 ) {

		continue;

	    } else if ( strcmp(name, "..")==0 ) {

		continue;

	    }

	    /* add this entry to the base path */

	    memcpy(path+lenpath+1, name, lenname);
	    *(path+lenpath+1+lenname)='\0';

	    /* read the stat */

	    res=stat(path, &st);

	    if ( res==-1 ) {

		/* huh??? should not happen, ignore 
		    maybe additional actions here when a client is interested in this
		    part in a group/view
		*/

		continue;

	    }

	    /* 	find the matching entry in this fs 
		if not found create it */

	    tmp_entry=find_entry(entry->inode->ino, name);
	    entrycreated=0;
	    docompare=1;

	    if ( ! tmp_entry ) {

		tmp_entry=create_entry(entry, name, NULL);

		if (tmp_entry) {

		    assign_inode(tmp_entry);

		    if (tmp_entry->inode) {

			entrycreated=1;

			add_entry_to_dir(tmp_entry);
			add_to_name_hash_table(tmp_entry);
			add_to_inode_hash_table(tmp_entry->inode);

		    } else {

			remove_entry(tmp_entry);

			/* ignore this (memory) error */

			continue;

		    }

		} else {

		    continue;

		}

	    }

	    if ( atinit==1 ) continue;
	    tmp_entry->synced=1;

	    if (entrycreated==1) {

		docompare=0;

	    } else if ( ! tmp_entry->inode ) {

		/* compare the cached and the new stat 
		    only when not at init and with an existing entry */

		/* if there is a watch attached to this entry, this has to take action on this */

		docompare=0;

	    } else if ( tmp_entry->inode->effective_watch ) {

		docompare=0;

	    }

	    if ( docompare==1 ) {
		struct stat *cached_st=&(tmp_entry->inode->st);

		while (1) {

		    int group=NOTIFYFS_FSEVENT_NOTSET;
		    int type=0;

		    /* mode, owner and group belong to group META */

		    if (cached_st->st_mode!=st.st_mode || cached_st->st_uid!=st.st_uid || cached_st->st_gid!=st.st_gid) {

			group=NOTIFYFS_FSEVENT_META;

			if (cached_st->st_mode!=st.st_mode) {

			    type|=NOTIFYFS_FSEVENT_META_ATTRIB_MODE;
			    cached_st->st_mode=st.st_mode;

			}

			if (cached_st->st_uid!=st.st_uid) {

			    type|=NOTIFYFS_FSEVENT_META_ATTRIB_OWNER;
			    cached_st->st_uid=st.st_uid;

			}

			if (cached_st->st_gid!=st.st_gid) {

			    type|=NOTIFYFS_FSEVENT_META_ATTRIB_GROUP;
			    cached_st->st_gid=st.st_gid;

			}

			cached_st->st_ctim.tv_sec=st.st_ctim.tv_sec;
			cached_st->st_ctim.tv_nsec=st.st_ctim.tv_nsec;

			logoutput("sync_directory_entries: META attributes changed for %s", tmp_entry->name);

			/* here take action like sending messages*/

			continue;

		    }

		    /* nlinks belongs to group FS */

		    if (cached_st->st_nlink!=st.st_nlink) {

			group=NOTIFYFS_FSEVENT_FS;
			type|=NOTIFYFS_FSEVENT_FS_NLINKS;

			cached_st->st_nlink=st.st_nlink;

			cached_st->st_ctim.tv_sec=st.st_ctim.tv_sec;
			cached_st->st_ctim.tv_nsec=st.st_ctim.tv_nsec;

			logoutput("sync_directory_entries: FS nlinks changed for %s", tmp_entry->name);

			/* here take action */

			continue;

		    }

		    /* size belongs to group FILE */

		    if (cached_st->st_size!=st.st_size) {

			group=NOTIFYFS_FSEVENT_FILE;
			type|=NOTIFYFS_FSEVENT_FILE_SIZE;

			cached_st->st_size=st.st_size;

			/* change both the ctime and the mtime, since the inode attributes and the file are changed */

			cached_st->st_ctim.tv_sec=st.st_ctim.tv_sec;
			cached_st->st_ctim.tv_nsec=st.st_ctim.tv_nsec;

			cached_st->st_mtim.tv_sec=st.st_mtim.tv_sec;
			cached_st->st_mtim.tv_nsec=st.st_mtim.tv_nsec;

			logoutput("sync_directory_entries: FILE properties changed for %s", tmp_entry->name);

			/* here take action */

			continue;

		    }

		    /* if still ctim is different then there is something changed in xattr */

		    if ( cached_st->st_ctim.tv_sec<st.st_ctim.tv_sec || cached_st->st_ctim.tv_nsec!=st.st_ctim.tv_nsec ) {

			group=NOTIFYFS_FSEVENT_META;
			type|=NOTIFYFS_FSEVENT_META_ATTRIB_NOTSET; /* what has changed is not yet determined */

			cached_st->st_ctim.tv_sec=st.st_ctim.tv_sec;
			cached_st->st_ctim.tv_nsec=st.st_ctim.tv_nsec;

			logoutput("sync_directory_entries: META xattributes changed for %s", tmp_entry->name);

			/* here take action .... */

			continue;

		    }

		    /* compare the mtim: file is changed or contents of directory */

		    if ( cached_st->st_mtim.tv_sec<st.st_mtim.tv_sec || cached_st->st_mtim.tv_nsec!=st.st_mtim.tv_nsec ) {

			group=NOTIFYFS_FSEVENT_FILE;
			type|=NOTIFYFS_FSEVENT_FILE_NOTSET;

			cached_st->st_mtim.tv_sec=st.st_mtim.tv_sec;
			cached_st->st_mtim.tv_nsec=st.st_mtim.tv_nsec;

			logoutput("sync_directory_entries: FILE properties changed for %s", tmp_entry->name);

			/* here take action .... */

			continue;

		    }

		    if (group==NOTIFYFS_FSEVENT_NOTSET) break;

		}

	    } else if (entrycreated==1) {

		copy_stat(&(tmp_entry->inode->st), &st);

	    }

	}

	closedir(dp);
	*(path+lenpath)='\0';

    }

    /* here check entries which are not detected with the opendir
       these are obviously deleted/moved */

    if ( atinit==0 && entry->child ) {
	struct notifyfs_entry_struct *child_entry=entry->child, *next_entry;
	int lenname;

	*(path+lenpath)='/';

	while (child_entry) {

	    next_entry=child_entry->dir_next;

	    if ( child_entry->synced==0 ) {

		/* here: the entry is deleted */

		logoutput("entry %s is removed", child_entry->name);

		/* what if inode has watch attached: remove that too */

		if ( child_entry->inode ) {
		    struct call_info_struct call_info;

		    init_call_info(&call_info, child_entry);

		    /* add this entry to the base path */

		    lenname=strlen(child_entry->name);
		    memcpy(path+lenpath+1, child_entry->name, lenname);
		    *(path+lenpath+1+lenname)='\0';

		    call_info.path=path;

		    changestate(&call_info, FSEVENT_INODE_ACTION_REMOVE);

		} else {

		    remove_entry_from_dir(child_entry);
		    remove_entry_from_name_hash(child_entry);
		    remove_entry(child_entry);

		}

	    }

	    child_entry=next_entry;

	}

	*(path+lenpath)='\0';

    }

    if ( lockset==0 ) res=unlock_effective_watch(effective_watch);

}

void del_watch_backend(struct effective_watch_struct *effective_watch)
{
    int res;

    remove_watch_backend_os_specific(effective_watch);

}

/* 	function to call backend notify methods like:
	- inotify_add_watch
	- send a forward message to client fs

	called when setting a watch via xattr, but also
	when a fs is unmounted normally or via autofs

*/

void set_watch_backend(struct effective_watch_struct *effective_watch, int newmask, unsigned char lockset)
{
    int res;
    pathstring path;
    struct mount_entry_struct *mount_entry=effective_watch->mount_entry;

    /* create path of this watch 
       note the effective watch is relative to the mountpoint
       and if it's empty it's on the mountpoint*/

    if ( effective_watch->path ) {

	if ( is_rootmount(mount_entry) ) {

	    snprintf(path, PATH_MAX, "/%s", effective_watch->path);

	} else {

	    snprintf(path, PATH_MAX, "%s/%s", mount_entry->mountpoint, effective_watch->path);

	}

    } else {

	if ( is_rootmount(mount_entry) ) {

	    snprintf(path, PATH_MAX, "/");

	} else {

	    snprintf(path, PATH_MAX, "%s", mount_entry->mountpoint);

	}

    }

    if ( effective_watch->inotify_id==0 ) {

	sync_directory_entries(path, effective_watch, 1, lockset);

    }

    /* by default always set a inotify watch */

    logoutput("set_watch_backend: call inotify_add_watch on fd %i, path %s and mask %i", notifyfs_options.inotify_fd, path, newmask);

    set_watch_backend_os_specific(effective_watch, path, newmask);

}

/* function to remove every watch attached to an effective_watch 
   effective_watch must be locked
   when a watch is removed, a message is send to the client owning the watch
   */

int changestate_watches(struct effective_watch_struct *effective_watch, unsigned char typeaction)
{
    struct watch_struct *watch=effective_watch->watches, *next_watch;
    int nreturn=0;
    int res;

    while (watch) {

        next_watch=watch->next_per_watch;

        /* notify client */

        if ( watch->client ) {
            struct client_struct *client=watch->client;

            if ( client->status==NOTIFYFS_CLIENTSTATUS_UP ) {

                /* only send a message when client is up */

                if ( typeaction==WATCH_ACTION_REMOVE ) {

                    // res=send_delwatch_message(client->fd, watch->id);

                    if (res<0) logoutput("error writing to %i when sending remove watch message", client->fd);

                } else if ( typeaction==WATCH_ACTION_SLEEP ) {

                    // res=send_sleepwatch_message(client->fd, watch->id);

                    if (res<0) logoutput("error writing to %i when sending sleep watch message", client->fd);

                } else if ( typeaction==WATCH_ACTION_WAKEUP ) {

                    // res=send_wakewatch_message(client->fd, watch->id);

                    if (res<0) logoutput("error writing to %i when sending wakeup watch message", client->fd);

                }

            }

            if ( typeaction==WATCH_ACTION_REMOVE ) {

                /* if remove remove from client */

		lock_client(client);

                client->lock=1;

                if ( client->watches==watch ) client->watches=watch->next_per_client;
                if ( watch->prev_per_client ) watch->prev_per_client->next_per_client=watch->next_per_client;
                if ( watch->next_per_client ) watch->next_per_client->prev_per_client=watch->prev_per_client;

                watch->prev_per_client=NULL;
                watch->next_per_client=NULL;
                watch->client=NULL;

                unlock_client(client);

            }

        }

        if ( typeaction==WATCH_ACTION_REMOVE ) free(watch);

        watch=next_watch;

    }

    return nreturn;

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

int changestate_effective_watch(struct notifyfs_inode_struct *inode, unsigned char lockset, unsigned char typeaction, char *path)
{
    int nreturn=0;
    int res;

    if ( inode->effective_watch ) {
        struct effective_watch_struct *effective_watch=inode->effective_watch;

        /* try to lock the effective watch */

        res=lock_effective_watch(effective_watch);

        /* after here: the effective watch is locked by this thread */

        res=changestate_watches(effective_watch, typeaction);

        /* correct backend: backend and mount status and specific */

        if ( typeaction==WATCH_ACTION_REMOVE || typeaction==WATCH_ACTION_SLEEP ) {

	    /* when remove or sleep: remove the backend */

	    del_watch_backend(effective_watch);

        } else if ( typeaction==WATCH_ACTION_WAKEUP ) {

            /* when wake up and or set: set the backend */

	    set_watch_backend(effective_watch, effective_watch->mask, 1);

        }

        if ( typeaction==WATCH_ACTION_REMOVE ) {

	    if ( effective_watch->path ) {

		free(effective_watch->path);
		effective_watch->path=NULL;

	    }

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

static void changestate_recursive(struct notifyfs_entry_struct *entry, char *path, int pathlen, unsigned char typeaction)
{
    int res, nlen, pathlen_orig=pathlen;

    logoutput("changestate_recursive: entry %s, action %i", entry->name, typeaction);

    if ( entry->child ) {
        struct notifyfs_entry_struct *child_entry=entry->child, *tmp_entry;
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

        	res=fuse_lowlevel_notify_delete(notifyfs_chan, entry->parent->inode->ino, entry->inode->ino, entry->name, strlen(entry->name));

	    }

	    entry->inode->status=FSEVENT_INODE_STATUS_REMOVED;

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

static unsigned char queue_changestate(struct notifyfs_entry_struct *entry, char *path1)
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

    logoutput("wait_in_queue_and_process: action %i, path %s", typeaction, call_info->path);

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

	    if ( call_info->mount_entry ) {

		if ( mounted_by_autofs(call_info->mount_entry)==1 ) {

		    wait_in_queue_and_process(call_info, FSEVENT_INODE_ACTION_SLEEP);

		} else {

		    wait_in_queue_and_process(call_info, FSEVENT_INODE_ACTION_REMOVE);

		}

	    } else {

		logoutput("changestate: cannot go futher, mount entry not set for %s", call_info->path);

	    }

	}

    }

    res=pthread_mutex_unlock(&changestate_mutex);

}

/* process an notifyfs fsevent
   after an fs event occurs, it's translated into a notifyfs_fsevent
   for some actions (delete) this has serious consequences for notifyfs
   this function looks which actions have consequences and calls changestate

    NOTE: the actions like create are already processed in an earlier stage when translating a
    backend specific event into notifyfs_fsevent

*/

void process_notifyfs_fsevent(struct notifyfs_fsevent_struct *notifyfs_fsevent)
{

    if (notifyfs_fsevent->group==NOTIFYFS_FSEVENT_MOVE) {
	int type=notifyfs_fsevent->type;

	if (type&(NOTIFYFS_FSEVENT_MOVE_MOVED | NOTIFYFS_FSEVENT_MOVE_MOVED_FROM | NOTIFYFS_FSEVENT_MOVE_DELETED)) {

	    /* dealing with an entry which is removed */

	    if (notifyfs_fsevent->entry) {
		struct notifyfs_inode_struct *inode=notifyfs_fsevent->entry->inode;

		if (inode->status != FSEVENT_INODE_STATUS_REMOVED) {
		    struct call_info_struct call_info;
		    int res;

		    init_call_info(&call_info, notifyfs_fsevent->entry);

		    if (determine_path(&call_info, NOTIFYFS_PATH_FORCE)<0) return;

		    changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE);

		}

	    }

	}

    }

}

