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
#include "testfs.h"

#include "fuse-loop-epoll-mt.h"

#include "utils.h"
#include "options.h"
#include "xattr.h"
#include "socket.h"

pthread_mutex_t changestate_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t changestate_condition=PTHREAD_COND_INITIALIZER;

struct call_info_struct *changestate_call_info_first=NULL;
struct call_info_struct *changestate_call_info_last=NULL;

struct testfs_commandline_options_struct testfs_commandline_options;
struct testfs_options_struct testfs_options;

struct fuse_opt testfs_help_options[] = {
     TESTFS_OPT("--notifyfssocket=%s",		notifyfssocket, 0),
     TESTFS_OPT("notifyfssocket=%s",		notifyfssocket, 0),
     TESTFS_OPT("logging=%i",			logging, 1),
     TESTFS_OPT("--logging=%i",			logging, 1),
     TESTFS_OPT("logarea=%i",			logarea, 0),
     TESTFS_OPT("--logarea=%i",			logarea, 0),
     FUSE_OPT_KEY("-V",            		KEY_VERSION),
     FUSE_OPT_KEY("--version",      		KEY_VERSION),
     FUSE_OPT_KEY("-h",             		KEY_HELP),
     FUSE_OPT_KEY("--help",         		KEY_HELP),
     FUSE_OPT_END
};


static struct fuse_chan *testfs_chan;
struct testfs_entry_struct *root_entry;

unsigned char loglevel=0;
unsigned char logarea=0;

