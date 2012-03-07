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

#include <fuse/fuse_lowlevel.h>

#include "entry-management.h"
#include "logging.h"
#include "notifyfs.h"

#include "fuse-loop-epoll-mt.h"

#include "utils.h"
#include "options.h"
#include "xattr.h"
#include "client.h"
#include "socket.h"
#include "access.h"
#include "mountinfo.h"

pthread_mutex_t changestate_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t changestate_condition=PTHREAD_COND_INITIALIZER;

struct call_info_struct *changestate_call_info_first=NULL;
struct call_info_struct *changestate_call_info_last=NULL;

struct notifyfs_commandline_options_struct notifyfs_commandline_options;
struct notifyfs_options_struct notifyfs_options;

struct fuse_opt notifyfs_help_options[] = {
     NOTIFYFS_OPT("--socket=%s",		socket, 0),
     NOTIFYFS_OPT("socket=%s",		        socket, 0),
     NOTIFYFS_OPT("logging=%i",			logging, 0),
     NOTIFYFS_OPT("--logging=%i",		logging, 0),
     NOTIFYFS_OPT("logarea=%i",			logarea, 0),
     NOTIFYFS_OPT("--logarea=%i",		logarea, 0),
     NOTIFYFS_OPT("accessmode=%i",		accessmode, 0),
     NOTIFYFS_OPT("--accessmode=%i",		accessmode, 0),
     NOTIFYFS_OPT("testmode",		        testmode, 1),
     NOTIFYFS_OPT("--testmode",		        testmode, 1),
     FUSE_OPT_KEY("-V",            		KEY_VERSION),
     FUSE_OPT_KEY("--version",      		KEY_VERSION),
     FUSE_OPT_KEY("-h",             		KEY_HELP),
     FUSE_OPT_KEY("--help",         		KEY_HELP),
     FUSE_OPT_END
};


static struct fuse_chan *notifyfs_chan;
struct notifyfs_entry_struct *root_entry;
extern struct mount_entry_struct *root_mount;
unsigned char loglevel=0;
unsigned char logarea=0;


/* function to read the contents of a directory when a watch is set on that directory */





/* 	function to call backend notify methods like:
	- inotify_add_watch

	called when setting a watch via xattr, but also
	when a fs is unmounted normally or via autofs

*/

void set_watch_backend(char *path, struct effective_watch_struct *effective_watch, int newmask, struct mount_entry_struct *mount_entry)
{
    int res;

    /* test the fs the inode is part of is mounted */

    if ( newmask>0 ) {

	if ( mount_entry ) {

	    if ( mount_entry->status!=MOUNT_STATUS_UP ) {

		logoutput("set_watch_backend: %s is on unmounted fs, doing nothing", path);
		return;

	    }

	} else {

	    logoutput("set_watch_backend: mount entry not defined with path %s", path);

	}

    }


    if ( effective_watch->typebackend==BACKEND_METHOD_INOTIFY ) {

	if ( newmask>0 ) {

            logoutput2("set_watch_backend: setting inotify watch on %s with mask %i", path, newmask);

            res=inotify_add_watch(notifyfs_options.inotify_fd, path, newmask);

            if ( res==-1 ) {

                logoutput2("set_watch_backend: setting inotify watch gives error: %i", errno);

            } else {

		/* here a check res is the same as id when the watch existed */

                effective_watch->id=(unsigned long) res;

    	    }

        } else {

	    /* newmask==0 means remove */

            res=inotify_rm_watch(notifyfs_options.inotify_fd, (int) effective_watch->id);

            if ( res==-1 ) {

                logoutput2("set_watch_backend: deleting inotify watch %i gives error: %i", (int) effective_watch->id, errno);

            } else {

                effective_watch->id=0;

            }

        }

    } else if ( effective_watch->typebackend==BACKEND_METHOD_FORWARD ) {
	struct client_struct *client=(struct client_struct *) effective_watch->backend;

	if ( client->status == NOTIFYFS_CLIENTSTATUS_UP ) {

	    /* forward the setting of the mask to the fs */

	    if ( newmask>0 ) {

		logoutput2("set_watch_backend: forward watch on %s with mask %i", path, newmask);

		/*  setting of watch with mask on client fs 
		    translate the internal inotify mask to a mask understood 
		    by the fs
		*/

		/* here also something like inotify_add_watch a reference to the watch
		   and possibly communicate with the fs using the path or in the ino
		   the ino is known since this function is called from setxattr where the
		  stat is checked (or from cache)
		*/

		res=send_notify_message(client->fd, NOTIFYFS_MESSAGE_TYPE_SETWATCH, 0, mask, strlen(path), path);

		/* here also a check of the result, and possibly a ack message */

	    } else {

		logoutput2("set_watch_backend: forward delwatch on %s", path);

		res=send_notify_message(client->fd, NOTIFYFS_MESSAGE_TYPE_DELWATCH, 0, mask, strlen(path), path);

		/* here also a check of the result, and possibly a ack message */

	    }

	} else {

	    if ( client->mount_entry ) {

		logoutput("set_watch_backend: forward watch on %s not possible: client %s is not up", path, client->mount_entry->fstype);

	    } else {

		logoutput("set_watch_backend: forward watch on %s not possible: client (unknown) is not up", path);

	    }

	}

    } else {

        logoutput("Error: backend %i not reckognized.", effective_watch->typebackend);

    }

}

/* function to match the path which is send by a client fs as his
   mountpoint (=path) to a mountpoint/entry known to notifyfs

   this function is called whenever a client registers itself at
   notifyfs as clientfs, and provides the mountpoint as additional
   data

   */

void assign_mountpoint_clientfs(struct client_struct *client, char *path)
{
    struct mount_entry_struct *mount_entry;

    logoutput("assign_mountpoint_clientfs: lookup mount entry matching %s", path);

    res=lock_mountlist(MOUNTENTRY_CURRENT);

    mount_entry=get_next_mount_entry(NULL, MOUNTENTRY_CURRENT);

    while (mount_entry) {


	if ( strcmp(mount_entry->mountpoint, path)==0 ) {

	    mount_entry->client=(void *) client;
	    client->mount_entry=mount_entry;

	    logoutput("assign_mountpoint_clientfs: mount entry found");

	    break;

	}


        mount_entry=get_next_mount_entry(mount_entry, MOUNTENTRY_CURRENT);

    }

    if ( ! mount_entry ) logoutput("assign_mountpoint_clientfs: mount entry not found");

    res=unlock_mountlist(MOUNTENTRY_CURRENT);

}


void cachechanges(struct call_info_struct *call_info, int mask)
{
    int res;

    if ( call_info->entry ) {

	if ( ! call_info->path ) {

	    res=determine_path(call_info, NOTIFYFS_PATH_FORCE);
	    if (res<0) return;

	}

	if ( mask & IN_ATTRIB ) {
	    struct stat st;
	    unsigned char statchanged=0;

	    res=lstat(call_info->path, &st);

	    /* compare with the cached ones */

	    if ( call_info->entry->inode->st.st_mode != st.st_mode ) {

		statchanged=1;
		call_info->entry->inode->st.st_mode = st.st_mode;

	    }

	    if ( call_info->entry->inode->st.st_nlink != st.st_nlink ) {

		statchanged=1;
		call_info->entry->inode->st.st_nlink = st.st_nlink;

	    }

	    if ( call_info->entry->inode->st.st_uid != st.st_uid ) {

		statchanged=1;
		call_info->entry->inode->st.st_uid = st.st_uid;

	    }

	    if ( call_info->entry->inode->st.st_gid != st.st_gid ) {

		statchanged=1;
		call_info->entry->inode->st.st_gid = st.st_gid;

	    }

	    if ( call_info->entry->inode->st.st_rdev != st.st_rdev ) {

		statchanged=1;
		call_info->entry->inode->st.st_rdev = st.st_rdev;

	    }

	    if ( call_info->entry->inode->st.st_size != st.st_size ) {

		statchanged=1;
		call_info->entry->inode->st.st_size = st.st_size;

	    }

	    if ( call_info->entry->inode->st.st_atime != st.st_atime ) {

		// access: no statchanged=1;
		call_info->entry->inode->st.st_atime = st.st_atime;

	    }

#ifdef  __USE_MISC

	    // time defined as timespec

	    if ( call_info->entry->inode->st.st_atim.tv_nsec != st.st_atim.tv_nsec ) {

		// access: no statchanged=1;
		call_info->entry->inode->st.st_atim.tv_nsec = st.st_atim.tv_nsec;

	    }

#else

	    if ( call_info->entry->inode->st.st_atimensec != st.st_atimensec ) {

		// access: no statchanged=1;
		call_info->entry->inode->st.st_atimensec = st.st_atimensec;

	    }


#endif

	    if ( call_info->entry->inode->st.st_mtime != st.st_mtime ) {

		statchanged=1;
		call_info->entry->inode->st.st_mtime = st.st_mtime;

	    }

#ifdef  __USE_MISC

	    // time defined as timespec

	    if ( call_info->entry->inode->st.st_mtim.tv_nsec != st.st_mtim.tv_nsec ) {

		// access: no statchanged=1;
		call_info->entry->inode->st.st_mtim.tv_nsec = st.st_mtim.tv_nsec;

	    }

#else

	    if ( call_info->entry->inode->st.st_mtimensec != st.st_mtimensec ) {

		// access: no statchanged=1;
		call_info->entry->inode->st.st_mtimensec = st.st_mtimensec;

	    }


#endif


	    if ( call_info->entry->inode->st.st_ctime != st.st_ctime ) {

		statchanged=1;
		call_info->entry->inode->st.st_ctime = st.st_ctime;

	    }

#ifdef  __USE_MISC

	    // time defined as timespec

	    if ( call_info->entry->inode->st.st_ctim.tv_nsec != st.st_ctim.tv_nsec ) {

		// access: no statchanged=1;
		call_info->entry->inode->st.st_ctim.tv_nsec = st.st_ctim.tv_nsec;

	    }

#else

	    if ( call_info->entry->inode->st.st_ctimensec != st.st_ctimensec ) {

		// access: no statchanged=1;
		call_info->entry->inode->st.st_ctimensec = st.st_ctimensec;

	    }


#endif


	    if ( call_info->entry->inode->st.st_blksize != st.st_blksize ) {

		statchanged=1;
		call_info->entry->inode->st.st_blksize = st.st_blksize;

	    }

	    if ( call_info->entry->inode->st.st_blocks != st.st_blocks ) {

		statchanged=1;
		call_info->entry->inode->st.st_blocks = st.st_blocks;

	    }

	    if ( statchanged==1 ) {

		logoutput2("cachechanges: stat changed for %s", call_info->path);

	    }

	    /* here also the xattr ?? */

	}

    }

}


