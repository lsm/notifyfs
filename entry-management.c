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

#include <linux/kdev_t.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#define LOG_LOGAREA LOG_LOGAREA_INODES

#include "logging.h"

#include "epoll-utils.h"

#include "socket.h"
#include "utils.h"

#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"
#include "entry-management.h"
#include "backend.h"
#include "simple-list.h"

static unsigned long long inoctr = FUSE_ROOT_ID;
static pthread_mutex_t inoctr_mutex=PTHREAD_MUTEX_INITIALIZER;

struct notifyfs_inode_struct *rootinode=NULL;
struct notifyfs_entry_struct *rootentry=NULL;

static struct simple_group_struct group_mount;
static struct simple_group_struct group_entry;
static struct simple_group_struct group_attr;
struct simple_group_struct group_view;

static struct supermount_struct *supermount_list[MOUNTTABLE_SIZE];
static unsigned char initialized=0;
static pthread_mutex_t supermount_mutex=PTHREAD_MUTEX_INITIALIZER;

static unsigned long global_owner_id=0;
static pthread_mutex_t owner_id_mutex=PTHREAD_MUTEX_INITIALIZER;

unsigned long new_owner_id()
{
    unsigned long owner_id;

    pthread_mutex_lock(&owner_id_mutex);

    owner_id=global_owner_id;
    global_owner_id++;

    pthread_mutex_unlock(&owner_id_mutex);

    return owner_id;
}


int calculate_mount_hash(int major, int minor)
{
    return MKDEV(major, minor) % SIMPLE_LIST_HASHSIZE;
}

int mount_hashfunction(void *data)
{
    struct notifyfs_mount_struct *mount=(struct notifyfs_mount_struct *) data;
    return calculate_mount_hash(mount->major, mount->minor);

}

struct notifyfs_mount_struct *create_mount(struct notifyfs_entry_struct *entry, int major, int minor)
{
    struct notifyfs_mount_struct *mount=NULL;
    struct simple_list_struct *element;

    pthread_mutex_lock(&group_mount.mutex);

    element=get_from_free(&group_mount);

    if (! element) {

	element=create_simple_element();

	if (element) {

	    mount=notifyfs_malloc_mount();

	    if (! mount ) {

		free(element);
		element=NULL;

	    } else {

		element->data=(void *) mount;

	    }

	}

    }

    if (element) {

	mount=(struct notifyfs_mount_struct *) element->data;

	logoutput("create_mount: new mount %i:%i", major, minor);

	mount->entry=entry->index;
	entry->mount=mount->index;

	mount->rootentry=-1; /* TODO */
	mount->major=major;
	mount->minor=minor;

	insert_in_used(&group_mount, element);

    }

    pthread_mutex_unlock(&group_mount.mutex);

    return mount;

}

void remove_mount(struct notifyfs_mount_struct *mount)
{
    struct simple_list_struct *element;

    pthread_mutex_lock(&group_mount.mutex);

    element=lookup_simple_list(&group_mount, (void *) mount);

    if (element) {

	move_from_used(&group_mount, element);
	insert_in_free(&group_mount, element);

    }

    mount->entry=-1;
    mount->rootentry=-1;

    mount->major=0;
    mount->minor=0;

    mount->status=NOTIFYFS_MOUNTSTATUS_NOTUSED;

    pthread_mutex_unlock(&group_mount.mutex);

}

struct notifyfs_mount_struct *find_mount_majorminor(int major, int minor, struct notifyfs_mount_struct *new_mount)
{
    struct notifyfs_mount_struct *mount=NULL;
    int hashvalue=calculate_mount_hash(major, minor) % group_mount.len;
    struct simple_list_struct *element=group_mount.hash[hashvalue];

    while(element) {

	mount=(struct notifyfs_mount_struct *) element->data;

	if (mount->entry>=0 && mount->major==major && mount->minor==minor) {

	    if (new_mount) {

		if (mount != new_mount) break;

	    } else {

		break;

	    }

	}

	element=element->next;

    }

    return mount;

}

struct notifyfs_mount_struct *get_next_mount(int major, int minor, void **index)
{
    struct simple_list_struct *element=NULL;

    if (! *index) {
	int hashvalue=calculate_mount_hash(major, minor) % group_mount.len;

	element=group_mount.hash[hashvalue];

	*index=(void *) element;

    } else {

	element=(struct simple_list_struct *) *index;

	element=element->next;
	*index=(struct simple_list_struct *) element;

    }

    if (element) return element->data;

    return NULL;

}





void lock_supermounts()
{
    pthread_mutex_lock(&supermount_mutex);
}

void unlock_supermounts()
{
    pthread_mutex_unlock(&supermount_mutex);
}

/*
    function to get the supermount based upon major/minor
*/

