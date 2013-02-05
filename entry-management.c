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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>

#include <pthread.h>
#include <fcntl.h>


#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_INODES

#include <fuse/fuse_lowlevel.h>
#include "logging.h"
#include "notifyfs-io.h"
#include "utils.h"
#include "entry-management.h"


static unsigned long long inoctr = FUSE_ROOT_ID;
static pthread_mutex_t inoctr_mutex=PTHREAD_MUTEX_INITIALIZER;

struct notifyfs_inode_struct *rootinode=NULL;
struct notifyfs_entry_struct *rootentry=NULL;

static int free_entries=-1;
static pthread_mutex_t free_entries_mutex=PTHREAD_MUTEX_INITIALIZER;

static int free_attributes=-1;
static pthread_mutex_t free_attributes_mutex=PTHREAD_MUTEX_INITIALIZER;

static int free_mounts=-1;
static pthread_mutex_t free_mounts_mutex=PTHREAD_MUTEX_INITIALIZER;

struct notifyfs_mount_struct *create_mount(char *fs, char *mountsource, char *superoptions, struct notifyfs_entry_struct *entry)
{
    struct notifyfs_mount_struct *mount=NULL;

    pthread_mutex_lock(&free_mounts_mutex);

    if (free_mounts>=0) {

	mount=get_mount(free_mounts);

	if (mount->next>=0) {
	    struct notifyfs_mount_struct *next=get_mount(mount->next);

	    next->prev=mount->prev;

	}

	if (mount->prev>=0) {
	    struct notifyfs_mount_struct *prev=get_mount(mount->prev);

	    prev->next=mount->next;

	}

	free_mounts=mount->next;

    }

    pthread_mutex_unlock(&free_mounts_mutex);

    if (! mount) mount=notifyfs_malloc_mount();

    if (mount) {

	logoutput("create_mount: new mount fs %s, backend %s", fs, mountsource);

	mount->entry=entry->index;
	entry->mount=mount->index;
	mount->rootentry=-1;
	mount->major=0;
	mount->minor=0;

	snprintf(mount->filesystem, 32, "%s", fs);
	snprintf(mount->mountsource, 64, "%s", mountsource);
	snprintf(mount->superoptions, 256, "%s", superoptions);

	mount->mode=0;
	mount->next=-1;
	mount->prev=-1;

    }

    return mount;

}

void remove_mount(struct notifyfs_mount_struct *mount)
{

    pthread_mutex_lock(&free_mounts_mutex);

    if (free_mounts>=0) {
	struct notifyfs_mount_struct *first_mount=get_mount(free_mounts);

	first_mount->prev=mount->index;

    }

    mount->next=free_mounts;
    mount->prev=-1;
    free_mounts=mount->index;

    pthread_mutex_unlock(&free_mounts_mutex);

}

static struct notifyfs_inode_struct *create_inode()
{
    struct notifyfs_inode_struct *inode=NULL;

    inode = notifyfs_malloc_inode();

    if (inode) {

	pthread_mutex_lock(&inoctr_mutex);

	inode->ino=inoctr;
	inoctr++;

	pthread_mutex_unlock(&inoctr_mutex);

	inode->nlookup = 0;
	inode->status=FSEVENT_INODE_STATUS_OK;

    }

    return inode;

}

struct notifyfs_entry_struct *create_entry(struct notifyfs_entry_struct *parent, const char *newname)
{
    struct notifyfs_entry_struct *entry=NULL;

    logoutput("create_entry: name %s", newname);

    pthread_mutex_lock(&free_entries_mutex);

    if (free_entries>=0) {

	entry=get_entry(free_entries);

	if (entry->name_next>=0) {
	    struct notifyfs_entry_struct *next=NULL;

	    next=get_entry(entry->name_next);
	    next->name_prev=entry->name_prev;

	}

	if (entry->name_prev>=0) {
	    struct notifyfs_entry_struct *prev=NULL;

	    prev=get_entry(entry->name_prev);
	    prev->name_next=entry->name_next;

	}

	free_entries=entry->name_next;

    }

    pthread_mutex_unlock(&free_entries_mutex);

    if (! entry) entry = notifyfs_malloc_entry();

    if (entry) {
	int lenname=strlen(newname);

	entry->name=notifyfs_malloc_data(lenname+1);

	if (entry->name==-1) {

	    remove_entry(entry);
	    entry = NULL;

	} else {
	    char *name=get_data(entry->name);

	    memcpy(name, newname, lenname);

	    logoutput("create_entry: created name %s", name);

	    if (parent) {

		entry->parent = parent->index;

	    } else {

		entry->parent=-1;

	    }

	    entry->inode=-1;
	    entry->name_next=-1;
	    entry->name_prev=-1;
	    entry->mount=-1;
	    entry->nameindex_value=0;

	}

    }