/* function to remove every watch attached to an effective_watch 
   effective_watch must be locked
   when a watch is removed, a message is send to the client owning the watch
   */

int changestate_watches(struct effective_watch_struct *effective_watch, unsigned char typeaction)
{
    struct watch_struct *watch=effective_watch->watches;
    int nreturn=0;
    int res;

    while (watch) {

        effective_watch->watches=watch->next_per_watch;

        /* notify client */

        if ( watch->client ) {
            struct client_struct *client=watch->client;

            if ( client->status==NOTIFYFS_CLIENTSTATUS_UP ) {

                /* only send a message when client is up */

                if ( typeaction==WATCH_ACTION_REMOVE ) {

                    res=send_notify_message(client->fd, NOTIFYFS_MESSAGE_TYPE_DELWATCH, watch->id, 0, 0, NULL);

                    if (res<0) logoutput1("error writing to %i when sending remove watch message", client->fd);

                } else if ( typeaction==WATCH_ACTION_SLEEP ) {

                    res=send_notify_message(client->fd, NOTIFYFS_MESSAGE_TYPE_SLEEPWATCH, watch->id, 0, 0, NULL);

                    if (res<0) logoutput1("error writing to %i when sending sleep watch message", client->fd);

                } else if ( typeaction==WATCH_ACTION_WAKEUP ) {

                    res=send_notify_message(client->fd, NOTIFYFS_MESSAGE_TYPE_WAKEWATCH, watch->id, 0, 0, NULL);

                    if (res<0) logoutput1("error writing to %i when sending wakeup watch message", client->fd);

                }

            }

            if ( typeaction==WATCH_ACTION_REMOVE ) {

                /* if remove remove from client */

                res=pthread_mutex_lock(&client->lock_mutex);

                if ( client->lock==1 ) {

                    while (client->lock==1) {

                        res=pthread_cond_wait(&client->lock_condition, &client->lock_mutex);

                    }

                }

                client->lock=1;

                if ( client->watches==watch ) client->watches=watch->next_per_client;
                if ( watch->prev_per_client ) watch->prev_per_client->next_per_client=watch->next_per_client;
                if ( watch->next_per_client ) watch->next_per_client->prev_per_client=watch->prev_per_client;

                watch->prev_per_client=NULL;
                watch->next_per_client=NULL;
                watch->client=NULL;

                client->lock=0;
                res=pthread_cond_broadcast(&client->lock_condition);
                res=pthread_mutex_unlock(&client->lock_mutex);

            }

        }

        if ( typeaction==WATCH_ACTION_REMOVE ) free(watch);

        watch=effective_watch->watches;

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

        res=pthread_mutex_lock(&effective_watch->lock_mutex);

        if ( effective_watch->lock==1 ) {

            while (effective_watch->lock==1) {

                res=pthread_cond_wait(&effective_watch->lock_condition, &effective_watch->lock_mutex);

            }

        }

        effective_watch->lock=1;

        /* after here: the effective watch is locked by this thread */

        res=changestate_watches(effective_watch, typeaction);

        /* backend specific */

        if ( typeaction==WATCH_ACTION_REMOVE || typeaction==WATCH_ACTION_SLEEP ) {

	    /* when remove or sleep: remove the backend */

	    set_watch_backend(path, effective_watch, 0, NULL);

        } else if ( typeaction==WATCH_ACTION_WAKEUP ) {

            /* when wake up and or set: set the backend */

	    set_watch_backend(path, effective_watch, effective_watch->mask, NULL;

        }

        effective_watch->lock=0;
        res=pthread_cond_broadcast(&effective_watch->lock_condition);
        res=pthread_mutex_unlock(&effective_watch->lock_mutex);

        if ( typeaction==WATCH_ACTION_REMOVE ) {

            /* remove from global list */

            remove_effective_watch_from_list(effective_watch);

            /* detach from inode */

            inode->effective_watch=NULL;
            effective_watch->inode=NULL;

            /* free and destroy */

            pthread_mutex_destroy(&effective_watch->lock_mutex);
            pthread_cond_destroy(&effective_watch->lock_condition);

            free(effective_watch);

        }

    }

    return nreturn;

}


static void changestate_recursive(struct notifyfs_entry_struct *entry, char *path, int pathlen, unsigned char typeaction)
{
    int res, nlen, pathlen_orig=pathlen;

    logoutput2("changestate_recursive: entry %s", entry->name);

    if ( entry->child ) {
        struct notifyfs_entry_struct *child_entry=entry->child;
        /* check every child */

        while (child_entry) {

            entry->child=child_entry->dir_next;
            child_entry->dir_prev=NULL;
            child_entry->dir_next=NULL;

            /* here something like copy child_entry->name to path */

            *(path+pathlen)='/';
            pathlen++;
            nlen=strlen(child_entry->name);
            memcpy(path+pathlen, child_entry->name, nlen);
            pathlen+=nlen;

            changestate_recursive(child_entry, path, pathlen, typeaction ); /* recursive */

            child_entry=entry->child;

        }

    }

    /* when here the directory is empty */

    pathlen=pathlen_orig;
    *(path+pathlen)='\0';

    if ( entry->inode ) {

        /* set path back to original state */

        res=changestate_effective_watch(entry->inode, 0, typeaction, path);

    }

    if ( typeaction==WATCH_ACTION_REMOVE ) {

        remove_entry_from_name_hash(entry);

        if ( entry->parent ) {

            /* send invalidate entry */
            /* use notify_delete or notify_inval_entry ?? */

            res=fuse_lowlevel_notify_delete(notifyfs_chan, entry->parent->inode->ino, entry->inode->ino, entry->name, strlen(entry->name));

            /* remove from child list parent if not detached already */

            if ( entry->parent->child==entry ) entry->parent->child=entry->dir_next;
            if ( entry->dir_prev ) entry->dir_prev->dir_next=entry->dir_next;
            if ( entry->dir_next ) entry->dir_next->dir_prev=entry->dir_prev;

        }

        remove_entry(entry);

    }

}

/* function to test queueing a remove entry is necessary 

   note the queue has to be locked 
    TODO: the type of the action is required??
   */

unsigned char queue_changestate(struct notifyfs_entry_struct *entry, char *path1)
{
    unsigned char doqueue=0;

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

void wait_in_queue_and_process(struct call_info_struct *call_info, unsigned char typeaction)
{
    int res;
    pathstring path;

    if ( call_info != changestate_call_info_first ) {

        /* wait till it's the first */

        while ( call_info != changestate_call_info_first ) {

            res=pthread_cond_wait(&changestate_condition, &changestate_mutex);

        }

    }

    /* call_info is the first in the queue, release the queue for other queues and go to work */

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

        logoutput1("changestate: processing %s, entry %s", call_info->path, call_info->entry->name);

    } else {

        logoutput1("changestate: processing %s, entry not set", call_info->path);

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

        wait_in_queue_and_process(call_info, typeaction);

    }

    res=pthread_mutex_unlock(&changestate_mutex);

}

static int create_notifyfs_mount_path(struct mount_entry_struct *mount_entry)
{
    char *path, *slash, *name;
    struct notifyfs_inode_struct *pinode; 
    struct notifyfs_entry_struct *pentry, *entry;
    unsigned char fullpath=0;
    int nreturn=0, res;
    pathstring tmppath;
    struct stat st;

    pentry=root_entry;
    pinode=pentry->inode;
    strcpy(tmppath, mount_entry->mountpoint);
    path=tmppath;

    logoutput1("create_notifyfs_mount_path: creating %s", tmppath);

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

            if (strlen(slash)==0) break;

            continue;

        }

        if ( ! slash ) {

            fullpath=1;

        } else {

            *slash='\0';

        }

        name=path;

        if ( name ) {

            /* check the stat */

            res=lstat(tmppath, &st);

            if ( res==-1 ) {

                /* what to do here?? the mountpoint should exist.... ignore ?? */
                mount_entry->status=MOUNT_STATUS_NOTSET;
                goto out;

            }

            entry=find_entry(pinode->ino, name);

            if ( ! entry ) {

                entry=create_entry(pentry, name, NULL);

                if (entry) {

                    assign_inode(entry);

                    /* here also a check the inode is assigned ..... */

                    /*  is there a function to signal kernel an inode and entry are created 
                        like fuse_lowlevel_notify_new_inode */

                    add_to_inode_hash_table(entry->inode);
                    add_to_name_hash_table(entry);

                    if ( entry->parent ) {

                        entry->dir_next=NULL;
                        entry->dir_prev=NULL;

                        if ( entry->parent->child ) {

                            entry->parent->child->dir_prev=entry;
                            entry->dir_next=entry->parent->child;

                        }

                        entry->parent->child=entry;

                    }

                    copy_stat(&(entry->inode->st), &st);


                } else {

                    nreturn=-ENOMEM;
                    goto out;

                }

            }


        }

        if ( fullpath==1 ) {

            break;

        } else {

            /* make slash a slash again (was a \0) */
            *slash='/';
            path=slash+1;
            pentry=entry;
            pinode=entry->inode;

        }

    }

    mount_entry->entry=(void *) entry;
    entry->mount_entry=mount_entry;

    mount_entry->status=MOUNT_STATUS_UP;

    out:

    return nreturn;

}

