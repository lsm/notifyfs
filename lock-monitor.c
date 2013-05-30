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
#include <dirent.h>

#include <glib.h>

#define LOG_LOGAREA LOG_LOGAREA_MOUNTMONITOR

#include "logging.h"
#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"
#include "entry-management.h"
#include "path-resolution.h"
#include "lock-monitor.h"
#include "utils.h"


#define NOTIFYFS_LOCKKIND_POSIX			1
#define NOTIFYFS_LOCKKIND_FLOCK			2
#define NOTIFYFS_LOCKKIND_UNKNOWN		3

#define NOTIFYFS_LOCKTYPE_READ			1
#define NOTIFYFS_LOCKTYPE_WRITE			2
#define NOTIFYFS_LOCKTYPE_UNKNOWN		3

#define LOCKS_FILE "/proc/locks"

struct locks_list_struct {
    struct lock_entry_struct *first;
    struct lock_entry_struct *last;
    struct lock_entry_struct *time_first;
    struct lock_entry_struct *time_last;
    pthread_mutex_t mutex;
};

struct locks_list_struct current_locks={NULL, NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER};
struct locks_list_struct new_locks={NULL, NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER};

void init_lock_entry(struct lock_entry_struct *lock_entry)
{

    lock_entry->major=0;
    lock_entry->minor=0;
    lock_entry->ino=0;
    lock_entry->mandatory=0;
    lock_entry->kind=0;
    lock_entry->type=0;
    lock_entry->pid=0;

    lock_entry->start=0;
    lock_entry->end=0;

    lock_entry->next=NULL;
    lock_entry->prev=NULL;

    lock_entry->time_next=NULL;
    lock_entry->time_prev=NULL;

    lock_entry->entry=NULL;

    lock_entry->detect_time.tv_sec=0;
    lock_entry->detect_time.tv_nsec=0;

}

struct lock_entry_struct *get_lock_entry()
{
    struct lock_entry_struct *lock_entry=NULL;

    lock_entry=malloc(sizeof(struct lock_entry_struct));

    return lock_entry;

}

void free_lock_entry(struct lock_entry_struct *lock_entry)
{

    free(lock_entry);

}


/* compare two lock entries 
    based upon pid,major,minor,ino

    a compare function is required to sort */

int compare_lock_entries(struct lock_entry_struct *lock_a, struct lock_entry_struct *lock_b)
{
    int res=0;

    if (lock_a->pid < lock_b->pid) {

	res=-1;

    } else if (lock_a->pid > lock_b->pid) {

	res=1;

    } else {

	if (lock_a->major<lock_b->major) {

	    res=-1;

	} else if (lock_a->major>lock_b->major) {

	    res=1;

	} else {

	    if (lock_a->minor<lock_b->minor) {

		res=-1;

	    } else if (lock_a->minor>lock_b->minor) {

	    res=1;

	    } else {

		if (lock_a->ino<lock_b->ino) {

		    res=-1;

		} else if (lock_a->ino>lock_b->ino) {

		    res=1;

		} else {

		    res=0;

		}

	    }

	}

    }

    return res;

}

void add_lock_to_timeindex(struct locks_list_struct *locks_list, struct lock_entry_struct *lock_entry)
{
    struct lock_entry_struct *lock_entry_prev;

    logoutput("add_lock_to_timeindex");

    if (lock_entry->detect_time.tv_sec==0 && lock_entry->detect_time.tv_nsec==0) {

	get_current_time(&lock_entry->detect_time);

    }

    pthread_mutex_lock(&locks_list->mutex);

    lock_entry_prev=locks_list->time_first;

    while(lock_entry_prev) {

	if (is_later(&lock_entry_prev->detect_time, &lock_entry->detect_time, 0, 0)==1) {

	    /* insert before lock_entry_prev */

	    if (lock_entry_prev->time_prev) lock_entry_prev->time_prev->time_next=lock_entry;
	    lock_entry->time_prev=lock_entry_prev->time_prev;
	    lock_entry->time_next=lock_entry_prev;
	    lock_entry_prev->time_prev=lock_entry;

	    if (locks_list->time_first==lock_entry_prev) locks_list->time_first=lock_entry;

	    break;

	}

	lock_entry_prev=lock_entry_prev->time_next;

    }

    if (! lock_entry_prev) {

	/* add it to tail */

	lock_entry_prev=locks_list->time_last;

	lock_entry->time_prev=lock_entry_prev;
	if ( lock_entry_prev) lock_entry_prev->time_next=lock_entry;

	lock_entry->time_next=NULL;

	locks_list->time_last=lock_entry;

	if (! locks_list->time_first) locks_list->time_first=lock_entry;

    }

    pthread_mutex_unlock(&locks_list->mutex);

}