    return entry;

}

struct notifyfs_attr_struct *assign_attr(struct stat *st, struct notifyfs_inode_struct *inode)
{
    struct notifyfs_attr_struct *attr=NULL;

    pthread_mutex_lock(&free_attributes_mutex);

    if (free_attributes>=0) {

	attr=get_attr(free_attributes);

	if (attr->next>=0) {
	    struct notifyfs_attr_struct *next=NULL;

	    next=get_attr(attr->next);
	    next->prev=attr->prev;

	}

	if (attr->prev>=0) {
	    struct notifyfs_attr_struct *prev=NULL;

	    prev=get_attr(attr->prev);
	    prev->next=attr->next;

	}

	free_attributes=attr->next;

    }

    pthread_mutex_unlock(&free_attributes_mutex);

    if (! attr) attr=notifyfs_malloc_attr();

    if (attr) {

	inode->attr=attr->index;

	if (st) {

	    copy_stat(&attr->cached_st, st);
	    copy_stat_times(&attr->cached_st, st);

	}

    }

    return attr;

}

void remove_attr(struct notifyfs_attr_struct *attr)
{

    pthread_mutex_lock(&free_attributes_mutex);

    if (free_attributes>=0) {
	struct notifyfs_attr_struct *first_attr=get_attr(free_attributes);

	first_attr->prev=attr->index;

    }

    attr->next=free_attributes;
    attr->prev=-1;

    free_attributes=attr->index;

    pthread_mutex_unlock(&free_attributes_mutex);

}


void remove_entry(struct notifyfs_entry_struct *entry)
{

    if ( entry->inode>=0 ) {
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	inode->alias=-1;

	if (inode->attr>0) {
	    struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	    remove_attr(attr);

	}

    }

    pthread_mutex_lock(&free_entries_mutex);

    if (free_entries>=0) {
	struct notifyfs_entry_struct *first_entry=get_entry(free_entries);

	first_entry->name_prev=entry->index;

    }

    entry->name_next=free_entries;
    entry->name_prev=-1;

    free_entries=entry->index;

    pthread_mutex_unlock(&free_entries_mutex);

}

void assign_inode(struct notifyfs_entry_struct *entry)
{
    struct notifyfs_inode_struct *inode=create_inode();

    if (inode) {

	entry->inode=inode->index;
	inode->alias=entry->index;

    }

}

unsigned long long get_inoctr()
{
    return inoctr;
}

/* lookup the inode in the inode array 
    since there can be more than one arrays, which are in different shm chunks
    it's the task to find the right array
    the way to do this is to look at the modulo and the remainder
    of the ino compared to the array size
    note that the relation between index in array and ino is that
    index starts at 0, and ino at FUSE_ROOT_ID, which is 1 normally
*/

struct notifyfs_inode_struct *find_inode(fuse_ino_t ino)
{
    struct notifyfs_inode_struct *inode=NULL;
    int ctr=0;
    int index=0;

    index=ino-FUSE_ROOT_ID; /* correct the ino to fit with array index */

    inode=get_inode(index);

    return inode;

}

struct notifyfs_entry_struct *find_entry_by_ino(fuse_ino_t ino, const char *name)
{
    struct notifyfs_inode_struct *inode=find_inode(ino);

    if (inode) {
	struct notifyfs_entry_struct *parent;

	parent=get_entry(inode->alias);

	if (parent) {

	    return find_entry_raw(parent, inode, name, 1, NULL);

	}

    }

    return NULL;

}

struct notifyfs_entry_struct *find_entry_by_entry(struct notifyfs_entry_struct *parent, const char *name)
{
    struct notifyfs_inode_struct *inode=get_inode(parent->inode);

    return find_entry_raw(parent, inode, name, 1, NULL);

}

/* create the root inode and entry */

int create_root()
{
    int nreturn=0;
    char *name;

    if ( rootentry ) goto out;

    /* rootentry (no parent) */

    rootentry=create_entry(NULL, ".");

    /* rootinode */

    assign_inode(rootentry);

    rootinode=get_inode(rootentry->inode);

    name=get_data(rootentry->name);

    logoutput("create_root: created rootentry %s with ino %li", name, rootinode->ino);

    out:

    return nreturn;

}

void assign_rootentry()
{

    rootentry=get_entry(0);

    if (rootentry) {
	char *name=get_data(rootentry->name);

	logoutput("assign_rootentry: found root entry %s", name);

    }

}


/* get the root inode */

struct notifyfs_inode_struct *get_rootinode()
{
    return rootinode;
}

struct notifyfs_entry_struct *get_rootentry()
{
    return rootentry;
}