/* function to update notifyfs when mounts are added and/or removed 
   some observations:
   - not every mountpoint (added or removed) is used by this fs
   - in case of an fs mounted by fs, it's possible that watches are already set on this fs has been
   set to "sleep" mode after is has been umounted before
   -   
   - note::: added and removed mounts are already sorted in the same way the main sort module works 
   in mountinfo.c:
   (mountpoint, mountsource, fstype)
   */

int update_notifyfs(struct mount_list_struct *added_mounts, struct mount_list_struct *removed_mounts, struct mount_list_struct *removed_mounts_keep)
{
    struct mount_entry_struct *mount_entry=NULL;
    struct notifyfs_entry_struct *entry;
    unsigned char doqueue=0;
    int nreturn=0, res;

    logoutput1("update_notifyfs");

    /* walk through removed mounts to see it affects the fs */

    res=lock_mountlist(MOUNTENTRY_REMOVED);

    /* start with last: the list is sorted, bigger/longer (also submounts) mounts are first this way */

    mount_entry=get_next_mount_entry(NULL, MOUNTENTRY_REMOVED);

    while (mount_entry) {


        entry=(struct notifyfs_entry_struct *) mount_entry->ptr;

        /* if normal umount then remove the tree... 
               if umounted by autofs then set tree to sleep */

        if ( entry ) {
            struct call_info_struct call_info;

            call_info.path=mount_entry->mountpoint;
            call_info.entry=entry;

            if ( mount_entry->autofs_mounted==1 ) {

                /* managed by autofs: put whole tree in sleep */

                changestate(&call_info, WATCH_ACTION_SLEEP);
                mount_entry->status=MOUNT_STATUS_SLEEP;

            } else {

                /* normal umount: remove whole tree */

                changestate(&call_info, WATCH_ACTION_REMOVE);
                mount_entry->status=MOUNT_STATUS_REMOVE;

            }

        } else {

            if ( mount_entry->autofs_mounted==1 ) {

                mount_entry->status=MOUNT_STATUS_SLEEP;

            } else {

                mount_entry->status=MOUNT_STATUS_REMOVE;

            }

        }

        mount_entry=get_next_mount_entry(mount_entry, MOUNTENTRY_REMOVED);

    }

    mount_entry=get_next_mount_entry(NULL, MOUNTENTRY_REMOVED_KEEP);

    while (mount_entry) {


	if ( mount_entry->processed==1 ) {

	    mount_entry=get_next_mount_entry(mount_entry, MOUNTENTRY_REMOVED_KEEP);
	    continue;

	}

        entry=(struct notifyfs_entry_struct *) mount_entry->ptr;

        /* if normal umount then remove the tree... 
               if umounted by autofs then set tree to sleep */

        if ( entry ) {
            struct call_info_struct call_info;

            call_info.path=mount_entry->mountpoint;
            call_info.entry=entry;

            if ( mount_entry->autofs_mounted==1 ) {

                /* managed by autofs: put whole tree in sleep */

                changestate(&call_info, WATCH_ACTION_SLEEP);
                mount_entry->status=MOUNT_STATUS_SLEEP;

            } else {

                /* normal umount: remove whole tree */

                changestate(&call_info, WATCH_ACTION_REMOVE);
                mount_entry->status=MOUNT_STATUS_REMOVE;

            }

        } else {

            if ( mount_entry->autofs_mounted==1 ) {

                mount_entry->status=MOUNT_STATUS_SLEEP;

            } else {

                mount_entry->status=MOUNT_STATUS_REMOVE;

            }

        }

	mount_entry->processed=1;

        mount_entry=get_next_mount_entry(mount_entry, MOUNTENTRY_REMOVED_KEEP);

    }

    res=unlock_mountlist(MOUNTENTRY_REMOVED);

    /* in any case the watches set on an autofs managed fs, get out of "sleep" mode */

    res=lock_mountlist(MOUNTENTRY_ADDED);

    mount_entry=get_next_mount_entry(NULL, MOUNTENTRY_ADDED);

    while (mount_entry) {
        struct notifyfs_entry_struct *entry=NULL;

	if ( !root_mount && strcmp(mount_entry->mountpoint, "/") == 0 ) {

	    root_mount=mount_entry;
	    root_mount->ptr=(void *) root_entry;
	    root_entry->ptr=(void *) root_mount;

	}

        entry=(struct notifyfs_entry_struct *) mount_entry->ptr;

        if ( entry ) {
            struct call_info_struct call_info;

            call_info.path=mount_entry->mountpoint;
            call_info.entry=entry;

            /* entry does exist already, there is a tree already related to this mount */
            /* walk through tree and wake up sleeping watches */

            changestate(&call_info, WATCH_ACTION_WAKEUP);

        } else {

	    if ( strcmp(mount_entry->mountpoint, "/") == 0 ) {

		/* skip the root... not necessary to create....*/
		goto next;

	    } else if ( strcmp(notifyfs_options.mountpoint, mount_entry->mountpoint)==0 ) {

		/* skip the mountpoint of this fs, caused deadlocks in the past... */
		goto next;

	    }

            nreturn=create_notifyfs_mount_path(mount_entry);

        }

	next:

        mount_entry=get_next_mount_entry(mount_entry, MOUNTENTRY_ADDED);

    }

    unlock:

    res=unlock_mountlist(MOUNTENTRY_ADDED);

}

static void init_notifyfs_mount_paths()
{
    struct mount_entry_struct *mount_entry=NULL;
    int res;

    res=lock_mountlist(MOUNTENTRY_CURRENT);

    mount_entry=get_next_mount_entry(NULL, MOUNTENTRY_CURRENT);

    while (mount_entry) {

        /* skip the root AND the mountpoint of this fs */

        if ( strcmp(mount_entry->mountpoint, "/") != 0 && strcmp(notifyfs_options.mountpoint, mount_entry->mountpoint)!=0 ) res=create_notifyfs_mount_path(mount_entry);

        mount_entry=get_next_mount_entry(mount_entry, MOUNTENTRY_CURRENT);

    }

    res=unlock_mountlist(MOUNTENTRY_CURRENT);

}


static void notifyfs_lookup(fuse_req_t req, fuse_ino_t parentino, const char *name)
{
    struct fuse_entry_param e;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    int nreturn=0;
    unsigned char entryexists=0, dostat=0;
    struct client_struct *client=NULL;
    struct call_info_struct call_info;

    logoutput1("LOOKUP, name: %s", name);

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("LOOKUP, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    init_call_info(&call_info, NULL);

    entry=find_entry(parentino, name);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    inode=entry->inode;

    call_info.entry=entry;

    entryexists=1;
    dostat=1;

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;


    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( call_info.mount_entry ) {
        struct mount_entry_struct *mount_entry;

        mount_entry=call_info.mount_entry;

        if ( mount_is_up(mount_entry)==0 ) {

            /* what with DFS like constructions ?? */
            dostat=0;

            /* here something with times and mount times?? */

            if ( inode->st.st_mode==0 ) dostat=1;

        }

    }

    /* if watch attached then not stat */

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;


    if ( dostat==1 ) {

        /* check entry on underlying fs */

        nreturn=lstat(call_info.path, &(e.attr));

        /* here copy e.attr to inode->st */

        if (nreturn!=-1) copy_stat(&inode->st, &(e.attr));

    } else {

        /* here copy the stat from inode->st */

        copy_stat(&(e.attr), &inode->st);
        nreturn=0;

    }

    out:

    if ( nreturn==-ENOENT) {

	logoutput2("lookup: entry does not exist (ENOENT)");

	e.ino = 0;
	e.entry_timeout = notifyfs_options.negative_timeout;

    } else if ( nreturn<0 ) {

	logoutput1("do_lookup: error (%i)", nreturn);

    } else {

	// no error

	entry->inode->nlookup++;
	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

	logoutput2("lookup return size: %zi", e.attr.st_size);

    }

    logoutput1("lookup: return %i", nreturn);

    if ( nreturn<0 ) {

	fuse_reply_err(req, -nreturn);

    } else {

        fuse_reply_entry(req, &e);

    }

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, WATCH_ACTION_REMOVE);

    }

    if ( call_info.path ) free(call_info.path);

}