void cachechanges(struct call_info_struct *call_info, int mask)
{
    int res;

    if ( call_info->entry ) {

	if ( ! call_info->path ) {

	    res=determine_path(call_info, TESTFS_PATH_FORCE);
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

        res=pthread_mutex_lock(&effective_watch->lock_mutex);

        if ( effective_watch->lock==1 ) {

            while (effective_watch->lock==1) {

                res=pthread_cond_wait(&effective_watch->lock_condition, &effective_watch->lock_mutex);

            }

        }

        effective_watch->lock=1;

        /* after here: the effective watch is locked by this thread */

        /* backend specific */

        if ( typeaction==WATCH_ACTION_REMOVE ) {

            res=inotify_rm_watch(testfs_options.inotify_fd, effective_watch->id);

            logoutput2("changestate_effective_watch: removing watch %i.", effective_watch->id);


        } else if ( typeaction==WATCH_ACTION_WAKEUP ) {

            /* wake up a backend specific watch again */

            res=inotify_add_watch(testfs_options.inotify_fd, path, effective_watch->mask);

            if (res>0 ) {

                effective_watch->id=res;
                logoutput2("changestate_effective_watch: setting watch %i on %s.", effective_watch->id, path);

            } else {

                logoutput0("changestate_effective_watch: setting watch %i on %s.", effective_watch->id, path);

            }

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


static void changestate_recursive(struct testfs_entry_struct *entry, char *path, int pathlen, unsigned char typeaction)
{
    int res, nlen, pathlen_orig=pathlen;

    logoutput2("changestate_recursive: entry %s", entry->name);

    if ( entry->child ) {
        struct testfs_entry_struct *child_entry=entry->child;
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

    if ( entry->inode ) {

        /* set path back to original state */

        *(path+pathlen_orig)='\0';

        res=changestate_effective_watch(entry->inode, 0, typeaction, path);

    }

    if ( typeaction==WATCH_ACTION_REMOVE ) {

        remove_entry_from_name_hash(entry);

        if ( entry->parent ) {

            /* send invalidate entry */
            /* use notify_delete or notify_inval_entry ?? */

            res=fuse_lowlevel_notify_delete(testfs_chan, entry->parent->inode->ino, entry->inode->ino, entry->name, strlen(entry->name));

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

unsigned char queue_changestate(struct testfs_entry_struct *entry, char *path1)
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

                if ( strncmp(path1+len2, "/", 1)==0 && strncmp(path2, path1, len2)==0 ) {

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

static void testfs_lookup(fuse_req_t req, fuse_ino_t parentino, const char *name)
{
    struct fuse_entry_param e;
    struct testfs_entry_struct *entry;
    struct testfs_inode_struct *inode;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    int nreturn=0;
    unsigned char entryexists=0;
    struct call_info_struct call_info;

    logoutput1("LOOKUP, name: %s", name);

    init_call_info(&call_info, NULL);

    entry=find_entry(parentino, name);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    inode=entry->inode;

    call_info.entry=entry;

    entryexists=1;

    /* translate entry into path */

    nreturn=determine_path(&call_info, TESTFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    /* check entry on underlying fs */

    nreturn=lstat(call_info.path, &(e.attr));

    /* here copy e.attr to inode->st */

    if (nreturn!=-1) copy_stat(&inode->st, &(e.attr));

    out:

    if ( nreturn==-ENOENT) {

	logoutput2("lookup: entry does not exist (ENOENT)");

	e.ino = 0;
	e.entry_timeout = testfs_options.negative_timeout;

    } else if ( nreturn<0 ) {

	logoutput1("do_lookup: error (%i)", nreturn);

    } else {

	// no error

	entry->inode->nlookup++;
	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = testfs_options.attr_timeout;
	e.entry_timeout = testfs_options.entry_timeout;

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


static void testfs_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
    struct testfs_inode_struct *inode;

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

static void testfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct stat st;
    struct testfs_entry_struct *entry;
    struct testfs_inode_struct *inode;
    int nreturn=0;
    unsigned char entryexists=0;
    struct call_info_struct call_info;

    logoutput1("GETATTR");

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

    /* if dealing with an autofs managed fs do not stat
       but what to do when there is no cached stat??
       caching???
    */

    /* translate entry into path */

    nreturn=determine_path(&call_info, TESTFS_PATH_NONE);
    if (nreturn<0) goto out;

    /* get the stat from the underlying fs */

    nreturn=lstat(call_info.path, &st);

    /* copy the st -> inode->st */

    if ( nreturn!=-1 ) copy_stat(&inode->st, &st);

    out:

    logoutput1("getattr, return: %i", nreturn);

    if (nreturn < 0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_attr(req, &st, testfs_options.attr_timeout);

    }

    /* post reply action */

    if ( nreturn==-ENOENT && call_info.path) {

        /* entry in this fs exists but underlying entry not anymore */

        if ( entryexists==1 ) changestate(&call_info, WATCH_ACTION_REMOVE);

    }

    if ( call_info.path ) free(call_info.path);

}

int readlink_localhost(struct call_info_struct *call_info, char **link)
{
    int size = PATH_MAX;
    char *buf=NULL;
    int res, nreturn=0;

    logoutput2("readlink_localhost");

    buf = malloc(size);

    if ( ! buf ) {

	nreturn=-ENOMEM;
	goto out;

    }

    do {

    	res = readlink(call_info->path, buf, size);

	if ( res==-1) {
	    nreturn=-errno;
	    free(buf);
	    break;
	}

	if (res < size) {

	    buf[res] = '\0';
	    *link = buf;
	    break;

	}

	// not large enough: double the size and try it again...

	size *= 2;

	buf=realloc(buf, size);

	if ( buf==NULL ) {

	    nreturn=-ENOMEM;
	    break;

	}

    } while (true);


    out:

    logoutput2("readlink_localhost, return: %i", nreturn);

    return nreturn;

}


void testfs_readlink(fuse_req_t req, fuse_ino_t ino)
{
    struct testfs_inode_struct *inode;
    struct testfs_entry_struct *entry;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    char *link=NULL;
    int nreturn=0;
    struct call_info_struct call_info;
    unsigned char entryexists=0;


    logoutput1("READLINK");

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

    init_call_info(&call_info, NULL);

    call_info.entry=entry;
    entryexists=1;

    nreturn=determine_path(&call_info, TESTFS_PATH_NONE);
    if (nreturn<0) goto out;

    nreturn = readlink_localhost(&call_info, &link);

    out:

    logoutput1("readlink, return %i", nreturn);

    if (nreturn < 0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_readlink(req, link);

    }

    if (link) free(link);

}

static inline struct testfs_generic_dirp_struct *get_dirp(struct fuse_file_info *fi)
{
    return (struct testfs_generic_dirp_struct *) (uintptr_t) fi->fh;
}

static void free_dirp(struct testfs_generic_dirp_struct *dirp)
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

static void testfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    struct testfs_generic_dirp_struct *dirp=NULL;
    int nreturn=0;
    struct testfs_entry_struct *entry;;
    struct testfs_inode_struct *inode;
    struct call_info_struct *call_info=NULL;

    logoutput1("OPENDIR");

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

    nreturn=determine_path(call_info, TESTFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    dirp = malloc(sizeof(struct testfs_generic_dirp_struct));

    if ( ! dirp ) {

	nreturn=-ENOMEM;
	goto out;

    }

    memset(dirp, 0, sizeof(struct testfs_generic_dirp_struct));

    dirp->dp=opendir(call_info->path);

    if ( ! dirp->dp ) {

	nreturn=-errno;
	goto out;

    }

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

int get_direntry_stat(struct testfs_generic_dirp_struct *generic_dirp, struct stat *st)
{
    int nreturn=0;
    unsigned char entrycreated=0;
    char *name;

    // necessary??
    // memset(st, 0, sizeof(struct stat));

    st->st_mode = generic_dirp->direntry->d_type << 12;
    name=generic_dirp->direntry->d_name;

    if (strcmp(name, ".") == 0) {

	st->st_ino = generic_dirp->generic_fh.entry->inode->ino;

    } else if (strcmp(name, "..") == 0) {

	if (generic_dirp->generic_fh.entry->inode->ino == FUSE_ROOT_ID) {

	    st->st_ino = FUSE_ROOT_ID;

	} else {

	    st->st_ino = generic_dirp->generic_fh.entry->parent->inode->ino;

	}

    } else  {

	//
	// "normal entry": look there is already an entry for this (in this fuse fs)
	//

	if ( generic_dirp->entry ) {

	    // check the current dirp->entry is the right one

	    if ( strcmp(generic_dirp->entry->name, name)!=0 ) generic_dirp->entry=NULL;

	}

	if ( ! generic_dirp->entry ) generic_dirp->entry = find_entry(generic_dirp->generic_fh.entry->inode->ino, name);

	if ( ! generic_dirp->entry) {

	    // entry is not created

	    generic_dirp->entry = new_entry(generic_dirp->generic_fh.entry->inode->ino, name);

	    if ( ! generic_dirp->entry ) {

		nreturn=-ENOMEM;
		goto out;

	    }

	    entrycreated=1;

	}

	if ( ! generic_dirp->entry->inode ) {

	    logoutput0("get_direntry_stat, inode not attached");
	    nreturn=-ENOENT;

	}

	st->st_ino = generic_dirp->entry->inode->ino;

	if ( entrycreated==1 ) {

	    add_to_inode_hash_table(generic_dirp->entry->inode);
	    add_to_name_hash_table(generic_dirp->entry);

	}

    }

    out:

    return (nreturn==0) ? entrycreated : nreturn;

}

static int do_readdir_localhost(fuse_req_t req, char *buf, size_t size, off_t upperfs_offset, struct testfs_generic_dirp_struct *dirp)
{
    size_t bufpos = 0;
    int res, nreturn=0;
    struct stat st;
    size_t entsize;
    bool validentryfound=false;
    bool direntryfrompreviousbatch=false;


    dirp->upperfs_offset=upperfs_offset;
    dirp->underfs_offset=telldir(dirp->dp);

    while ( bufpos < size ) {

	//
	// no valid entry found yet (the purpose here is to find valid entries right?)
	// (or there is still on attached to dirp)

	// search a new direntry
	validentryfound=false;


	// start a "search" through the directory stream to the first valid next entry
	while ( ! validentryfound ) {

	    // read next entry (only when not one attached to dirp)

	    direntryfrompreviousbatch=true;


	    if ( ! dirp->direntry ) {

		// read a direntry from the stream

		// somehow setting this here is necessary

		errno=0;

		dirp->direntry=readdir(dirp->dp);

		direntryfrompreviousbatch=false;

		if ( ! dirp->direntry ) {

		    // no direntry from readdir, look at what's causing this

		    nreturn=0;

		    if (errno) {

			// some error ocuured

			nreturn=-errno;

		    }

		    goto out;

		}

	    }

	    // a direntry read from the directory stream, check this entry is hidden or not

	    res=get_direntry_stat(dirp, &st);

	    if ( res<0 ) {

		nreturn=res;
		goto out;

	    }

	    validentryfound=true;

        }

        // store the next offset (of course of the underlying fs)
        // this is after the readdir, so is pointing to the next direntry

        dirp->underfs_offset=telldir(dirp->dp);

        if ( ! direntryfrompreviousbatch ) {

            // a valid direntry not from a previous batch is here, so the offset has to be increased

            dirp->upperfs_offset+=1;

        }


        entsize = fuse_add_direntry(req, buf + bufpos, size - bufpos, dirp->direntry->d_name, &st, dirp->upperfs_offset);

        // break when buffer is not large enough
        // function fuse_add_direntry has not added it when buffer is too small to hold direntry, 
        // only returns the requested size

	if (entsize > size - bufpos) {

	    // the direntry does not fit in buffer
	    // (keep the current direntry and entry attached)
	    break;

	}

	bufpos += entsize;

	dirp->direntry=NULL;
	dirp->entry=NULL;

    }


    out:


    if (nreturn<0) {

	logoutput1("do_readdir, return: %i", nreturn);

	// if a real error: return that
	return nreturn;

    } else {

	logoutput1("do_readdir, return: %zi", bufpos);

	return bufpos;

    }

}

static void testfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct testfs_generic_dirp_struct *dirp = get_dirp(fi);
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


static void testfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct testfs_generic_dirp_struct *dirp = get_dirp(fi);
    int nreturn=0;

    (void) ino;

    logoutput1("RELEASEDIR");

    nreturn=closedir(dirp->dp);

    fuse_reply_err(req, nreturn);

    if ( dirp->call_info ) remove_call_info(dirp->call_info);
    free_dirp(dirp);
    fi->fh=0;

}

static void testfs_statfs(fuse_req_t req, fuse_ino_t ino)
{
    struct statvfs st;
    int nreturn=0, res;
    struct testfs_entry_struct *entry; 
    struct testfs_inode_struct *inode;

    logoutput1("STATFS");

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

static void testfs_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size, int flags)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    int nreturn=0, res;
    struct testfs_entry_struct *entry;
    struct testfs_inode_struct *inode;
    char *basexattr=NULL;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0;

    logoutput1("SETXATTR");

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

    nreturn=determine_path(&call_info, TESTFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    /* user must have read access */

    res=lstat(call_info.path, &st);

    /* copy the st to inode->st */

    if ( res!=-1 ) copy_stat(&inode->st, &st);


    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

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

	nreturn=setxattr4workspace(&call_info, name + strlen(basexattr), value);

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


static void testfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    int nreturn=0, nlen=0, res;
    struct testfs_entry_struct *entry;
    struct testfs_inode_struct *inode;
    void *value=NULL;
    struct xattr_workspace_struct *xattr_workspace;
    char *basexattr=NULL;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0;

    logoutput1("GETXATTR, name: %s, size: %i", name, size);

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

    nreturn=determine_path(&call_info, TESTFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    /* user must have read access */

    res=lstat(call_info.path, &st);

    /* copy the st to inode->st */

    if ( res!=-1) copy_stat(&inode->st, &st);


    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

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

	    getxattr4workspace(&call_info, name + strlen(basexattr), xattr_workspace);

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


static void testfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    ssize_t nlenlist=0;
    int nreturn=0, res;
    char *list=NULL;
    struct testfs_entry_struct *entry;
    struct testfs_inode_struct *inode;
    struct stat st;
    struct call_info_struct call_info;
    unsigned char entryexists=0;

    logoutput1("LISTXATTR, size: %li", (long) size);

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

    nreturn=determine_path(&call_info, TESTFS_PATH_BACKEND);
    if (nreturn<0) goto out;

    /* user must have read access */

    res=lstat(call_info.path, &st);

    /* copy the st to inode->st */

    if (res!=-1) copy_stat(&inode->st, &st);


    if (res==-1) {

        /* entry does not exist.... */
        /* additional action here: sync fs */

        nreturn=-ENOENT;
        goto out;

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

	nlenlist=listxattr4workspace(&call_info, list, size);

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

        snprintf(testfs_options.pidfile, PATH_MAX, "%s/testfs.pid", tmpchar);

        buf=malloc(20);

        if ( buf ) {
            struct stat st;
            int fd=0,res;

	    memset(buf, '\0', 20);
	    sprintf(buf, "%d", getpid()); 

	    logoutput1("storing pid: %s in %s", buf, testfs_options.pidfile);

	    res=stat(testfs_options.pidfile, &st);

	    if ( S_ISREG(st.st_mode) ) {

	        fd = open(testfs_options.pidfile, O_RDWR | O_EXCL | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	    } else {

	        fd = open(testfs_options.pidfile, O_RDWR | O_EXCL | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

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

    if ( strlen(testfs_options.pidfile)>0 ) {
        struct stat st;
        int res;

	res=stat(testfs_options.pidfile, &st);

	if ( res!=-1 && S_ISREG(st.st_mode) ) {

	    logoutput1("Pid file %s found, removing it.", testfs_options.pidfile);

	    res=unlink(testfs_options.pidfile);

	}

    }
}



static void testfs_init (void *userdata, struct fuse_conn_info *conn)
{

    // create a pid file

    create_pid_file();

    // init_notifyfs_mount_paths();

}


static void testfs_destroy (void *userdata)
{

    // remove pid file

    remove_pid_file();

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
                    struct testfs_entry_struct *entry=NULL;

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

                        nreturn=determine_path(&call_info, TESTFS_PATH_NONE);
                        if (nreturn<0) continue;

                        /* in case of delete, move or unmount: update fs */

                        if ( i_event->mask & ( IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM ) ) {

                            /* something is deleted/moved from in the directory or the directory self */

                            /* remove entry, and if directory and contents, do that recursive
                               also remove any watch set there, notify clients about that 
                               and invalidate the removed entries/inodes
                            */

                            changestate(&call_info, WATCH_ACTION_REMOVE);

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

                                nreturn=determine_path(&call_info, TESTFS_PATH_FORCE);
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


/* todo: (symlink,) readlink, open, read, (write,) close, setattr */

static struct fuse_lowlevel_ops testfs_oper = {
	.init		= testfs_init,
	.destroy	= testfs_destroy,
	.lookup		= testfs_lookup,
	.forget		= testfs_forget,
	.getattr	= testfs_getattr,
	.readlink	= testfs_readlink,
	.opendir	= testfs_opendir,
	.readdir	= testfs_readdir,
	.releasedir	= testfs_releasedir,
	.statfs		= testfs_statfs,
	.setxattr	= testfs_setxattr,
	.getxattr	= testfs_getxattr,
	.listxattr	= testfs_listxattr,
};


int main(int argc, char *argv[])
{
    struct fuse_args testfs_args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_session *testfs_session;
    char *testfs_mountpoint;
    int foreground=0;
    int res, epoll_fd;
    struct stat st;


    umask(0);

    // set logging

    openlog("fuse.testfs", 0,0); 

    // clear commandline options

    testfs_commandline_options.notifyfssocket=NULL;
    testfs_commandline_options.logging=1;

    // set defaults

    testfs_options.logging=0;
    testfs_options.logarea=0;

    memset(testfs_options.notifyfssocket, '\0', UNIX_PATH_MAX);
    memset(testfs_options.pidfile, '\0', PATH_MAX);


    // read commandline options

    res = fuse_opt_parse(&testfs_args, &testfs_commandline_options, testfs_help_options, testfs_options_output_proc);

    if (res == -1) {

	fprintf(stderr, "Error parsing options.\n");
	exit(1);

    }

    res = fuse_opt_insert_arg(&testfs_args, 1, "-oallow_other,nodev,nosuid");


    // socket

    if ( testfs_commandline_options.notifyfssocket ) {

        if ( strlen(testfs_commandline_options.notifyfssocket) >= UNIX_PATH_MAX ) {

	    fprintf(stderr, "Length of socket %s is too big.\n", testfs_commandline_options.notifyfssocket);
	    exit(1);

	}

	res=stat(testfs_commandline_options.notifyfssocket, &st);

	if ( res==-1 ) {

	    fprintf(stdout, "Notifyfssocket %s not found. Cannot conytinue.\n", testfs_commandline_options.notifyfssocket);

	}


	unslash(testfs_commandline_options.notifyfssocket);
	strcpy(testfs_options.notifyfssocket, testfs_commandline_options.notifyfssocket);

	fprintf(stdout, "Taking socket %s.\n", testfs_options.notifyfssocket);

    }


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

    if ( testfs_commandline_options.logging>0 ) testfs_options.logging=testfs_commandline_options.logging;
    if ( testfs_commandline_options.logarea>0 ) testfs_options.logarea=testfs_commandline_options.logarea;


    loglevel=testfs_options.logging;
    logarea=testfs_options.logarea;

    testfs_options.attr_timeout=1.0;
    testfs_options.entry_timeout=1.0;
    testfs_options.negative_timeout=1.0;

    res = -1;

    if (fuse_parse_cmdline(&testfs_args, &testfs_mountpoint, NULL, &foreground) == -1 ) {

        logoutput0("Error parsing options.");
        goto out;

    }

    testfs_options.mountpoint=testfs_mountpoint;

    if ( (testfs_chan = fuse_mount(testfs_mountpoint, &testfs_args)) == NULL) {

        logoutput0("Error mounting and setting up a channel.");
        goto out;

    }

    testfs_session=fuse_lowlevel_new(&testfs_args, &testfs_oper, sizeof(testfs_oper), NULL);

    if ( testfs_session == NULL ) {

        logoutput0("Error starting a new session.");
        goto out;

    }

    res = fuse_daemonize(foreground);

    if ( res!=0 ) {

        logoutput0("Error daemonize.");
        goto out;

    }

    fuse_session_add_chan(testfs_session, testfs_chan);

    epoll_fd=init_mainloop();

    if ( epoll_fd<0 ) {

        logoutput0("Error creating epoll fd: %i.", epoll_fd);
        goto out;

    }

    if ( strlen(testfs_options.notifyfssocket)>0 ) {

	/*
    	    connect to the notify fs socket clients

	*/

	testfs_options.notifyfssocket_fd=connect_socket(testfs_options.notifyfssocket);

	if ( testfs_options.notifyfssocket_fd<=0 ) {

    	    logoutput0("Error connecting to notifyfs socket: %i.", testfs_options.notifyfssocket_fd);
    	    goto out;

	}

	/* add notifyfs socket to epoll */

	res=add_to_epoll(testfs_options.notifyfssocket_fd, EPOLLIN, TYPE_FD_SOCKET, &handle_data_on_socket_fd, NULL);
	if ( res<0 ) {

    	    logoutput0("Error adding socket fd to epoll: %i.", res);
    	    goto out;

	} else {

    	    logoutput0("socket fd %i added to epoll", testfs_options.notifyfssocket_fd);

	}

	/* register as client fs to notifyfs */

	res=send_notify_message(testfs_options.notifyfssocket_fd, NOTIFYFS_MESSAGE_TYPE_REGISTERFS, 0, 0, strlen(testfs_options.mountpoint), testfs_options.mountpoint);

	if ( res<=0 ) {

	    logoutput0("Error sending register fs message to notifyfs: %i.", res);

	} else {

	    logoutput0("Sending register fs message: %i nr bytes written.", res);

	}

    }


    /*
    *    add an inotify instance to epoll : default backend 
    *
    */

    testfs_options.inotify_fd=inotify_init();

    if ( testfs_options.inotify_fd<=0 ) {

        logoutput0("Error creating inotify fd: %i.", testfs_options.inotify_fd);
        goto out;

    }

    /* add inotify to epoll */

    res=add_to_epoll(testfs_options.inotify_fd, EPOLLIN, TYPE_FD_INOTIFY, &handle_data_on_inotify_fd, NULL);

    if ( res<0 ) {

        logoutput0("Error adding inotify fd to epoll: %i.", res);
        goto out;

    } else {

        logoutput0("inotify fd %i added to epoll", testfs_options.inotify_fd);

    }


    res=fuse_session_loop_epoll_mt(testfs_session);

    fuse_session_remove_chan(testfs_chan);

    fuse_session_destroy(testfs_session);

    fuse_unmount(testfs_mountpoint, testfs_chan);

    out:

    fuse_opt_free_args(&testfs_args);

    closelog();

    return res ? 1 : 0;

}
