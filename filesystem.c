/*
 
  2010, 2011, 2012, 2013 Stef Bon <stefbon@gmail.com>

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
#include <pwd.h>

#include "logging.h"
#include "utils.h"
#include "filesystem.h"

#include "filesystem-smb.h"
#include "filesystem-nfs.h"
#include "filesystem-ssh.h"

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

static struct notifyfs_filesystem_struct *supported_filesystems_first=NULL;
static struct notifyfs_filesystem_struct *supported_filesystems_last=NULL;
static pthread_mutex_t fs_mutex=PTHREAD_MUTEX_INITIALIZER;

static struct fsfunctions_struct *fsfunctions_list=NULL;
static pthread_mutex_t fsfunctions_mutex=PTHREAD_MUTEX_INITIALIZER;

#ifdef __gnu_linux__

#define DEFAULT_FILESYSTEMS_FILE	"/proc/filesystems"

struct notifyfs_filesystem_struct *read_supported_fs_os(char *name, int *error)
{
    FILE *fp;
    char line[256];
    struct notifyfs_filesystem_struct *fs_current=NULL, *fs_return=NULL, *fs=NULL;
    char *pos;
    int res;
    unsigned char nodev=0;

    logoutput("read_supported_fs_os");

    pthread_mutex_lock(&fs_mutex);

    fs_current=get_next_kernel_fs(NULL);

    fp=fopen(DEFAULT_FILESYSTEMS_FILE, "r");

    if (! fp) {

	if (error) *error=errno;
	goto unlock;

    }

    while (! feof(fp)) {

	if (! fgets(line, 256, fp)) continue;

	pos=strchr(line, '\n');
	if (pos) *pos='\0';

	nodev=0;

	if (strncmp(line, "nodev", 5)==0) {

	    pos=line+5;
	    nodev=1;

	} else {

	    pos=line;

	}

	convert_to(pos, UTILS_CONVERT_SKIPSPACE);

	/*
	    skip the fuse filesystems here, they are not really kernel filesystems 
	    these are evaluated at mounttime
	*/

	if (strcmp(pos, "fuse")==0 || strcmp(pos, "fuseblk")==0) continue;

	res=-1;
	fs=NULL;
	if (fs_current) res=strcmp(pos, fs_current->filesystem);

	if (res<0) {

	    /* add */

	    fs=create_filesystem();

	    if (fs) {

		if (fs_current) {

		    if (fs_current->prev) {

			fs_current->prev->next=fs;
			fs->prev=fs_current->prev;
			fs->next=fs_current;
			fs_current->prev=fs;

		    } else {

			fs->next=fs_current;
			fs_current->prev=fs;
			supported_filesystems_first=fs;

		    }

		} else {

		    if (supported_filesystems_last) supported_filesystems_last->next=fs;
		    fs->prev=supported_filesystems_last;
		    supported_filesystems_last=fs;
		    if (! supported_filesystems_first) supported_filesystems_first=fs;

		}

		if (name) {

		    if (strcmp(name, pos)==0) fs_return=fs;

		}

	    } else {

		if (error) *error=ENOMEM;
		break;

	    }

	} else if (res>0) {

	    /* removed, actually do nothing */

	    logoutput("read_supported_fs_linux: fs %s removed", pos);

	    if (name) {

		if (strcmp(name, pos)==0) fs_return=fs_current;

	    }

	    fs_current=get_next_kernel_fs(fs_current); /* fs_current is defined when res>0 */

	} else {

	    if (name) {

		if (strcmp(name, pos)==0) fs_return=fs_current;

	    }

	    fs_current=get_next_kernel_fs(fs_current); /* fs_current is defined when res=0 */

	}

	if (fs) {

	    strncpy(fs->filesystem, pos, 32);
	    fs->mode|=NOTIFYFS_FILESYSTEM_KERNEL;

	    /*
		determine the filesystem is watchable
	    */

	    if (nodev==1) {

		/* a nodev filesystem */

		fs->mode|=NOTIFYFS_FILESYSTEM_NODEV;

	    }

	    fs->fsfunctions=lookup_fsfunctions_byfs(fs->filesystem, 1);

	    if (fs->fsfunctions) {

		logoutput("read_supported_fs_linux: fs functions found for %s", fs->filesystem);

	    }

	}

    }

    fclose(fp);

    unlock:

    pthread_mutex_unlock(&fs_mutex);

    return fs_return;

}