static void notifyfs_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
    struct notifyfs_inode_struct *inode;

    inode = find_inode(ino);

    if ( ! inode ) goto out;

    logoutput1("FORGET");

    if ( inode->nlookup < nlookup ) {

	logoutput0("internal error: forget ino=%llu %llu from %llu", (unsigned long long) ino, (unsigned long long) nlookup, (unsigned long long) inode->nlookup);
	inode->nlookup=0;

    } else {

        inode->nlookup -= nlookup;

    }

    logoutput2("forget, current nlookup value %llu", (unsigned long long) inode->nlookup);

    out:

    fuse_reply_none(req);

}

static void notifyfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct stat st;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    int nreturn=0;
    struct client_struct *client=NULL;
    unsigned char entryexists=0, dostat;
    struct call_info_struct call_info;

    logoutput1("GETATTR");

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("GETATTR, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    init_call_info(&call_info, NULL);

    // get the inode and the entry, they have to exist

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    entryexists=1;
    call_info.entry=entry;

    dostat=1;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if (nreturn<0) goto out;


    if ( call_info.mount_entry ) {
        struct mount_entry_struct *mount_entry;

        mount_entry=call_info.mount_entry;

        if ( mount_is_up(mount_entry)==0 ) {

            /* what with DFS like constructions ?? */
            dostat=0;

            /* here something with times and mount times?? */

            if ( inode->st.st_mode==0 ) dostat=1;

        }

    }

    /* if watch attached then not stat */

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;


    if ( dostat==1 ) {


        /* get the stat from the underlying fs */

        nreturn=lstat(call_info.path, &st);

        /* copy the st -> inode->st */

        if ( nreturn!=-1 ) copy_stat(&inode->st, &st);

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);
        nreturn=0;

    }

    out:

    logoutput1("getattr, return: %i", nreturn);

    if (nreturn < 0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_attr(req, &st, notifyfs_options.attr_timeout);

    }

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, WATCH_ACTION_REMOVE);

    }

    if ( call_info.path ) free(call_info.path);

}

static void notifyfs_access (fuse_req_t req, fuse_ino_t ino, int mask)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct stat st;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    int nreturn=0,  res;
    struct client_struct *client=NULL;
    unsigned char entryexists=0, dostat;
    struct call_info_struct call_info;

    logoutput1("ACCESS, mask: %i", mask);

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("ACCESS, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    init_call_info(&call_info, NULL);

    // get the inode and the entry, they have to exist

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    call_info.entry=entry;
    entryexists=1;

    dostat=1;

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if (nreturn<0) goto out;


    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( call_info.mount_entry ) {
        struct mount_entry_struct *mount_entry;

        mount_entry=call_info.mount_entry;

        if ( mounted_is_up(mount_entry)==0 ) {

            /* what with DFS like constructions ?? */
            dostat=0;

            /* here something with times and mount times?? */

            if ( inode->st.st_mode==0 ) dostat=1;

        }

    }

    /* if watch attached then not stat */

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;


    if ( dostat==1 ) {


        /* get the stat from the root fs */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if (res!=-1) copy_stat(&inode->st, &st);

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);

        res=0;

    }

    if ( mask == F_OK ) {

        // check for existence

        if ( res == -1 ) {

            /* TODO: take action here cause the underlying entry does exist anymore */

            nreturn=-ENOENT;

        } else {

            nreturn=1;

        }

    } else {

        if ( res == -1 ) {

            /* TODO: take action here cause the underlying entry does exist anymore*/

            nreturn=-ENOENT;
            goto out;

        }

        nreturn=check_access(req, ctx, &st, mask);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */

        }

    }

    out:

    logoutput1("access, return: %i", nreturn);

    fuse_reply_err(req, abs(nreturn));

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, WATCH_ACTION_REMOVE);

    }

    if ( call_info.path ) free(call_info.path);

}



/* create a directory in notifyfs, only allowed when the directory does exist in the underlying fs */

static void notifyfs_mkdir(fuse_req_t req, fuse_ino_t parentino, const char *name, mode_t mode)
{
    struct fuse_entry_param e;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct notifyfs_entry_struct *entry;
    int nreturn=0;
    unsigned char entrycreated=0;
    struct client_struct *client=NULL;
    struct call_info_struct call_info;

    logoutput1("MKDIR, name: %s", name);

    /* check client access */

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("MKDIR, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    init_call_info(&call_info, NULL);

    entry=find_entry(parentino, name);

    if ( ! entry ) {
        struct notifyfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct notifyfs_entry_struct *pentry;

	    pentry=pinode->alias;
	    entry=create_entry(pentry, name, NULL);
	    entrycreated=1;

	    if ( !entry ) {

		nreturn=-ENOMEM; /* not able to create due to memory problems */
		entrycreated=0;
		goto error;

	    } 

	} else { 

	    nreturn=-EIO; /* parent inode not found !!?? some strange error */
	    goto error;

	}

    } else {

	/* here an error, the entry does exist already */

	nreturn=-EEXIST;
	goto error;

    }

    call_info.entry=entry;

    /* check r permissions of entry */

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    if (call_info->mount_entry) {

	if ( mount_is_up(call_info->mount_entry)==0 ) {

	    /* underlying mount is not mounted: creating is not permitted */

	    nreturn=-EACCES;
	    goto error;

	}

    }

    /* only create directory here when it does exist in the underlying fs */

    nreturn=lstat(call_info.path, &(e.attr));

    if ( nreturn==-1 ) {

        /* TODO additional action */

        nreturn=-EACCES; /* does not exist: not permitted */

    } else {

        if ( ! S_ISDIR(e.attr.st_mode) ) {

            /* TODO additional action */

            nreturn=-ENOTDIR; /* not a directory */ 
            goto out;

        }

        /* check read access : sufficient to create a directory in this fs ... */

        nreturn=check_access(req, ctx, &(e.attr), R_OK);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */

        }

    }


    out:

    if ( nreturn==0 ) {

	// no error

        assign_inode(entry);

        if ( ! entry->inode ) {

            nreturn=-ENOMEM;
            goto error;

        }

	entry->inode->nlookup++;

	copy_stat(&entry->inode->st, &(e.attr));

	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

        add_to_inode_hash_table(entry->inode);
        add_to_name_hash_table(entry);

        /* insert in directory (some lock required (TODO)????) */

        if ( entry->parent ) {

            entry->dir_next=NULL;
            entry->dir_prev=NULL;

            if (entry->parent->child) {

                entry->parent->child->dir_prev=entry;
                entry->dir_next=entry->parent->child;

            }

            entry->parent->child=entry;

        }

        logoutput2("mkdir successfull");

        fuse_reply_entry(req, &e);
        if ( call_info.path ) free(call_info.path);

        return;

    }


    error:


    logoutput1("mkdir: error %i", nreturn);

    if ( entrycreated==1 ) remove_entry(entry);

    e.ino = 0;
    e.entry_timeout = notifyfs_options.negative_timeout;

    fuse_reply_err(req, abs(nreturn));

    if ( call_info.path ) free(call_info.path);

}

/* create a node in notifyfs, only allowed when the node does exist in the underlying fs (with the same porperties)*/

static void notifyfs_mknod(fuse_req_t req, fuse_ino_t parentino, const char *name, mode_t mode, dev_t dev)
{
    struct fuse_entry_param e;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct notifyfs_entry_struct *entry;
    int nreturn=0;
    unsigned char entrycreated=0;
    struct client_struct *client=NULL;
    struct call_info_struct call_info;

    logoutput1("MKNOD, name: %s", name);

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("MKNOD, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    init_call_info(&call_info, NULL);

    entry=find_entry(parentino, name);

    if ( ! entry ) {
        struct notifyfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct notifyfs_entry_struct *pentry;

	    pentry=pinode->alias;
	    entry=create_entry(pentry, name, NULL);
	    entrycreated=1;

	    if ( !entry ) {

		nreturn=-ENOMEM; /* not able to create due to memory problems */
		entrycreated=0;
		goto error;

	    } 

	} else { 

	    nreturn=-EIO; /* parent inode not found !!?? some strange error */
	    goto error;

	}

    } else {

	/* here an error, the entry does exist already */

	nreturn=-EEXIST;
	goto error;

    }

    call_info.entry=entry;

    /* translate entry into path */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    /* TODO: check when underlying fs is "sleeping": no access allowed */

    if (call_info->mount_entry) {

	if ( mount_is_up(call_info->mount_entry)==0 ) {

	    /* underlying mount is not mounted: creating is not permitted */

	    nreturn=-EACCES;
	    goto error;

	}

    }

    /* only create directory here when it does exist in the underlying fs */

    nreturn=lstat(call_info.path, &(e.attr));

    if ( nreturn==-1 ) {

        nreturn=-ENOENT; /* does not exist */

    } else {

        if ( (e.attr.st_mode & S_IFMT ) != (mode & S_IFMT ) ) {

            nreturn=-ENOENT; /* not the same mode */ 
            goto out;

        }

        /* check read access : sufficient to create a directory in this fs ... */

        nreturn=check_access(req, ctx, &(e.attr), R_OK);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */

        }

    }


    out:

    if ( nreturn==0 ) {

	// no error

        assign_inode(entry);

        if ( ! entry->inode ) {

            nreturn=-ENOMEM;
            goto error;

        }

	entry->inode->nlookup++;

	copy_stat(&entry->inode->st, &(e.attr));

	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = notifyfs_options.attr_timeout;
	e.entry_timeout = notifyfs_options.entry_timeout;

        add_to_inode_hash_table(entry->inode);
        add_to_name_hash_table(entry);

        /* insert in directory (some lock required (TODO)????) */

        if ( entry->parent ) {

            entry->dir_next=NULL;
            entry->dir_prev=NULL;

            if (entry->parent->child) {

                entry->parent->child->dir_prev=entry;
                entry->dir_next=entry->parent->child;

            }

            entry->parent->child=entry;

        }

        logoutput2("mknod succesfull");

        fuse_reply_entry(req, &e);
        if ( call_info.path ) free(call_info.path);

        return;

    }

    error:

    logoutput1("mknod: error %i", nreturn);

    if ( entrycreated==1 ) remove_entry(entry);

    e.ino = 0;
    e.entry_timeout = notifyfs_options.negative_timeout;

    fuse_reply_err(req, abs(nreturn));

    if ( call_info.path ) free(call_info.path);

}