unsigned char isrootinode(struct notifyfs_inode_struct *inode)
{

    return (inode->ino==FUSE_ROOT_ID) ? 1 : 0;

}

unsigned char isrootentry(struct notifyfs_entry_struct *entry)
{

    return (entry==rootentry) ? 1 : 0;

}

void copy_stat_to_notifyfs(struct stat *st, struct notifyfs_entry_struct *entry)
{

    if (entry->inode>=0) {
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	if (inode->attr>=0) {
	    struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	    attr->cached_st.st_dev=st->st_dev;
	    attr->cached_st.st_ino=st->st_ino;
	    attr->cached_st.st_nlink=st->st_nlink;
	    attr->cached_st.st_mode=st->st_mode;
	    attr->cached_st.st_uid=st->st_uid;
	    attr->cached_st.st_gid=st->st_gid;
	    attr->cached_st.st_rdev=st->st_rdev;
	    attr->cached_st.st_size=st->st_size;
	    attr->cached_st.st_blksize=st->st_blksize;
	    attr->cached_st.st_blocks=st->st_blocks;
	    attr->cached_st.st_atim.tv_sec=st->st_atim.tv_sec;
	    attr->cached_st.st_atim.tv_nsec=st->st_atim.tv_nsec;
	    attr->cached_st.st_mtim.tv_sec=st->st_mtim.tv_sec;
	    attr->cached_st.st_mtim.tv_nsec=st->st_mtim.tv_nsec;
	    attr->cached_st.st_ctim.tv_sec=st->st_ctim.tv_sec;
	    attr->cached_st.st_ctim.tv_nsec=st->st_ctim.tv_nsec;

	}

    }

}

void get_stat_from_cache(struct stat *st, struct notifyfs_entry_struct *entry)
{

    if (entry->inode>=0) {
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	if (inode->attr>=0) {
	    struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	    st->st_dev=attr->cached_st.st_dev;
	    st->st_ino=attr->cached_st.st_ino;
	    st->st_mode=attr->cached_st.st_mode;
	    st->st_nlink=attr->cached_st.st_nlink;
	    st->st_uid=attr->cached_st.st_uid;
	    st->st_gid=attr->cached_st.st_gid;
	    st->st_rdev=attr->cached_st.st_rdev;
	    st->st_size=attr->cached_st.st_size;
	    st->st_blksize=attr->cached_st.st_blksize;
	    st->st_blocks=attr->cached_st.st_blocks;
	    st->st_atim.tv_sec=attr->cached_st.st_atim.tv_sec;
	    st->st_atim.tv_nsec=attr->cached_st.st_atim.tv_nsec;
	    st->st_mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
	    st->st_mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;
	    st->st_ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
	    st->st_ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

	}

    }

}

void get_stat_from_notifyfs(struct stat *st, struct notifyfs_entry_struct *entry)
{

    if (entry->inode>=0) {
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	if (inode->attr>=0) {
	    struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	    st->st_dev=0;
	    st->st_ino=inode->ino;
	    st->st_mode=attr->cached_st.st_mode;
	    st->st_nlink=attr->cached_st.st_nlink;
	    st->st_uid=attr->cached_st.st_uid;
	    st->st_gid=attr->cached_st.st_gid;
	    st->st_rdev=attr->cached_st.st_rdev;
	    st->st_size=attr->cached_st.st_size;

	    st->st_atim.tv_sec=attr->atim.tv_sec;
	    st->st_atim.tv_nsec=attr->atim.tv_nsec;
	    st->st_mtim.tv_sec=attr->mtim.tv_sec;
	    st->st_mtim.tv_nsec=attr->mtim.tv_nsec;
	    st->st_ctim.tv_sec=attr->ctim.tv_sec;
	    st->st_ctim.tv_nsec=attr->ctim.tv_nsec;

	}

    }

}



void notify_kernel_delete(struct fuse_chan *chan, struct notifyfs_entry_struct *entry)
{
    int res=0;
    struct notifyfs_entry_struct *parent_entry;

    parent_entry=get_entry(entry->parent);

    if (parent_entry) {
	struct notifyfs_inode_struct *inode;
	struct notifyfs_inode_struct *parent_inode;
	char *name;

	inode=get_inode(entry->inode);
	parent_inode=get_inode(parent_entry->inode);
	name=get_data(entry->name);

#if FUSE_VERSION >= 29

	if (inode) {

	    res=fuse_lowlevel_notify_delete(chan, parent_inode->ino, inode->ino, name, strlen(name));

	}

#else

	res=-ENOSYS;

#endif

	if (res==-ENOSYS) {

	    fuse_lowlevel_notify_inval_entry(chan, parent_inode->ino, name, strlen(name));

	    if (inode) fuse_lowlevel_notify_inval_inode(chan, inode->ino, 0, 0);

	}

    }

}