struct supermount_struct *find_supermount_majorminor(int major, int minor)
{
    struct supermount_struct *supermount=NULL;

    pthread_mutex_lock(&supermount_mutex);

    supermount=supermount_list[calculate_mount_hash(major, minor)];

    while(supermount) {

	if (supermount->major==major && supermount->minor==minor) break;

	supermount=supermount->next;

    }

    pthread_mutex_unlock(&supermount_mutex);

    return supermount;

}

struct notifyfs_backend_struct *get_mount_backend(struct notifyfs_mount_struct *mount)
{
    struct notifyfs_backend_struct *backend=NULL;
    struct supermount_struct *supermount=NULL;

    logoutput("get_mount_backend: testing mount %i:%i", mount->major, mount->minor);

    supermount=find_supermount_majorminor(mount->major, mount->minor);
    if (supermount) backend=supermount->backend;

    if (backend) {

	logoutput("get_mount_backend: backend type %i found for %i:%i", backend->type, mount->major, mount->minor);

    } else {

	logoutput("get_mount_backend: no backend found for %i:%i", mount->major, mount->minor);

    }

    return backend;

}

struct supermount_struct *add_supermount(int major, int minor, char *source, char *options)
{
    struct supermount_struct *supermount=NULL;

    supermount=malloc(sizeof(struct supermount_struct));

    if (supermount) {
	int hashvalue=MKDEV(major, minor) % MOUNTTABLE_SIZE;

    	logoutput("add_supermount: creating supermount for %i:%i", major, minor);

	supermount->major=major;
	supermount->minor=minor;

	supermount->refcount=1;

	supermount->backend=NULL;
	supermount->fs=NULL;

	supermount->source=strdup(source);
	supermount->options=strdup(options);

	if (! supermount->source || ! supermount->options) {

	    goto error;

	}

	supermount->next=NULL;
	supermount->prev=NULL;

	pthread_mutex_lock(&supermount_mutex);

	if (supermount_list[hashvalue]) supermount_list[hashvalue]->prev=supermount;
	supermount->next=supermount_list[hashvalue];
	supermount_list[hashvalue]=supermount;

	pthread_mutex_unlock(&supermount_mutex);

    }

    return supermount;

    error:

    if (supermount) {

	if (supermount->options) {

	    free(supermount->options);
	    supermount->options=NULL;

	}

	if (supermount->source) {

	    free(supermount->source);
	    supermount->source=NULL;

	}

	free(supermount);
	supermount=NULL;

    }

    return supermount;


}