static inline struct notifyfs_generic_dirp_struct *get_dirp(struct fuse_file_info *fi)
{
    return (struct notifyfs_generic_dirp_struct *) (uintptr_t) fi->fh;
}

static void free_dirp(struct notifyfs_generic_dirp_struct *dirp)
{

    free(dirp);

}

//
// open a directory to read the contents
// here the backend is an audio cdrom, with only one root directory, and
// no subdirectories
//
// so in practice this will be called only for the root
// it's not required to build an extra check we're in the root, the
// VFS will never allow this function to be called on something else than a directory
//

/* here also add the contents of the underlying fs when there is a watch set */

static void notifyfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct notifyfs_generic_dirp_struct *dirp=NULL;
    int nreturn=0;
    struct notifyfs_entry_struct *entry;;
    struct notifyfs_inode_struct *inode;
    struct client_struct *client=NULL;
    struct call_info_struct *call_info=NULL;

    logoutput1("OPENDIR");

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("OPENDIR, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    /* register call */

    call_info=get_call_info(entry);

    if ( ! call_info ) {

        nreturn=-ENOMEM;
        goto out;

    }

    /* translate entry into path */

    nreturn=determine_path(call_info, NOTIFYFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    dirp = malloc(sizeof(struct notifyfs_generic_dirp_struct));

    if ( ! dirp ) {

	nreturn=-ENOMEM;
	goto out;

    }

    memset(dirp, 0, sizeof(struct notifyfs_generic_dirp_struct));

    dirp->entry=NULL;
    dirp->upperfs_offset=0;
    dirp->underfs_offset=1;
    dirp->call_info=call_info;

    dirp->generic_fh.entry=entry;

    // assign this object to fi->fh

    fi->fh = (unsigned long) dirp;


    out:

    if ( nreturn<0 ) {

        if ( call_info ) remove_call_info(call_info);
	if ( dirp ) free_dirp(dirp);

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_open(req, fi);

    }

    logoutput1("opendir, nreturn %i", nreturn);

}

static int do_readdir_localhost(fuse_req_t req, char *buf, size_t size, off_t upperfs_offset, struct notifyfs_generic_dirp_struct *dirp)
{
    size_t bufpos = 0;
    int res, nreturn=0;
    size_t entsize;
    bool direntryfrompreviousbatch=false;
    char *entryname=NULL;
    unsigned char namecreated=0;

    logoutput1("DO_READDIR, offset: %"PRId64, upperfs_offset);

    dirp->upperfs_offset=upperfs_offset;
    if ( dirp->upperfs_offset==0 ) dirp->upperfs_offset=1;

    if ( dirp->entry ) direntryfrompreviousbatch=true;


    while (bufpos < size ) {

        namecreated=0;

        if ( dirp->underfs_offset == 1 ) {

            /* the . entry */

            dirp->st.st_ino = dirp->generic_fh.entry->inode->ino;
            entryname=malloc(2);

            if ( entryname ) {

                sprintf(entryname, ".");
                namecreated=1;

            } else {

                nreturn=-ENOMEM;
                goto out;

            }

        } else if ( dirp->underfs_offset == 2 ) {

            /* the .. entry */

	    if (rootinode(dirp->generic_fh.entry->inode) == 1 ) {

	        dirp->st.st_ino = FUSE_ROOT_ID;

	    } else {

	        dirp->st.st_ino = dirp->generic_fh.entry->parent->inode->ino;

	    }

            entryname=malloc(3);

            if ( entryname ) {

                sprintf(entryname, "..");
                namecreated=1;

            } else {

                nreturn=-ENOMEM;
                goto out;

            }

        } else {


            if ( dirp->underfs_offset == 3 ) {

                /* first "normal" entry */

                dirp->entry=dirp->generic_fh.entry->child;

            } else {

                /* every next entry */

                if ( ! direntryfrompreviousbatch ) {

                    if ( dirp->entry ) dirp->entry=dirp->entry->dir_next;

                }

            }

            if ( dirp->entry ) {

                /* valid entry */

                dirp->st.st_ino = dirp->entry->inode->ino;
                dirp->st.st_mode = dirp->entry->inode->st.st_mode;

                entryname=dirp->entry->name;

            } else {

                /* no valid entry: end of stream */

                break;

            }

        }

	// break when buffer is not large enough
	// function fuse_add_direntry has not added it when buffer is too small to hold direntry, 
	// only returns the requested size

        entsize = fuse_add_direntry(req, buf + bufpos, size - bufpos, entryname, &dirp->st, dirp->upperfs_offset);

        if ( namecreated==1 ) {

            free(entryname);

        }

	if (entsize > size - bufpos) {

	    // the direntry does not fit in buffer
	    // (keep the current direntry and entry attached)
	    break;

	} else {

            dirp->upperfs_offset+=1;
            dirp->underfs_offset+=1;

        }

	bufpos += entsize;

    }


    out:

    if (nreturn<0) {

	logoutput1("do_readdir, return: %i", nreturn);

	return nreturn;

    } else {

	logoutput1("do_readdir, return: %zu", bufpos);

	return bufpos;

    }

}


static void notifyfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct notifyfs_generic_dirp_struct *dirp = get_dirp(fi);
    char *buf;
    int nreturn=0;

    logoutput1("READDIR, size: %zi", size);

    // look what readdir has to be called

    buf = malloc(size);

    if (buf == NULL) {

	nreturn=-ENOMEM;
	goto out;

    }

    nreturn=do_readdir_localhost(req, buf, size, offset, dirp);

    out:

    if (nreturn < 0 ) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_buf(req, buf, nreturn);

    }

    logoutput1("readdir, nreturn %i", nreturn);
    if ( buf ) free(buf);

}


static void notifyfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct notifyfs_generic_dirp_struct *dirp = get_dirp(fi);

    (void) ino;

    logoutput1("RELEASEDIR");

    fuse_reply_err(req, 0);

    if ( dirp->call_info ) remove_call_info(dirp->call_info);

    free_dirp(dirp);
    fi->fh=0;

}

static void notifyfs_statfs(fuse_req_t req, fuse_ino_t ino)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct statvfs st;
    int nreturn=0, res;
    struct notifyfs_entry_struct *entry; 
    struct notifyfs_inode_struct *inode;
    struct client_struct *client=NULL;

    logoutput1("STATFS");

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("STATFS, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ){

	nreturn=-ENOENT;
	goto out;

    }

    memset(&st, 0, sizeof(statvfs));

    /* should the statvfs be taken of the path or the root ?? */

    res=statvfs("/", &st);

    if ( res==0 ) {

	// take some values from the default

	/* note the fs does not provide opening/reading/writing of files, so info about blocksize etc
	   is useless, so do not override the default from the root */ 

	// st.f_bsize=4096; /* good?? */
	// st.f_frsize=st.f_bsize; /* no fragmentation on this fs */
	st.f_blocks=0;

	st.f_bfree=0;
	st.f_bavail=0;

	st.f_files=get_inoctr();
	st.f_ffree=UINT32_MAX - st.f_files ; /* inodes are of unsigned long int, 4 bytes:32 */
	st.f_favail=st.f_ffree;

	// do not know what to put here... just some default values... no fsid.... just zero

	st.f_fsid=0;
	st.f_flag=0;
	st.f_namemax=255;

    } else {

	nreturn=-errno;

    }

    out:

    if (nreturn==0) {

	fuse_reply_statfs(req, &st);

    } else {

        fuse_reply_err(req, nreturn);

    }

    logoutput1("statfs, B, nreturn: %i", nreturn);

}