void add_lock_to_list(struct locks_list_struct *locks_list, struct lock_entry_struct *lock_entry)
{
    struct lock_entry_struct *lock;
    int res;

    logoutput("add_lock_to_list: %i %i:%i:%li", (int) lock_entry->pid, lock_entry->major, lock_entry->minor, (long int) lock_entry->ino);

    pthread_mutex_lock(&locks_list->mutex);

    lock=locks_list->first;

    while(lock) {

	res=compare_lock_entries(lock_entry, lock);

	if (res<=0) {

	    if (lock==locks_list->first) {

		logoutput("add_lock_to_list: added %i:%i:%li at start", lock_entry->major, lock_entry->minor, (long int) lock_entry->ino);

		lock_entry->next=lock;
		lock_entry->prev=NULL;

		lock->prev=lock_entry;

		locks_list->first=lock_entry;

	    } else {

		logoutput("add_lock_to_list: added %i:%i:%li in between", lock_entry->major, lock_entry->minor, (long int) lock_entry->ino);

		lock_entry->next=lock;
		lock_entry->prev=lock->prev;

		lock->prev->next=lock_entry;
		lock->prev=lock_entry;

	    }

	    break;

	}

	lock=lock->next;

    }

    if (! lock) {

	logoutput("add_lock_to_list: added %i:%i:%li at tail", lock_entry->major, lock_entry->minor, (long int) lock_entry->ino);

	if (locks_list->last) {

	    lock=locks_list->last;

	    lock_entry->prev=lock;
	    lock_entry->next=NULL;

	    lock->next=lock_entry;

	} else {

	    /* no last: empty */

	    locks_list->first=lock_entry;

	}

	locks_list->last=lock_entry;

    }

    pthread_mutex_unlock(&locks_list->mutex);

}

/* function which creates a path in notifyfs knowing the pid and the ino found in the locks file
    every lock set by the kernel has a matching fd
    so it must be present in /proc/%pid%/fd/ 
    this path is a symlink to the object (file) the lock is set on 
    this function finds this target and creates the entry in notifysfs
    this entry will be returned
*/

static struct notifyfs_entry_struct *get_notifyfs_entry(ino_t ino, pid_t pid)
{
    int lenpath=strlen("/proc//fd/") + sizeof(int) + 1;
    char path[lenpath], *target=NULL;
    DIR *dirp;
    int fd;
    struct notifyfs_entry_struct *entry=NULL;
    struct dirent *de;

    snprintf(path, lenpath, "/proc/%i/fd/", (int) pid);

    logoutput("get_notifyfs_entry: check for pid %i, ino %li in %s", (int) pid, (long int) ino, path);

    dirp=opendir(path);

    if (! dirp) goto out;

    fd=dirfd(dirp);

    if (fd==-1) {

	closedir(dirp);
	goto out;

    }

    while((de=readdir(dirp))) {

	/* catch all direntries starting with a . */

	if (strncmp(de->d_name, ".", 1)==0) continue;

	if (strtol(de->d_name, (char **) NULL, 10)==0) {

	    continue;

	} else {
	    struct stat st;
	    char *sympath=NULL;
	    size_t lensymlink=0;
	    int res=0;
	    struct call_info_struct call_info;

	    if (fstatat(fd, de->d_name, &st, 0)==-1) continue;

	    if (st.st_ino!=ino) continue;

	    logoutput("get_notifyfs_entry: found fd %s", de->d_name);

	    /* size of target path of symlink is st.st_size */

	    lensymlink=st.st_size;

	    gettarget:

	    if (sympath) {

		sympath=realloc(sympath, lensymlink+1);

	    } else {

		sympath=malloc(lensymlink+1);

	    }

	    res=readlinkat(fd, de->d_name, sympath, lensymlink+1);

	    if (res>lensymlink) {

		/* increased in the meantime ?? */

		lensymlink=res;
		goto gettarget;

	    }

	    *(sympath+res)='\0';

	    /* create/check the path in notifyfs */

	    init_call_info(&call_info, NULL);

	    call_info.pathinfo.path=sympath;
	    call_info.strict=0;

	    create_notifyfs_path(&call_info);

	    entry=call_info.entry;

	    free(sympath);

	    break;

	}

    }

