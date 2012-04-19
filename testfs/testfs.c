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
#include <sys/epoll.h>
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
#include "path-resolution.h"
#include "watches.h"
#include "logging.h"
#include "testfs.h"


#include "epoll-utils.h"
#include "handlefuseevent.h"

#include "utils.h"
#include "options.h"
#include "xattr.h"
#include "socket.h"
#include "client.h"
#include "changestate.h"

#include "message.h"
#include "message-client.h"

struct testfs_options_struct testfs_options;

struct fuse_chan *testfs_chan;
struct testfs_entry_struct *root_entry;

unsigned char loglevel=0;
int logarea=0;

static void testfs_lookup(fuse_req_t req, fuse_ino_t parentino, const char *name)
{
    struct fuse_entry_param e;
    struct testfs_entry_struct *entry;
    struct testfs_inode_struct *inode;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);
    int nreturn=0;
    struct call_info_struct call_info;
    unsigned char entrycreated=0;

    logoutput1("LOOKUP, name: %s", name);

    init_call_info(&call_info, NULL);

    entry=find_entry(parentino, name);

    if ( ! entry ) {
	struct testfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
	    struct testfs_entry_struct *pentry;

	    pentry=pinode->alias;

	    entry=create_entry(pentry, name, NULL);
	    if ( ! entry ) {

		nreturn=-ENOMEM;
		goto out;

	    }

	} else {

	    nreturn=-ENOENT;
	    goto out;

	}

	entrycreated=1;

    }

    inode=entry->inode;

    call_info.entry=entry;
    call_info.ctx=ctx;

    /* translate entry into path */

    nreturn=determine_path(&call_info, TESTFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    /* check entry on underlying fs */

    nreturn=lstat(call_info.path, &(e.attr));

    if ( nreturn==-1 ) {

	nreturn=-errno;

    } else {

	/* here copy e.attr to inode->st */

	if (inode) copy_stat(&(inode->st), &(e.attr));

    }

    out:

    if ( nreturn==-ENOENT ) {

	logoutput2("lookup: entry does not exist (ENOENT)");

	e.ino = 0;
	e.entry_timeout = testfs_options.negative_timeout;

    } else if ( nreturn<0 ) {

	logoutput1("do_lookup: error (%i)", nreturn);

    } else {

	// no error

	if ( entrycreated==1 ) {

	    // when created here, add it to the various tables, is not done yet

	    assign_inode(entry);

	    add_to_inode_hash_table(entry->inode);
	    add_to_name_hash_table(entry);

	}

	entry->inode->nlookup++;
	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = testfs_options.attr_timeout;
	e.entry_timeout = testfs_options.entry_timeout;

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

        if ( entrycreated==0 ) {

	    logoutput("lookup: changestate");
    	    changestate(&call_info, WATCH_ACTION_REMOVE);

	}

    }

    if ( call_info.path ) {

	logoutput("lookup: free path");
	free(call_info.path);
	logoutput("lookup: free path after");

    }

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
    call_info.ctx=ctx;

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

    if ( nreturn==-1 ) {

	nreturn=-errno;

    } else {

	copy_stat(&inode->st, &st);

    }

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

static void testfs_mknod(fuse_req_t req, fuse_ino_t parentino, const char *name, mode_t mode, dev_t dev)
{
    struct fuse_entry_param e;
    struct testfs_entry_struct *entry;
    int nreturn=0;
    unsigned char entrycreated=0;
    struct call_info_struct call_info;

    logoutput("MKNOD, name: %s", name);

    init_call_info(&call_info, NULL);
    call_info.ctx=fuse_req_ctx(req);

    entry=find_entry(parentino, name);

    if ( ! entry ) {
        struct testfs_inode_struct *pinode;

	pinode=find_inode(parentino);

	if ( pinode ) {
            struct testfs_entry_struct *pentry;

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

    nreturn=determine_path(&call_info, TESTFS_PATH_NONE);
    if ( nreturn<0 ) goto out;

    /* only create something here when it does exist in the underlying fs */

    nreturn=mknod(call_info.path, mode, dev);

    if ( nreturn==-1 ) {

	if ( errno==EEXIST ) {

	    if ( entrycreated==1 ) {

		/* when created: handle it as if it is created here */

		nreturn=0;

	    } else {

		nreturn=-errno;

	    }

	} else {

	    nreturn=-errno;

	}

    }

    if ( nreturn==0 ) {

	nreturn=lstat(call_info.path, &(e.attr));

	if ( nreturn==-1 ) {

	    nreturn=-errno;

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
	e.attr_timeout = testfs_options.attr_timeout;
	e.entry_timeout = testfs_options.entry_timeout;

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

        logoutput("mknod succesfull");

        fuse_reply_entry(req, &e);
        if ( call_info.path ) free(call_info.path);

        return;

    }

    error:

    logoutput("mknod: error %i", nreturn);

    if ( entrycreated==1 ) remove_entry(entry);

    e.ino = 0;
    e.entry_timeout = testfs_options.negative_timeout;

    fuse_reply_err(req, abs(nreturn));

    if ( call_info.path ) free(call_info.path);

}


int readlink_localhost(struct call_info_struct *call_info, char **link)
{
    int size = PATH_MAX;
    char *buf=NULL;
    int res, nreturn=0;

    logoutput("readlink_localhost");

    buf = malloc(size);

    if ( ! buf ) {

	nreturn=-ENOMEM;
	goto out;

    }

    do {

    	res = readlink(call_info->path, buf, size);

	if ( res==-1) {
	    nreturn=-errno;
	    *link=NULL;
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

	if ( ! buf ) {

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

    nreturn=determine_path(&call_info, TESTFS_PATH_NONE);
    if (nreturn<0) goto out;

    nreturn = readlink_localhost(&call_info, &link);

    out:

    if (nreturn < 0) {

	logoutput("readlink, return %i", nreturn);

	fuse_reply_err(req, -nreturn);

    } else {

	logoutput("readlink, link %s", link);

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

    call_info->ctx=ctx;

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

    logoutput("STATFS");

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

    logoutput("statfs, B, nreturn: %i", nreturn);

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
    call_info.ctx=ctx;
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
    call_info.ctx=ctx;

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
    call_info.ctx=ctx;
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

void handle_data_on_inotify_fd(struct epoll_extended_data_struct *epoll_xdata, uint32_t events, int signo)
{
    int nreturn=0;
    char outputstring[256];

    logoutput1("handle_data_on_inotify_fd.");

    if ( events & EPOLLIN ) {
        int lenread=0;
        char buff[INOTIFY_BUFF_LEN];

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

                effective_watch=lookup_watch(FSEVENT_BACKEND_METHOD_INOTIFY, i_event->wd);

                if ( effective_watch ) {

		    /* test it's an event on a watch long gone 
		       watches should be disabled and removed when entry/inodes are removed, 
		       so receiving messages on them should 
		       not happen, but to make sure */

		    if ( ! effective_watch->inode ) {

			goto next;

		    } else if ( effective_watch->inode->status!=FSEVENT_INODE_STATUS_OK ) {

			goto next;

		    }

                    if ( i_event->name && i_event->len>0 ) {

                        /* something happens on an entry in the directory.. check it's in use
                           by this fs, the find command will return NULL if it isn't 

                           do nothing with close, open and access*/

			if ( i_event->mask & ( IN_DELETE | IN_MOVED_FROM | IN_ATTRIB | IN_CREATE | IN_MOVED_TO | IN_IGNORED | IN_UNMOUNT ) ) {
			    struct testfs_entry_struct *entry=NULL;

			    entry=find_entry(effective_watch->inode->ino, i_event->name);

			    if ( entry ) {
                    		struct call_info_struct call_info;

				/* entry is part of this fs */

                    		init_call_info(&call_info, entry);

                    		nreturn=determine_path(&call_info, TESTFS_PATH_FORCE);
                    		if (nreturn<0) continue;

				if ( i_event->mask & ( IN_DELETE | IN_MOVED_FROM | IN_IGNORED | IN_UNMOUNT ) ) {
				    unsigned char prestatus=FSEVENT_INODE_STATUS_REMOVE;
				    struct testfs_inode_struct *inode=entry->inode;

				    /* entry deleted (here) so adjust the filesystem */

				    if ( inode ) prestatus=inode->status;

				    changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE);

				    if ( prestatus!=FSEVENT_INODE_STATUS_REMOVE ) {

					if ( inode && inode->status==FSEVENT_INODE_STATUS_REMOVE) {

					    /* send a message to server/client to inform */

					    send_notify_message(testfs_options.notifyfssocket_fd, effective_watch->id, i_event->mask, i_event->name, i_event->len);

					}

				    }

				} else if ( i_event->mask & ( IN_CREATE | IN_MOVED_TO ) ) {
				    unsigned char filechanged=0;

				    /* entry created... and does exist already here... huhhh??? */

				    /* here compare the new values (attributes) with the cached ones,
			    		and only forward a message when there has something really changed..*/

				    filechanged=determinechanges(&call_info, i_event->mask);

				    if ( filechanged != TESTFS_FILECHANGED_NONE ) {

					send_notify_message(testfs_options.notifyfssocket_fd, effective_watch->id, i_event->mask, i_event->name, i_event->len);

				    }

				} else if ( i_event->mask & IN_ATTRIB ) {
				    unsigned char filechanged=0;

				    /* here compare the new values (attributes) with the cached ones,
			    		and only forward a message when there has something really changed..*/

				    filechanged=determinechanges(&call_info, i_event->mask);

				    if ( filechanged != TESTFS_FILECHANGED_NONE ) {

					send_notify_message(testfs_options.notifyfssocket_fd, effective_watch->id, i_event->mask, i_event->name, i_event->len);

				    }

				}

			    } else {

				/* entry not part of fs */

				send_notify_message(testfs_options.notifyfssocket_fd, effective_watch->id, i_event->mask, i_event->name, i_event->len);

			    }

			}

		    } else {

			/* event on watch self

                           do nothing with close, open and access*/

			if ( i_event->mask & ( IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB | IN_IGNORED | IN_UNMOUNT ) ) {
			    struct testfs_entry_struct *entry=NULL;

			    entry=effective_watch->inode->alias;

			    if ( entry ) {
                    		struct call_info_struct call_info;

				/* entry is part of this fs */

                    		init_call_info(&call_info, entry);

                    		nreturn=determine_path(&call_info, TESTFS_PATH_FORCE);
                    		if (nreturn<0) continue;

				if ( i_event->mask & ( IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED | IN_UNMOUNT ) ) {
				    unsigned char prestatus=FSEVENT_INODE_STATUS_REMOVE;
				    struct testfs_inode_struct *inode=entry->inode;

				    /* entry deleted (here) so adjust the filesystem */

				    if ( inode ) prestatus=inode->status;

				    changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE);

				    if ( prestatus!=FSEVENT_INODE_STATUS_REMOVE ) {

					if ( inode && inode->status==FSEVENT_INODE_STATUS_REMOVE) {

					    /* send a message to server/client to inform */

					    send_notify_message(testfs_options.notifyfssocket_fd, effective_watch->id, i_event->mask, NULL, 0);

					}

				    }

				} else if ( i_event->mask & IN_ATTRIB ) {
				    unsigned char filechanged=0;

				    /* here compare the new values (attributes) with the cached ones,
			    		and only forward a message when there has something really changed..*/

				    filechanged=determinechanges(&call_info, i_event->mask);

				    if ( filechanged != TESTFS_FILECHANGED_NONE ) {

					send_notify_message(testfs_options.notifyfssocket_fd, effective_watch->id, i_event->mask, NULL, 0);

				    }

				}

			    }

			}

                    }

		}

		next:

                i += INOTIFY_EVENT_SIZE + i_event->len;

            }

        }

    }

}



/* callback which acts on the recieving of a setwatch by ino message */

void handle_setwatch_byino(struct notifyfs_fsevent_message *fsevent, unsigned long long ino)
{
    struct testfs_inode_struct *inode=NULL;
    int nreturn=0;

    inode=find_inode(ino);

    if ( ! inode ) {

	/* inode has to exist */

	nreturn=-ENOENT;
	goto out;

    }

    if ( fsevent->mask==0 ) {

	/* mask may not be null (possible remove an existing watch??) */

	nreturn=-EINVAL;
	goto out;

    }


    {

	struct call_info_struct call_info;
	struct testfs_entry_struct *entry=NULL;
	struct effective_watch_struct *effective_watch;
	int oldmask, res;

	init_call_info(&call_info, NULL);

	// the entry, it has to exist

	entry=inode->alias;

	if ( ! entry ) {

	    nreturn=-ENOENT;
	    goto out;

	}

	call_info.entry=entry;

	/* translate entry into path */

	nreturn=determine_path(&call_info, TESTFS_PATH_NONE);
	if (nreturn<0) goto out;

        effective_watch=inode->effective_watch;

        if ( ! effective_watch ) {

	    /* create the watch */

	    effective_watch=get_effective_watch();

            if ( ! effective_watch ) {

                nreturn=-ENOMEM;
                goto out;

            }

	    /* use the id from sender (server) for later communication */

	    effective_watch->id=fsevent->id;

	    /* mutual links */

            effective_watch->inode=inode;
            inode->effective_watch=effective_watch;

	    effective_watch->path=call_info.path; /* make sure when call_info is freed, keep the path */

            add_effective_watch_to_list(effective_watch);

	}

	res=lock_effective_watch(effective_watch);

	if ( effective_watch->typebackend==FSEVENT_BACKEND_METHOD_NOTSET ) {

	    set_backend(&call_info, effective_watch);

	}

	oldmask=effective_watch->mask;

	if ( oldmask!=fsevent->mask ) {

	    /* only set when different */

	    set_watch_at_backend(effective_watch, fsevent->mask);

	    effective_watch->mask=fsevent->mask;

	}

	res=unlock_effective_watch(effective_watch);

    }

    out:

    if ( nreturn<0 ) {

	logoutput("handle_setwatch_byino: error %i", nreturn);

    }

}

/* callback which acts on the recieving of a setwatch by path message 
   this path may not exist in the fs, it is created here 
   TODO permissions checking to do so... */

void handle_setwatch_bypath(struct notifyfs_fsevent_message *fsevent, char *path)
{
    struct testfs_inode_struct *inode=NULL;
    struct testfs_entry_struct *entry=NULL;
    int nreturn=0;

    entry=create_fs_path(path);

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    inode=entry->inode;

    if ( ! inode ) {

	/* inode has to exist */

	nreturn=-ENOENT;
	goto out;

    }

    if ( fsevent->mask==0 ) {

	/* mask may not be null (possible remove an existing watch??) */

	nreturn=-EINVAL;
	goto out;

    }


    {

	struct call_info_struct call_info;
	struct effective_watch_struct *effective_watch;
	int oldmask, res;

	init_call_info(&call_info, NULL);

	// the entry, it has to exist

	entry=inode->alias;

	if ( ! entry ) {

	    nreturn=-ENOENT;
	    goto out;

	}

	call_info.entry=entry;
	call_info.path=path;

        effective_watch=inode->effective_watch;

        if ( ! effective_watch ) {

	    /* create the watch */

	    effective_watch=get_effective_watch();

            if ( ! effective_watch ) {

                nreturn=-ENOMEM;
                goto out;

            }

	    /* use the id from sender (server) for later communication */

	    effective_watch->id=fsevent->id;

	    /* mutual links */

            effective_watch->inode=inode;
            inode->effective_watch=effective_watch;

	    effective_watch->path=strdup(path);

            add_effective_watch_to_list(effective_watch);

	}

	res=lock_effective_watch(effective_watch);

	if ( effective_watch->typebackend==FSEVENT_BACKEND_METHOD_NOTSET ) {

	    set_backend(&call_info, effective_watch);

	}

	oldmask=effective_watch->mask;

	if ( oldmask!=fsevent->mask ) {

	    /* only set when different */

	    set_watch_at_backend(effective_watch, fsevent->mask);

	    effective_watch->mask=fsevent->mask;

	}

	res=unlock_effective_watch(effective_watch);

    }

    out:

    if ( nreturn<0 ) {

	logoutput("handle_setwatch_byino: error %i", nreturn);

    }

}


/* remove a watch, identified by fsevent->id, which has been set before.. */

void handle_delwatch(struct notifyfs_fsevent_message *fsevent)
{
    struct effective_watch_struct *effective_watch;

    effective_watch=lookup_watch(FSEVENT_BACKEND_METHOD_NOTSET, fsevent->id);

    if ( effective_watch ) {
	int res;

	res=lock_effective_watch(effective_watch);

	del_watch_at_backend(effective_watch);

	remove_effective_watch_from_list(effective_watch, 0);

	move_effective_watch_to_unused(effective_watch);

	res=unlock_effective_watch(effective_watch);

    } else {

	logoutput("handle_delwatch: error removing watch with id %li, watch not found.", fsevent->id);

    }

}

void handle_fsevent_message(struct client_struct *client,  struct notifyfs_fsevent_message *fsevent_message, void *data1, int len1)
{
    unsigned char type=fsevent_message->type;

    if ( type==NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYPATH ) {

	logoutput("handle_fsevent_message: setwatch_bypath");

	/* here read the data, it must be complete:
	   - path and mask 
	   then set the watch at backend
	*/

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYINO ) {

	logoutput("handle_fsevent_message: setwatch_byino");

	/* here read the data, it must be complete:
	   - ino and mask 
	   then set the watch at backend
	*/

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_NOTIFY ) {

	logoutput("handle_fsevent_message: notify");

	/* read the data from the client fs 
	   and pass it through to client apps 
	   but first filter it out as it maybe an event caused by this fs
	   and because it comes through a message it's an event on the backend
	   howto determine....
	   it's a fact that inotify events have been realised on the VFS,
	   with events on the backend this is not so
	   but first filter out the events caused by this host....*/


    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_DELWATCH ) {

	logoutput("handle_fsevent_message: delwatch");

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_SLEEPWATCH ) {

	logoutput("handle_fsevent_message: sleepwatch");

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_WAKEWATCH ) {

	logoutput("handle_fsevent_message: wakewatch");

    } else {

	logoutput("handle_fsevent_message: unknown message");

    }

}



/* todo: (symlink,) readlink, open, read, (write,) close, setattr */

static struct fuse_lowlevel_ops testfs_oper = {
	.init		= testfs_init,
	.destroy	= testfs_destroy,
	.lookup		= testfs_lookup,
	.forget		= testfs_forget,
	.getattr	= testfs_getattr,
	.mknod		= testfs_mknod,
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
    struct fuse_args testfs_fuse_args = FUSE_ARGS_INIT(0, NULL);
    struct fuse_session *testfs_session;
    char *testfs_mountpoint;
    int res, epoll_fd;
    struct epoll_extended_data_struct xdata_inotify={0, 0, NULL, NULL, NULL, NULL}; 
    struct epoll_extended_data_struct xdata_socket={0, 0, NULL, NULL, NULL, NULL};

    umask(0);

    // set logging

    openlog("fuse.testfs", 0,0); 

    /* parse commandline options and initialize fuse options */

    res=parse_arguments(argc, argv, &testfs_fuse_args);

    if (res<0) {

	if ( res==-2 ) fprintf(stderr, "Error parsing options.\n");
	goto skipeverything;

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
    //

    res=create_root();

    if ( res<0 ) {

	fprintf(stderr, "Error, failed to create the root entry(error: %i).\n", res);
	exit(1);

    }

    //
    // set default options
    //

    loglevel=testfs_options.logging;
    logarea=testfs_options.logarea;

    testfs_options.attr_timeout=1.0;
    testfs_options.entry_timeout=1.0;
    testfs_options.negative_timeout=1.0;

    if ( (testfs_chan = fuse_mount(testfs_options.mountpoint, &testfs_fuse_args)) == NULL) {

        logoutput0("Error mounting and setting up a channel.");
        goto out;

    }

    testfs_session=fuse_lowlevel_new(&testfs_fuse_args, &testfs_oper, sizeof(testfs_oper), NULL);

    if ( testfs_session == NULL ) {

        logoutput0("Error starting a new session.");
        goto out;

    }

    res = fuse_daemonize(0);

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
	struct epoll_extended_data_struct *epoll_xdata=NULL;

	/*
    	    connect to the notify fs socket clients

	*/

	testfs_options.notifyfssocket_fd=connect_socket(testfs_options.notifyfssocket);

	if ( testfs_options.notifyfssocket_fd<=0 ) {

    	    logoutput("Error connecting to notifyfs socket: %i.", testfs_options.notifyfssocket_fd);
    	    goto out;

	}

	/* set the callbacks, only setting and deleting is here used */

	assign_message_callback_client(NOTIFYFS_MESSAGE_TYPE_FSEVENT, &handle_fsevent_message);

	/* add notifyfs socket to epoll */

	epoll_xdata=add_to_epoll(testfs_options.notifyfssocket_fd, EPOLLIN, TYPE_FD_SOCKET, &handle_data_on_connection_fd, NULL, &xdata_socket);

	if ( ! epoll_xdata ) {

    	    logoutput("Error adding socket fd %i to mainloop", testfs_options.notifyfssocket_fd);
    	    goto out;

	} else {

    	    logoutput("socket fd %i added to mainloop", testfs_options.notifyfssocket_fd);

	    add_xdata_to_list(epoll_xdata);

	}

	/* register as client fs to notifyfs */

	sleep(1);

	send_client_message(testfs_options.notifyfssocket_fd, NOTIFYFS_MESSAGE_CLIENT_REGISTERFS, testfs_options.mountpoint, strlen(testfs_options.mountpoint));

	if ( res<=0 ) {

	    logoutput("Error sending register fs message to notifyfs: %i.", res);

	} else {

	    logoutput("Sending register fs message: %i nr bytes written.", res);

	}

    }


    /*
    *    add an inotify instance to epoll : default backend 
    *
    */

    testfs_options.inotify_fd=inotify_init();

    if ( testfs_options.inotify_fd<=0 ) {

        logoutput("Error creating inotify fd: %i.", testfs_options.inotify_fd);
        goto out;

    } else {
	struct epoll_extended_data_struct *epoll_xdata=NULL;

	/* add inotify to mainloop */

	epoll_xdata=add_to_epoll(testfs_options.inotify_fd, EPOLLIN, TYPE_FD_INOTIFY, &handle_data_on_inotify_fd, NULL, &xdata_inotify);

	if ( ! epoll_xdata ) {

    	    logoutput("Error adding inotify fd %i to mainloop.", testfs_options.inotify_fd);
    	    goto out;

	} else {

    	    logoutput("inotify fd %i added to epoll", testfs_options.inotify_fd);

	    add_xdata_to_list(epoll_xdata);

	}

    }

    /* add the fuse channel(=fd) to the mainloop */

    res=addfusechannelstomainloop(testfs_session, testfs_mountpoint);

    /* handle error here */

    res=startfusethreads();

    /* handle error again */

    res=epoll_mainloop();

    out:

    terminatefuse(NULL);

    if ( xdata_inotify.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_inotify, 0);
	close(xdata_inotify.fd);
	xdata_inotify.fd=0;
	testfs_options.inotify_fd=0;
	remove_xdata_from_list(&xdata_inotify);

    }

    if ( xdata_socket.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_socket, 0);
	close(xdata_socket.fd);
	xdata_socket.fd=0;
	testfs_options.notifyfssocket_fd=0;
	remove_xdata_from_list(&xdata_socket);

    }

    destroy_mainloop();

    fuse_opt_free_args(&testfs_fuse_args);

    skipeverything:

    closelog();

    return 0;

}