static void notifyfs_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size, int flags)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    int nreturn=0, res;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    char *basexattr=NULL;
    struct client_struct *client=NULL;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0, dostat;


    logoutput1("SETXATTR");

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("SETXATTR, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    init_call_info(&call_info, NULL);

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    call_info.entry=entry;
    entryexists=1;

    /* translate entry to path..... and try to determine the backend */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    dostat=1;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( call_info.mount_entry ) {
        struct mount_entry_struct *mount_entry;

        mount_entry=call_info.mount_entry;

        if ( mounted_by_autofs(mount_entry)==1 ) {

	    if ( mount_is_up(mount_entry)==0 ) {

		/* when dealing with a sleeping mount, and it's not mounted no access */

		nreturn=-EACCES;
		goto out;

	    }

            /* what with DFS like constructions ?? */
            dostat=0;

            /* here something with times and mount times?? */

            if ( inode->st.st_mode==0 ) dostat=1;

        }

    }

    /* if watch attached then not stat */

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if ( res!=-1 ) copy_stat(&inode->st, &st);

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);

        res=0;

    }

    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

    } else {

        /* check read access */

        nreturn=check_access(req, ctx, &st, R_OK);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */
            goto out;

        }

    }

    // make this global....

    basexattr=malloc(strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3); /* plus _ and terminator */

    if ( ! basexattr ) {

        nreturn=-ENOMEM;
        goto out;

    }

    memset(basexattr, '\0', strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3);

    sprintf(basexattr, "system.%s_", XATTR_SYSTEM_NAME);

    // intercept the xattr used by the fs here and jump to the end

    if ( strlen(name) > strlen(basexattr) && strncmp(name, basexattr, strlen(basexattr))==0 ) {

	nreturn=setxattr4workspace(ctx, &call_info, name + strlen(basexattr), value);

    } else {

	nreturn=-ENOATTR;

    }

    out:

    if (nreturn<0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_err(req, 0);

    }

    logoutput1("setxattr, nreturn %i", nreturn);

    if ( basexattr ) free(basexattr);

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, WATCH_ACTION_REMOVE);

    }

    if ( call_info.path ) free(call_info.path);

}


static void notifyfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    int nreturn=0, nlen=0, res;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    void *value=NULL;
    struct xattr_workspace_struct *xattr_workspace;
    char *basexattr=NULL;
    struct client_struct *client=NULL;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0, dostat;

    logoutput1("GETXATTR, name: %s, size: %i", name, size);

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("GETXATTR, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    init_call_info(&call_info, NULL);

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    call_info.entry=entry;

    entryexists=1;

    /* translate entry to path..... */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    dostat=1;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( call_info.mount_entry ) {
        struct mount_entry_struct *mount_entry;

        mount_entry=call_info.mount_entry;

        if ( mounted_by_autofs(mount_entry)==1 ) {

            /* what with DFS like constructions ?? */
            dostat=0;

            /* here something with times and mount times?? */

            if ( inode->st.st_mode==0 ) dostat=1;

        }

    }

    /* if watch attached then not stat */

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if ( res!=-1) copy_stat(&inode->st, &st);

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);

        res=0;

    }


    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

    } else {

        /* check read access */

        nreturn=check_access(req, ctx, &st, R_OK);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */
            goto out;

        }

    }

    // make this global: this is always the same

    basexattr=malloc(strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3); /* plus _ and terminator */

    if ( ! basexattr ) {

        nreturn=-ENOMEM;
        goto out;

    }

    memset(basexattr, '\0', strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3);

    sprintf(basexattr, "system.%s_", XATTR_SYSTEM_NAME);

    if ( strlen(name) > strlen(basexattr) && strncmp(name, basexattr, strlen(basexattr))==0 ) {

	    // workspace related xattrs 
	    // (they begin with system. and follow the value of XATTR_SYSTEM_NAME and the a _)

	    xattr_workspace=malloc(sizeof(struct xattr_workspace_struct));

	    if ( ! xattr_workspace ) {

		nreturn=-ENOMEM;
		goto out;

	    }

	    memset(xattr_workspace, 0, sizeof(struct xattr_workspace_struct));

            // here pass only the relevant part? 

	    xattr_workspace->name=NULL;
	    xattr_workspace->size=size;
	    xattr_workspace->nerror=0;
	    xattr_workspace->value=NULL;
	    xattr_workspace->nlen=0;

	    getxattr4workspace(ctx, &call_info, name + strlen(basexattr), xattr_workspace);

	    if ( xattr_workspace->nerror<0 ) {

		nreturn=xattr_workspace->nerror;

		if ( xattr_workspace->value) {

	            free(xattr_workspace->value);
		    xattr_workspace->value=NULL;

                }

	    } else {

		nlen=xattr_workspace->nlen;
		if ( xattr_workspace->value ) value=xattr_workspace->value;

	    }

	    // free the tmp struct xattr_workspace
	    // note this will not free value, which is just a good thing
	    // it is used as reply overall

	    free(xattr_workspace);

    } else {

	nreturn=-ENOATTR;

    }

    out:

    if ( nreturn < 0 ) { 

	fuse_reply_err(req, -nreturn);

    } else {

	if ( size == 0 ) {

	    // reply with the requested bytes

	    fuse_reply_xattr(req, nlen);

            logoutput1("getxattr, fuse_reply_xattr %i", nlen);

	} else if ( nlen > size ) {

	    fuse_reply_err(req, ERANGE);

            logoutput1("getxattr, fuse_reply_err ERANGE");

	} else {

	    // reply with the value

	    fuse_reply_buf(req, value, strlen(value));

            logoutput1("getxattr, fuse_reply_buf value %s", value);

	}

    }

    logoutput1("getxattr, nreturn: %i, nlen: %i", nreturn, nlen);

    if ( value ) free(value);
    if ( basexattr ) free(basexattr);

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path ) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, WATCH_ACTION_REMOVE);

    }

    if ( call_info.path ) free(call_info.path);

}




static void notifyfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    ssize_t nlenlist=0;
    int nreturn=0, res;
    char *list=NULL;
    struct notifyfs_entry_struct *entry;
    struct notifyfs_inode_struct *inode;
    struct client_struct *client=NULL;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0, dostat;

    logoutput1("LISTXATTR, size: %li", (long) size);

    client=lookup_client(ctx->pid, 0);

    if ( notifyfs_options.accessmode!=0 ) {
        unsigned char accessdeny=1;

        /* check access */

        if ( (notifyfs_options.accessmode & NOTIFYFS_ACCESS_ROOT) && ctx->uid==0 ) accessdeny=0;

        if ( accessdeny==1 && (notifyfs_options.accessmode & NOTIFYFS_ACCESS_CLIENT) && client ) accessdeny=0;

        if ( accessdeny==1 ) {

            logoutput1("LISTXATTR, access denied for pid %i", (int) ctx->pid);

            nreturn=-EACCES;
            goto out;

        }

    }

    init_call_info(&call_info, NULL);

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    call_info.entry=entry;
    entryexists=1;

    /* translate entry to path..... */

    nreturn=determine_path(&call_info, NOTIFYFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    dostat=1;

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    if ( call_info.mount_entry ) {
        struct mount_entry_struct *mount_entry;

        mount_entry=call_info.mount_entry;

        if ( mounted_by_autofs(mount_entry)==1 ) {

            /* what with DFS like constructions ?? */
            dostat=0;

            /* here something with times and mount times?? */

            if ( inode->st.st_mode==0 ) dostat=1;

        }

    }

    /* if watch attached then not stat */

    if ( inode->effective_watch && inode->effective_watch->nrwatches>0 ) dostat=0;

    if ( dostat==1 ) {

        /* user must have read access */

        res=lstat(call_info.path, &st);

        /* copy the st to inode->st */

        if (res!=-1) copy_stat(&inode->st, &st);

    } else {

        /* copy inode->st to st */

        copy_stat(&st, &inode->st);

        res=0;

    }

    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

    } else {

        /* check read access */

        nreturn=check_access(req, ctx, &st, R_OK);

        if ( nreturn==1 ) {

            nreturn=0; /* grant access */

        } else if ( nreturn==0 ) {

            nreturn=-EACCES; /* access denied */
            goto out;

        }

    }

    if ( nreturn==0 ) {

	if ( size>0 ) {

	    // just create a list with the overall size

	    list=malloc(size);

	    if ( ! list ) {

		nreturn=-ENOMEM;
		goto out;

	    } else {

                memset(list, '\0', size);

            }

	}

	nlenlist=listxattr4workspace(ctx, &call_info, list, size);

	if ( nlenlist<0 ) {

	    // some error
	    nreturn=nlenlist;
	    goto out;

	}

    }

    out:

    // some checking

    if ( nreturn==0 ) {

        if ( size>0 ) {

            if ( nlenlist > size ) {

                nreturn=-ERANGE;

            }

        }

    }

    if ( nreturn != 0) {

	fuse_reply_err(req, abs(nreturn));

    } else {

	if ( size == 0 ) {

	    // reply with the requested size

	    fuse_reply_xattr(req, nlenlist);

	} else {

	    // here a security check the list exists??

	    fuse_reply_buf(req, list, size);

	    // should the list be freed ???

	}

    }

    // if ( list ) free(list);
    logoutput1("listxattr, nreturn: %i, nlenlist: %i", nreturn, nlenlist);

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, WATCH_ACTION_REMOVE);

    }

    if ( call_info.path ) free(call_info.path);

}