    closedir(dirp);

    out:

    if (entry) {

	logoutput("get_notifyfs_entry: created entry");

    } else {

	logoutput("get_notifyfs_entry: no entry created");

    }

    return entry;

}

void get_new_locks_list()
{
    FILE *fp;
    char buff[PATH_MAX];
    int lenbuff=sizeof(buff);
    int res;
    char ctr[8];
    char kindlock[64];
    char urgence[64];
    char typelock[64];
    char location[64];
    pid_t pid;
    int major;
    int minor;
    ino_t ino;
    char startpos[32];
    char endpos[32];
    struct lock_entry_struct *lock_entry;
    char *sep;

    logoutput("get_new_locks_list");

    fp=fopen(LOCKS_FILE, "r");

    if (fp) {

	while (! feof(fp)) {

	    if (!fgets(buff, lenbuff, fp)) continue;

	    sep=strchr(buff, '\n');

	    if (sep) *sep='\0';

	    logoutput("get_new_locks_list: read %s", &buff[0]);

	    res=sscanf(&buff[0], "%s %s %s %s %i %s %s %s", &ctr[0], &kindlock[0], &urgence[0], &typelock[0], &pid, &location[0], &startpos[0], &endpos[0]);

	    logoutput("get_new_locks_list: return sscanf %i", res);

	    if (res!=8) continue;

	    sscanf(location, "%02x:%02x:%lli", &major, &minor, &ino);

	    lock_entry=get_lock_entry();

	    if (lock_entry) {

		init_lock_entry(lock_entry);

		lock_entry->kind=NOTIFYFS_LOCKKIND_UNKNOWN;

		if (strcmp(kindlock, "POSIX")==0) {

		    lock_entry->kind=NOTIFYFS_LOCKKIND_POSIX;

		} else if (strcmp(kindlock, "FLOCK")==0) {

		    lock_entry->kind=NOTIFYFS_LOCKKIND_FLOCK;

		}

		lock_entry->mandatory=0;

		if (strcmp(urgence, "MANDATORY")==0) lock_entry->mandatory=1;

		lock_entry->type=NOTIFYFS_LOCKTYPE_UNKNOWN;

		if (strcmp(typelock, "READ")==0) {

		    lock_entry->type=NOTIFYFS_LOCKTYPE_READ;

		} else if (strcmp(typelock, "WRITE")==0) {

		    lock_entry->type=NOTIFYFS_LOCKTYPE_WRITE;

		}

		lock_entry->pid=pid;
		lock_entry->major=major;
		lock_entry->minor=minor;
		lock_entry->ino=ino;

		if (strcmp(startpos, "EOF")==0) {

		    lock_entry->start=0;

		} else {

		    lock_entry->start=strtoll(startpos, (char **) NULL, 10);

		}

		if (strcmp(endpos, "EOF")==0) {

		    lock_entry->end=0;

		} else {

		    lock_entry->end=strtoll(endpos, (char **) NULL, 10);

		}

		add_lock_to_list(&new_locks, lock_entry);

	    }

	}

    }

    fclose(fp);

}