int remove_mount_supermount(struct supermount_struct *supermount)
{
    int refcount=0;

    pthread_mutex_lock(&supermount_mutex);

    supermount->refcount--;

    refcount=supermount->refcount;

    if (supermount->refcount<=0) {
	int hashvalue=MKDEV(supermount->major, supermount->minor) % MOUNTTABLE_SIZE;

	if (supermount->next) supermount->next->prev=supermount->prev;
	if (supermount->prev) supermount->prev->next=supermount->next;

	if (supermount==supermount_list[hashvalue]) supermount_list[hashvalue]=supermount->next;

	if (supermount->backend) {
	    struct notifyfs_backend_struct *backend=supermount->backend;

	    backend->refcount--;

	    /* here do something when refcount becomes zero or less */

	}

	free(supermount);

	refcount=0;

    }

    pthread_mutex_unlock(&supermount_mutex);

    return refcount;

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

int entry_hashfunction(void *data)
{
    struct notifyfs_entry_struct *entry=(struct notifyfs_entry_struct *) data;

    /* index for now, any other value is possible */

    return entry->index % SIMPLE_LIST_HASHSIZE;

}

struct notifyfs_entry_struct *create_entry(struct notifyfs_entry_struct *parent, const char *newname)
{
    struct notifyfs_entry_struct *entry=NULL;
    struct simple_list_struct *element;

    logoutput("create_entry: name %s", newname);

    pthread_mutex_lock(&group_entry.mutex);

    element=get_from_free(&group_entry);

    if (element) {

	entry=(struct notifyfs_entry_struct *) element->data;

    } else {

	entry = notifyfs_malloc_entry();

    }

    if (entry) {
	int lenname=strlen(newname);

	entry->name=notifyfs_malloc_data(lenname+1);

	if (entry->name==-1) {

	    /* unable to allocate space for name */

	    insert_in_free(&group_entry, element);
	    entry = NULL;

	} else {
	    char *name=get_data(entry->name);

	    memcpy(name, newname, lenname);

	    if (parent) {

		entry->parent = parent->index;
		entry->mount = parent->mount;

	    } else {

		entry->parent=-1;
		entry->mount=-1;

	    }

	    entry->inode=-1;
	    entry->name_next=-1;
	    entry->name_prev=-1;
	    entry->nameindex_value=0;

	    if (element) free(element);

	}

    }

    pthread_mutex_unlock(&group_entry.mutex);

    return entry;

}

void remove_entry(struct notifyfs_entry_struct *entry)
{
    struct simple_list_struct *element;

    pthread_mutex_lock(&group_entry.mutex);

    if ( entry->inode>=0 ) {
	struct notifyfs_inode_struct *inode=get_inode(entry->inode);

	inode->alias=-1;

	if (inode->attr>0) {
	    struct notifyfs_attr_struct *attr=get_attr(inode->attr);

	    remove_attr(attr);

	}

    }

    element=create_simple_element();

    if (element) {

	element->data=(void *) entry;
	insert_in_free(&group_entry, element);

    }

    entry->inode=-1;
    // entry->name=-1;
    entry->name_next=-1;
    entry->name_prev=-1;
    entry->parent=-1;
    entry->nameindex_value=0;
    entry->mount=-1;

    pthread_mutex_unlock(&group_entry.mutex);

}

int attr_hashfunction(void *data)
{
    struct notifyfs_attr_struct *attr=(struct notifyfs_attr_struct *) data;

    /* index for now, could be any value */

    return attr->index;

}


struct notifyfs_attr_struct *assign_attr(struct stat *st, struct notifyfs_inode_struct *inode)
{
    struct notifyfs_attr_struct *attr=NULL;
    struct simple_list_struct *element;

    pthread_mutex_lock(&group_attr.mutex);

    element=get_from_free(&group_attr);

    if (! element) {

	element=create_simple_element();

	if (element) {

	    attr=notifyfs_malloc_attr();

	    if (! attr ) {

		free(element);
		element=NULL;

	    } else {

		element->data=(void *) attr;

	    }

	}

    }

    if (element) {

	attr=(struct notifyfs_attr_struct *) element->data;

	inode->attr=attr->index;

	if (st) {

	    copy_stat(&attr->cached_st, st);
	    copy_stat_times(&attr->cached_st, st);

	}

	insert_in_used(&group_attr, element);

    }

    pthread_mutex_unlock(&group_attr.mutex);

    return attr;

}

void remove_attr(struct notifyfs_attr_struct *attr)
{
    struct simple_list_struct *element;

    pthread_mutex_lock(&group_attr.mutex);

    element=lookup_simple_list(&group_attr, (void *) attr);

    if (element) {

	move_from_used(&group_attr, element);
	insert_in_free(&group_attr, element);

    }

    pthread_mutex_unlock(&group_attr.mutex);

}

void assign_inode(struct notifyfs_entry_struct *entry)
{
    struct notifyfs_inode_struct *inode=create_inode();

    if (inode) {

	entry->inode=inode->index;
	inode->alias=entry->index;

    }

}

/* maintain a hash table of views per pid */

int calculate_view_hash(pid_t pid)
{
    return (pid % group_view.len);
}

int view_hashfunction(void *data)
{
    struct view_struct *view=(struct view_struct *) data;

    return calculate_view_hash(view->pid);

}

void activate_view(struct view_struct *view)
{

    pthread_mutex_lock(&group_view.mutex);

    if ( ! lookup_simple_list(&group_view, (void *) view)) {
	struct simple_list_struct *element=NULL;

	element=create_simple_element();

	if (element) {

	    element->data=(void *) view;
	    insert_in_used(&group_view, element);

	}

    }

    pthread_mutex_unlock(&group_view.mutex);

}

struct view_struct *get_next_view(pid_t pid, void **index)
{
    struct view_struct *view=NULL;

    pthread_mutex_lock(&group_view.mutex);

    while(1) {

	view=(struct view_struct *) get_next_element(&group_view, index, pid);

	if (!view) {

	    break;

	} else {

	    if (view->pid==pid) break;

	}

    }

    pthread_mutex_unlock(&group_view.mutex);

    return view;

}

int init_entry_management()
{
    int res;

    res=initialize_group(&group_mount, mount_hashfunction, 512);
    if (res<0) return res;

    res=initialize_group(&group_entry, entry_hashfunction, 0);
    if (res<0) return res;

    res=initialize_group(&group_attr, attr_hashfunction, 512);
    if (res<0) return res;

    res=initialize_group(&group_view, view_hashfunction, 512);
    if (res<0) return res;

    res=create_root();

    return res;

}

unsigned long long get_inoctr()
{
    return inoctr;
}

/* lookup the inode in the inode array 

    note that the relation between index in array and ino is that
    index starts at 0, and ino at FUSE_ROOT_ID (which is 1 normally)
*/

struct notifyfs_inode_struct *find_inode(fuse_ino_t ino)
{
    struct notifyfs_inode_struct *inode=NULL;
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

    logoutput("create_root: create root entry");

    rootentry=create_entry(NULL, ".");

    /* rootinode */

    logoutput("create_root: assign root inode");

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