void create_pid_file()
{
    char *tmpchar=getenv("RUNDIR");

    if ( tmpchar ) {
        char *buf;

        snprintf(notifyfs_options.pidfile, PATH_MAX, "%s/notifyfs.pid", tmpchar);

        buf=malloc(20);

        if ( buf ) {
            struct stat st;
            int fd=0,res;

	    memset(buf, '\0', 20);
	    sprintf(buf, "%d", getpid()); 

	    logoutput1("storing pid: %s in %s", buf, notifyfs_options.pidfile);

	    res=stat(notifyfs_options.pidfile, &st);

	    if ( S_ISREG(st.st_mode) ) {

	        fd = open(notifyfs_options.pidfile, O_RDWR | O_EXCL | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	    } else {

	        fd = open(notifyfs_options.pidfile, O_RDWR | O_EXCL | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	    }


	    if ( fd>0 ) {

	        res=write(fd, buf, strlen(buf));

	        close(fd);

	    }

	    free(buf);

        }

    }

}


void remove_pid_file()
{

    if ( strlen(notifyfs_options.pidfile)>0 ) {
        struct stat st;
        int res;

	res=stat(notifyfs_options.pidfile, &st);

	if ( res!=-1 && S_ISREG(st.st_mode) ) {

	    logoutput1("Pid file %s found, removing it.", notifyfs_options.pidfile);

	    res=unlink(notifyfs_options.pidfile);

	}

    }
}



static void notifyfs_init (void *userdata, struct fuse_conn_info *conn)
{

    // create a pid file

    create_pid_file();

    // init_notifyfs_mount_paths();

}


static void notifyfs_destroy (void *userdata)
{

    // remove pid file

    remove_pid_file();

}

/* function to inform waiting clients interested in a specific watch about an event
*/

void send_message_clients(struct notifyfs_inode_struct *inode, int mask, int len, char *name)
{
    struct effective_watch_struct *effective_watch;

    /* walk through watches/clients attached to inode */

    /* here lock the effective watch.... */

    effective_watch=inode->effective_watch;

    if ( effective_watch ) {
        struct watch_struct *watch=effective_watch->watches;
        int effmask, res;

        while(watch) {

            if (watch->client) {

                /* what will be sent is the combination of what happened and the client is interested in */

                effmask=(watch->mask & mask);

                if ( effmask>0 ) {

                    /* send client data */

                    /* here something like send_inotify_message_client(client, reference of wd, effmask) 
                    what is missing here is a reference of the wd which makes sense to the client
                    obviously the client must have a reference back when reporting an event
                    it's not that easy cause the watch has been initiated/set using xattr
                    maybe just use the path, or use a unique id per client, also to be get
                    via xattr (system.inotifyfs_watchid)
                    */

                    res=send_notify_message(watch->client->fd, NOTIFYFS_MESSAGE_TYPE_NOTIFYAPP, watch->id, mask, len, name);

                    if (res<0) logoutput0("Error (%i) sending inotify event to client on fd %i.", abs(res), watch->client->fd);

                }

            }

            watch=watch->next_per_watch;

        }

    }

}

/* INOTIFY BACKEND SPECIFIC CALLS */

typedef struct INTEXTMAP {
                const char *name;
                unsigned int mask;
                } INTEXTMAP;

static const INTEXTMAP inotify_textmap[] = {
            { "IN_ACCESS", IN_ACCESS},
            { "IN_MODIFY", IN_MODIFY},
            { "IN_ATTRIB", IN_ATTRIB},
            { "IN_CLOSE_WRITE", IN_CLOSE_WRITE},
            { "IN_CLOSE_NOWRITE", IN_CLOSE_NOWRITE},
            { "IN_OPEN", IN_OPEN},
            { "IN_MOVED_FROM", IN_MOVED_FROM},
            { "IN_MOVED_TO", IN_MOVED_TO},
            { "IN_CREATE", IN_CREATE},
            { "IN_DELETE", IN_DELETE},
            { "IN_DELETE_SELF", IN_DELETE_SELF},
            { "IN_MOVE_SELF", IN_MOVE_SELF},
            { "IN_ONLYDIR", IN_ONLYDIR},
            { "IN_DONT_FOLLOW", IN_DONT_FOLLOW},
            { "IN_EXCL_UNLINK", IN_EXCL_UNLINK},
            { "IN_MASK_ADD", IN_MASK_ADD},
            { "IN_ISDIR", IN_ISDIR},
            { "IN_Q_OVERFLOW", IN_Q_OVERFLOW},
            { "IN_UNMOUNT", IN_UNMOUNT}};


static int print_mask(unsigned int mask, char *string, size_t size)
{
    int i, pos=0, len;

    for (i=0;i<(sizeof(inotify_textmap)/sizeof(inotify_textmap[0]));i++) {

        if ( inotify_textmap[i].mask & mask ) {

            len=strlen(inotify_textmap[i].name);

            if ( pos + len + 1  > size ) {

                pos=-1;
                goto out;

            } else {

                if ( pos>0 ) {

                    *(string+pos)=';';
                    pos++;

                }

                strcpy(string+pos, inotify_textmap[i].name);
                pos+=len;

            }

        }

    }

    out:

    return pos;

}


/* read data from inotify fd */
#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUFF_LEN (1024 * (INOTIFY_EVENT_SIZE + 16))

void *handle_data_on_inotify_fd(struct epoll_event *epoll_event)
{
    int nreturn=0;
    char outputstring[256];

    logoutput1("handle_data_on_inotify_fd.");

    if ( epoll_event->events & EPOLLIN ) {
        struct epoll_extended_data_struct *epoll_xdata;
        int lenread=0;
        char buff[INOTIFY_BUFF_LEN];

        /* get the epoll_xdata from event */

        epoll_xdata=(struct epoll_extended_data_struct *) epoll_event->data.ptr;

        lenread=read(epoll_xdata->fd, buff, INOTIFY_BUFF_LEN);

        if ( lenread<0 ) {

            logoutput0("Error (%i) reading inotify events (fd: %i).", errno, epoll_xdata->fd);

        } else {
            int i=0, res;
            struct inotify_event *i_event;
            struct effective_watch_struct *effective_watch;


            while(i<lenread) {

                i_event = (struct inotify_event *) &buff[i];

                /* handle overflow here */

                if ( (i_event->mask & IN_Q_OVERFLOW) && i_event->wd==-1 ) {

                    /* what to do here: read again?? go back ??*/

                    logoutput0("Error reading inotify events: buffer overflow.");
                    continue;

                }

                /* here: activity on a certain wd */

                /* lookup watch wd in table
                   lookup inode
                   lookup watches->clients
                   send message to clients

                    eventually take right action when something is deleted

                 */

                /* lookup watch using this wd */

                logoutput1("Received an inotify event on wd %i.", i_event->wd);

                memset(outputstring, '\0', 256);
                res=print_mask(i_event->mask, outputstring, 256);

                if ( res>0 ) {

                    logoutput2("Mask: %i/%s", i_event->mask, outputstring);

                } else {

                    logoutput2("Mask: %i", i_event->mask);

                }

                effective_watch=lookup_watch(BACKEND_METHOD_INOTIFY, i_event->wd);

                if ( effective_watch ) {
                    struct notifyfs_entry_struct *entry=NULL;

                    if ( i_event->name && i_event->len>0 ) {

                        /* something happens on an entry in the directory.. check it's in use
                           by this fs, the find command will return NULL if it isn't */

                        entry=find_entry(effective_watch->inode->ino, i_event->name);

                    } else {

                        entry=effective_watch->inode->alias;

                    }

                    if ( entry ) {
                        struct call_info_struct call_info;

                        init_call_info(&call_info, entry);

                        /* translate entry to path.....(force) */

                        nreturn=determine_path(&call_info, NOTIFYFS_PATH_NONE);
                        if (nreturn<0) continue;

                        if ( notifyfs_options.testmode==0 ) {

                            /* send the clients interested in this watch to required info */

                            send_message_clients(effective_watch->inode, i_event->mask, i_event->len, i_event->name);

                        }

                        /* in case of delete, move or unmount: update fs */

                        if ( i_event->mask & ( IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM ) ) {

                            /* something is deleted/moved from in the directory or the directory self */

                            /* remove entry, and if directory and contents, do that recursive
                               also remove any watch set there, notify clients about that 
                               and invalidate the removed entries/inodes
                            */

                            /* explicitly deny unmount cause they are handled by the mountmonitor!!! */

                            if ( ! (i_event->mask & IN_UNMOUNT) ) changestate(&call_info, WATCH_ACTION_REMOVE);

                        } else {

			    cachechanges(&call_info, i_event->mask);

			}

                    } else {

                        /* entry not found... */

                        if ( i_event->name && i_event->len>0 ) {

                            if ( effective_watch->inode->alias ) {
                                struct call_info_struct call_info;

				call_info.entry=effective_watch->inode->alias;

                                /* translate entry to path.....(force) */

                                nreturn=determine_path(&call_info, NOTIFYFS_PATH_FORCE);
                                if (nreturn<0) continue;

                                memset(outputstring, '\0', 256);
                                res=print_mask(i_event->mask, outputstring, 256);

                                if ( res>0 ) {

                                    logoutput1("Inotify event on entry not managed by this fs: %s/%s:%i/%s", call_info.path, i_event->name, i_event->mask, outputstring);

                                } else {

                                    logoutput1("Inotify event on entry not managed by this fs: %s/%s:%i", call_info.path, i_event->name, i_event->mask);

                                }

                            } else {

                                logoutput0("Error....inotify event received but entry not found....");

                            }

                        } else {

                            logoutput0("Error....inotify event received.. entry not found");

                        }

                    }

                }

                i += INOTIFY_EVENT_SIZE + i_event->len;

            }

        }

    }

}


static struct fuse_lowlevel_ops notifyfs_oper = {
	.init		= notifyfs_init,
	.destroy	= notifyfs_destroy,
	.lookup		= notifyfs_lookup,
	.forget		= notifyfs_forget,
	.getattr	= notifyfs_getattr,
	.access         = notifyfs_access,
	.mkdir          = notifyfs_mkdir,
	.mknod          = notifyfs_mknod,
	.opendir	= notifyfs_opendir,
	.readdir	= notifyfs_readdir,
	.releasedir	= notifyfs_releasedir,
	.statfs		= notifyfs_statfs,
	.setxattr	= notifyfs_setxattr,
	.getxattr	= notifyfs_getxattr,
	.listxattr	= notifyfs_listxattr,
};


int main(int argc, char *argv[])
{
    struct fuse_args notifyfs_args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_session *notifyfs_session;
    char *notifyfs_mountpoint;
    int foreground=0;
    int res, epoll_fd, socket_fd, inotify_fd, mountinfo_fd;
    struct stat st;
    pthread_t threadid_mountmonitor=0;


    umask(0);

    // set logging

    openlog("fuse.notifyfs", 0,0); 

    // clear commandline options

    notifyfs_commandline_options.socket=NULL;
    notifyfs_commandline_options.logging=1;
    notifyfs_commandline_options.accessmode=0;
    notifyfs_commandline_options.testmode=0;


    // set defaults

    notifyfs_options.logging=0;
    notifyfs_options.accessmode=0;
    notifyfs_options.testmode=0;
    notifyfs_options.logarea=0;
    memset(notifyfs_options.socket, '\0', UNIX_PATH_MAX);
    memset(notifyfs_options.pidfile, '\0', PATH_MAX);


    // read commandline options

    res = fuse_opt_parse(&notifyfs_args, &notifyfs_commandline_options, notifyfs_help_options, notifyfs_options_output_proc);

    if (res == -1) {

	fprintf(stderr, "Error parsing options.\n");
	exit(1);

    }

    res = fuse_opt_insert_arg(&notifyfs_args, 1, "-oallow_other,nodev,nosuid");


    // socket

    if ( notifyfs_commandline_options.socket ) {

	res=stat(notifyfs_commandline_options.socket, &st);

	if ( res!=-1 ) {

	    fprintf(stdout, "Socket %s does exist, will remove it.\n", notifyfs_commandline_options.socket);

	}

        if ( strlen(notifyfs_commandline_options.socket) >= UNIX_PATH_MAX ) {

	    fprintf(stderr, "Length of socket %s is too big.\n", notifyfs_commandline_options.socket);
	    exit(1);

	}

	unslash(notifyfs_commandline_options.socket);
	strcpy(notifyfs_options.socket, notifyfs_commandline_options.socket);

	fprintf(stdout, "Taking socket %s.\n", notifyfs_options.socket);

    } else {

        /* default */

        res=snprintf(notifyfs_options.socket, UNIX_PATH_MAX, "/var/run/notifyfs.sock");

    }

    res=unlink(notifyfs_options.socket); /* remove existing socket */

    //
    // init the name and inode hashtables
    //

    res=init_hashtables();

    if ( res<0 ) {

	fprintf(stderr, "Error, cannot intialize the hashtables (error: %i).\n", abs(res));
	exit(1);

    }

    //
    // create the root inode and entry
    // (and the root directory data)
    //

    root_entry=create_entry(NULL,".",NULL);

    if ( ! root_entry ) {

	fprintf(stderr, "Error, failed to create the root entry.\n");
	exit(1);

    }

    assign_inode(root_entry);

    if ( ! root_entry->inode ) {

	fprintf(stderr, "Error, failed to create the root inode.\n");
	exit(1);

    }

    add_to_inode_hash_table(root_entry->inode);


    //
    // set default options
    //

    if ( notifyfs_commandline_options.logging>0 ) notifyfs_options.logging=notifyfs_commandline_options.logging;

    if ( notifyfs_commandline_options.logarea>0 ) notifyfs_options.logarea=notifyfs_commandline_options.logarea;
    notifyfs_options.accessmode=notifyfs_commandline_options.accessmode;
    notifyfs_options.testmode=notifyfs_commandline_options.testmode;

    loglevel=notifyfs_options.logging;
    logarea=notifyfs_options.logarea;

    notifyfs_options.attr_timeout=1.0;
    notifyfs_options.entry_timeout=1.0;
    notifyfs_options.negative_timeout=1.0;

    res = -1;

    if (fuse_parse_cmdline(&notifyfs_args, &notifyfs_mountpoint, NULL, &foreground) == -1 ) {

        logoutput0("Error parsing options.");
        goto out;

    }

    notifyfs_options.mountpoint=notifyfs_mountpoint;

    if ( (notifyfs_chan = fuse_mount(notifyfs_mountpoint, &notifyfs_args)) == NULL) {

        logoutput0("Error mounting and setting up a channel.");
        goto out;

    }

    notifyfs_session=fuse_lowlevel_new(&notifyfs_args, &notifyfs_oper, sizeof(notifyfs_oper), NULL);

    if ( notifyfs_session == NULL ) {

        logoutput0("Error starting a new session.");
        goto out;

    }

    res = fuse_daemonize(foreground);

    if ( res!=0 ) {

        logoutput0("Error daemonize.");
        goto out;

    }

    fuse_session_add_chan(notifyfs_session, notifyfs_chan);

    epoll_fd=init_mainloop();

    if ( epoll_fd<0 ) {

        logoutput0("Error creating epoll fd: %i.", epoll_fd);
        goto out;

    }

    /*
        create the socket clients can connect to

    */

    socket_fd=create_socket(notifyfs_options.socket);

    if ( socket_fd<=0 ) {

        logoutput0("Error creating socket fd: %i.", socket_fd);
        goto out;

    }

    /* add socket to epoll */

    res=add_to_epoll(socket_fd, EPOLLIN, TYPE_FD_SOCKET, &handle_data_on_socket_fd, NULL);
    if ( res<0 ) {

        logoutput0("Error adding socket fd to epoll: %i.", res);
        goto out;

    } else {

        logoutput0("socket fd %i added to epoll", socket_fd);

    }

    notifyfs_options.socket_fd=socket_fd;


    /*
    *    add a inotify instance to epoll : default backend 
    *
    */

    /* create the inotify instance */

    inotify_fd=inotify_init();

    if ( inotify_fd<=0 ) {

        logoutput0("Error creating inotify fd: %i.", errno);
        goto out;

    }

    /* add inotify to epoll */

    res=add_to_epoll(inotify_fd, EPOLLIN, TYPE_FD_INOTIFY, &handle_data_on_inotify_fd, NULL);
    if ( res<0 ) {

        logoutput0("Error adding inotify fd to epoll: %i.", res);
        goto out;

    } else {

        logoutput0("inotify fd %i added to epoll", inotify_fd);

    }

    notifyfs_options.inotify_fd=inotify_fd;


    /* 
    *     add mount info 
    */

    mountinfo_fd=open(MOUNTINFO, O_RDONLY);

    if ( mountinfo_fd==-1 ) {

        logoutput0("unable to open file %s", MOUNTINFO);
        goto out;

    }

    res=add_to_epoll(mountinfo_fd, EPOLLERR, TYPE_FD_MOUNTINFO, &signal_mountmonitor, NULL);

    if ( res<0 ) {

        logoutput0("error adding mountinfo fd %i to epoll(error: %i)", mountinfo_fd, res);
        goto out;

    } else {

        logoutput0("mountinfo fd %i added to epoll", mountinfo_fd);

    }

    // read the mounttable to start

    // res=get_new_mount_list(NULL);
    // set_parents_raw(NULL);
    // init_notifyfs_mount_paths();

    /* connect the root entry and the root mount */

    // root_mount=get_root_mount();

    /* start the special mount monitor thread */

    res=start_mountmonitor_thread(&threadid_mountmonitor);

    if ( res<0 ) {

        logoutput0("error (%i) starting mountmonitor thread", res);

    } else {

        logoutput0("thread for mountmonitor started");

    }

    /* signal the mountmonitor to do the initial reading of the mounttable */

    signal_mountmonitor(NULL);

    read_fstab();

    /* more backends ?? and how to connect */

    res=fuse_session_loop_epoll_mt(notifyfs_session);

    fuse_session_remove_chan(notifyfs_chan);

    fuse_session_destroy(notifyfs_session);

    fuse_unmount(notifyfs_mountpoint, notifyfs_chan);

    out:

    if ( threadid_mountmonitor ) pthread_cancel(threadid_mountmonitor);

    fuse_opt_free_args(&notifyfs_args);

    closelog();

    return res ? 1 : 0;

}