void parse_changes_locks()
{
    struct lock_entry_struct *lock_entry, *lock_entry_new;
    int res;

    logoutput("parse_changes_locks");

    /* get a new sorted list of locks */

    get_new_locks_list();

    lock_entry=current_locks.first;
    lock_entry_new=new_locks.first;

    while(1) {

	if (lock_entry && lock_entry_new) {

	    /* both are set */

	    res=compare_lock_entries(lock_entry_new, lock_entry);

	} else if (lock_entry_new) {

	    /* only lock entry in new locks */

	    logoutput("parse_changes_locks: lock added");

	    res=-1;

	} else if (lock_entry) {

	    /* only lock entry in locks */

	    logoutput("parse_changes_locks: lock removed");

	    res=1;

	} else {

	    /* no more lock entries: ready */

	    logoutput("parse_changes_locks: ready");

	    break;

	}

	if (res==0) {
	    struct lock_entry_struct *lock_entry_next=lock_entry_new->next;

	    /* the same: remove from new */

	    if (lock_entry_new->prev) lock_entry_new->prev->next=lock_entry_new->next;
	    if (lock_entry_new->next) lock_entry_new->next->prev=lock_entry_new->prev;

	    if (lock_entry_new==new_locks.first) new_locks.first=lock_entry_new->next;
	    if (lock_entry_new==new_locks.last) new_locks.last=lock_entry_new->prev;

	    free_lock_entry(lock_entry_new);

	    /* step in both lists */

	    lock_entry_new=lock_entry_next;
	    lock_entry=lock_entry->next;

	} else if (res==-1) {
	    struct lock_entry_struct *lock_entry_next=lock_entry_new->next;
	    struct notifyfs_entry_struct *entry;

	    /* added */

	    /* remove from main index new locks */

	    if (lock_entry_new->prev) lock_entry_new->prev->next=lock_entry_new->next;
	    if (lock_entry_new->next) lock_entry_new->next->prev=lock_entry_new->prev;

	    if (lock_entry_new==new_locks.first) new_locks.first=lock_entry_new->next;
	    if (lock_entry_new==new_locks.last) new_locks.last=lock_entry_new->prev;

	    /* insert in current */

	    if (lock_entry) {

		lock_entry_new->prev=lock_entry->prev;
		lock_entry_new->next=lock_entry;

		if (lock_entry->prev) lock_entry->prev->next=lock_entry_new;
		lock_entry->prev=lock_entry_new;

		if (lock_entry==current_locks.first) current_locks.first=lock_entry_new;

	    } else {

		if(! current_locks.first) current_locks.first=lock_entry_new;

		if (current_locks.last) current_locks.last->next=lock_entry_new;
		lock_entry_new->prev=current_locks.last;
		current_locks.last=lock_entry_new;

	    }

	    /* create the path to lock in notifyfs (result: entry)*/

	    logoutput("parse_changes_locks: found a new lock");

	    entry=get_notifyfs_entry(lock_entry_new->ino, lock_entry_new->pid);

	    if (entry) {
		struct notifyfs_inode_struct *inode;
		char *name=get_data(entry->name);

		/* take the (cached) time of the file: mtime is the creation time of the lock ?? (atime??) 
		*/

		logoutput("parse_changes_locks: take time from %s", name);

		inode=get_inode(entry->inode);

		//if (inode) {

		//    lock_entry_new->detect_time.tv_sec=inode->mtime.tv_sec;
		//    lock_entry_new->detect_time.tv_nsec=inode->mtime.tv_nsec;

		//}

		lock_entry_new->entry=(void *) entry;

	    }

	    add_lock_to_timeindex(&current_locks, lock_entry_new);

	    /* step in new list */

	    lock_entry_new=lock_entry_next;

	} else if (res==1) {
	    struct lock_entry_struct *lock_entry_next=lock_entry->next;

	    /* removed */

	    /* remove from the main index */

	    if (lock_entry->prev) lock_entry->prev->next=lock_entry->next;
	    if (lock_entry->next) lock_entry->next->prev=lock_entry->prev;

	    if (lock_entry==current_locks.first) current_locks.first=lock_entry->next;
	    if (lock_entry==current_locks.last) current_locks.last=lock_entry->prev;

	    /* remove from the time index */

	    if (lock_entry->time_prev) lock_entry->time_prev->time_next=lock_entry->time_next;
	    if (lock_entry->time_next) lock_entry->time_next->time_prev=lock_entry->time_prev;

	    if (lock_entry==current_locks.time_first) current_locks.time_first=lock_entry->time_next;
	    if (lock_entry==current_locks.time_last) current_locks.time_last=lock_entry->time_prev;

	    /* step in current list */

	    lock_entry=lock_entry_next;

	}

    }

}

int open_locksfile()
{

    return open(LOCKS_FILE, O_RDONLY);

}