#else

struct notifyfs_filesystem_struct *read_supported_fs_os(char *name, int *error)
{
    return NULL;
}

#endif


struct notifyfs_filesystem_struct *create_filesystem()
{
    struct notifyfs_filesystem_struct *fs=NULL;

    fs=malloc(sizeof(struct notifyfs_filesystem_struct));

    if (fs) {

	memset(&fs->filesystem, '\0', 32);
	fs->mode=0;

	fs->fsfunctions=NULL;

	fs->next=NULL;
	fs->prev=NULL;

    }

    return fs;

}

void add_filesystem(struct notifyfs_filesystem_struct *fs, unsigned char kernelfs)
{

    fs->fsfunctions=lookup_fsfunctions_byfs(fs->filesystem, kernelfs);

    /* add it at tail of list of filesystems */

    pthread_mutex_lock(&fs_mutex);

    fs->next=NULL;
    fs->prev=NULL;

    if (kernelfs==1) {

	/* add kernel filesystems at tail */

	if (supported_filesystems_last) supported_filesystems_last->next=fs;
	fs->prev=supported_filesystems_last;
	supported_filesystems_last=fs;

	if (! supported_filesystems_first) supported_filesystems_first=fs;

    } else {

	/* add non kernel filesystems at start */

	if (! supported_filesystems_first) {

	    supported_filesystems_first=fs;
	    supported_filesystems_last=fs;

	} else {

	    supported_filesystems_first->prev=fs;
	    fs->next=supported_filesystems_first;
	    supported_filesystems_first=fs;

	}

    }


    pthread_mutex_unlock(&fs_mutex);

}

struct notifyfs_filesystem_struct *get_next_kernel_fs(struct notifyfs_filesystem_struct *fs)
{

    if (fs) {

	fs=fs->next;

    } else {

	fs=supported_filesystems_first;

    }

    while(fs) {

	if (fs->mode & NOTIFYFS_FILESYSTEM_KERNEL) break;

	fs=fs->next;

    }

    return fs;

}

struct notifyfs_filesystem_struct *read_supported_filesystems(char *name, int *error)
{

    return read_supported_fs_os(name, error);

}


struct notifyfs_filesystem_struct *lookup_filesystem(char *name)
{
    struct notifyfs_filesystem_struct *fs=NULL;

    pthread_mutex_lock(&fs_mutex);

    fs=supported_filesystems_first;

    while(fs) {

	if (strcmp(fs->filesystem, name)==0) break;

	fs=fs->next;

    }

    pthread_mutex_unlock(&fs_mutex);

    return fs;

}

unsigned char is_fusefs(char *name, unsigned char *isnetwork)
{

    if (strncmp(name, "fuse.", 5)==0) {

	if (isnetwork && strcmp(name, "fuse.sshfs")==0) *isnetwork=1;
	return 1;

    }

    return 0;

}

void register_fsfunctions()
{

    register_smb_functions();
    register_nfs_functions();
    register_ssh_functions();

}

void add_fsfunctions(struct fsfunctions_struct *fsfunctions)
{
    pthread_mutex_lock(&fsfunctions_mutex);

    fsfunctions->next=fsfunctions_list;
    fsfunctions_list=fsfunctions;

    pthread_mutex_unlock(&fsfunctions_mutex);

}

struct fsfunctions_struct *lookup_fsfunctions_byfs(char *fs, unsigned char kernelfs)
{
    struct fsfunctions_struct *fsfunctions=NULL;

    pthread_mutex_lock(&fsfunctions_mutex);

    fsfunctions=fsfunctions_list;

    while(fsfunctions) {

	if (fsfunctions->apply) {

	    if ((*fsfunctions->apply)(fs, kernelfs)==1) break;

	}

	fsfunctions=fsfunctions->next;

    }

    pthread_mutex_unlock(&fsfunctions_mutex);

    return fsfunctions;

}

struct fsfunctions_struct *lookup_fsfunctions_byservice(char *service)
{
    struct fsfunctions_struct *fsfunctions=NULL;

    pthread_mutex_lock(&fsfunctions_mutex);

    fsfunctions=fsfunctions_list;

    while(fsfunctions) {

	if (fsfunctions->name) {

	    if (strcmp(fsfunctions->name, service)==0) break;

	}

	fsfunctions=fsfunctions->next;

    }

    pthread_mutex_unlock(&fsfunctions_mutex);

    return fsfunctions;

}

