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
#include <sys/types.h>
#include <sys/inotify.h>
#include <fcntl.h>

//
// unslash a path, remove double slashes and a trailing slash
// 20120412: replaced by realpath

void unslash(char *p)
{
    char *q = p;
    char *pkeep = p;

    while ((*q++ = *p++) != 0) {

	if (q[-1] == '/') {

	    while (*p == '/') {

		p++;
	    }

	}
    }

    if (q > pkeep + 2 && q[-2] == '/') q[-2] = '\0';
}


int compare_stat_time(struct stat *ast, struct stat *bst, unsigned char ntype)
{
    if ( ntype==1 ) {

	if ( ast->st_atime > bst->st_atime ) {

	    return 1;

	} else if ( ast->st_atime == bst->st_atime ) {

#ifdef  __USE_MISC

	    // time defined as timespec

	    if ( ast->st_atim.tv_nsec > bst->st_atim.tv_nsec ) return 1;

#else

	    if ( ast->st_atimensec > bst->st_atimensec ) return 1;

#endif

	}

    } else if ( ntype==2 ) {

	if ( ast->st_mtime > bst->st_mtime ) {

	    return 1;

	} else if ( ast->st_mtime == bst->st_mtime ) {

#ifdef  __USE_MISC

	    // time defined as timespec

	    if ( ast->st_mtim.tv_nsec > bst->st_mtim.tv_nsec ) return 1;

#else

	    if ( ast->st_mtimensec > bst->st_mtimensec ) return 1;

#endif

	}

    } else if ( ntype==3 ) {

	if ( ast->st_ctime > bst->st_ctime ) {

	    return 1;

	} else if ( ast->st_ctime == bst->st_ctime ) {

#ifdef  __USE_MISC

	    // time defined as timespec

	    if ( ast->st_ctim.tv_nsec > bst->st_ctim.tv_nsec ) return 1;

#else

	    if ( ast->st_ctimensec > bst->st_ctimensec ) return 1;

#endif

	}

    }

    return 0;

}

void copy_stat(struct stat *st_to, struct stat *st_from)
{


if ( st_to && st_from) {

    st_to->st_mode=st_from->st_mode;
    st_to->st_nlink=st_from->st_nlink;
    st_to->st_uid=st_from->st_uid;
    st_to->st_gid=st_from->st_gid;

    st_to->st_rdev=st_from->st_rdev;
    st_to->st_size=st_from->st_size;

    st_to->st_atime=st_from->st_atime;
    st_to->st_mtime=st_from->st_mtime;
    st_to->st_ctime=st_from->st_ctime;

#ifdef  __USE_MISC

    /* time defined as timespec */

    st_to->st_atim.tv_nsec=st_from->st_atim.tv_nsec;
    st_to->st_mtim.tv_nsec=st_from->st_mtim.tv_nsec;
    st_to->st_ctim.tv_nsec=st_from->st_ctim.tv_nsec;

#else

    /* n sec defined as extra field */

    st_to->st_atimensec=st_from->st_atimensec;
    st_to->st_mtimensec=st_from->st_mtimensec;
    st_to->st_ctimensec=st_from->st_ctimensec;

#endif

    st_to->st_blksize=st_from->st_blksize;
    st_to->st_blocks=st_from->st_blocks;

    }

}

/* function to test path1 is a subdirectory of path2 */

unsigned char issubdirectory(const char *path1, const char *path2, unsigned char maybethesame)
{
    int lenpath2=strlen(path2);
    int lenpath1=strlen(path1);
    unsigned char issubdir=0;

    if ( maybethesame==1 ) {

	if ( lenpath1 < lenpath2 ) goto out;

    } else {

	if ( lenpath1 <= lenpath2 ) goto out;

    }

    if ( strncmp(path2, path1, lenpath2)==0 ) {

	if ( lenpath1>lenpath2 ) {

	    if ( strcmp(path2, "/")==0 ) {

		/* path2 is / */

		issubdir=2;

	    } else if ( strncmp(path1+lenpath2, "/", 1)==0 ) {

		/* is a real subdirectory */
		issubdir=2;

	    }

	} else {

	    /* here: lenpath1==lenpath2, since the case lenpath1<lenpath2 is checked earlier */

	    /* directories are the same here... and earlier tested this is only a subdir when maybethesame==1 */

	    issubdir=1;

	}

    }

    out:

    return issubdir;

}

/* a way to check two pids belong to the same process 
    to make this work process_id has to be the main thread of a process
    and thread_id is a process id of some thread of a process
    under linux then the directory
    /proc/<process_id>/task/<thread_id> 
    has to exist

    this does not work when both processes are not mainthreads
    20120426: looking for a better way to do this
*/

unsigned char belongtosameprocess(pid_t process_id, pid_t thread_id)
{
    char tmppath[40];
    int res;
    unsigned char sameprocess=0;
    struct stat st;

    snprintf(tmppath, 40, "/proc/%i/task/%i", process_id, thread_id);
    res=lstat(tmppath, &st);
    if (res!=-1) sameprocess=1;

    return sameprocess;
}

/* function to get the process id (PID) where the TID is given
   this is done by first looking /proc/tid exist
   if this is the case, then the tid is the process id
   if not, check any pid when walking back if this is
   the process id*/

pid_t getprocess_id(pid_t thread_id)
{
    pid_t process_id=0;
    char tmppath[40];
    int res;
    unsigned char sameprocess=0;
    struct stat st;

    process_id=thread_id;

    snprintf(tmppath, 40, "/proc/%i", process_id);
    res=stat(tmppath, &st);

    if ( res==0 ) goto out;


    checkpid:

    process_id--;

    if ( process_id==1 ) {

	/* prevent going too far */

	process_id=0;
	goto out;

    }

    snprintf(tmppath, 40, "/proc/%i", process_id);
    res=stat(tmppath, &st);

    if ( res==0 ) {

	/* directory does exist, check the thread is part of this process */

	snprintf(tmppath, 40, "/proc/%i/task/%i", process_id, thread_id);
	res=stat(tmppath, &st);

	if ( res==0 ) {

	    /* it does exist: found! */

	    goto out;

	} else {

	    /* another process, do stop here, since the process_id is the first pid bigger
               than thread_id which appears in /proc/ */

	    process_id=0;
	    goto out;

	}

    }

    goto checkpid;

    out:

    return process_id;

}

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


int print_mask(unsigned int mask, char *string, size_t size)
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

                    *(string+pos)='|';
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

