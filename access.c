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

#include "logging.h"
#include "access.h"

#define LOG_LOGAREA LOG_LOGAREA_PATH_RESOLUTION
#define LINUX_GIDLIST_SIZE	25

unsigned char call_path_lock;
pthread_mutex_t call_path_mutex;
pthread_cond_t call_path_condition;

struct call_path_struct *call_path_list=NULL;
struct call_path_struct *call_path_unused=NULL;

pthread_mutex_t call_path_unused_mutex;

/*
    function which checks a process is part of a group
    it returns 	1 if it is
		0 if is not
		<0 when error

*/

int check_groups_pid(pid_t pid, gid_t group)
{
    int nreturn=0, fd=0, res;
    char path[64], *sep;
    char *buff=NULL;
    size_t size=1024;

    snprintf(path, 64, "/proc/%i/task/%i/status", (int) pid, (int) pid);

    while(1) {

	if (buff) {

	    buff=realloc(buff, size);

	} else {

	    buff=malloc(size);

	}

	if (! buff) {

	    nreturn=-ENOMEM;
	    goto out;

	}

	fd=open(path, O_RDONLY);

	if (fd<0) {

	    nreturn=-errno;
	    break;

	}

	res=read(fd, buff, size);
	close(fd);

	if (res==-1) {

	    nreturn=-errno;
	    break;

	} else if (res==size) {

	    size+=1024;
	    continue;

	}

	sep=strstr(buff, "\nGroups:");

	if (sep) {
	    char *pos;
	    char search[8];

	    sep+=strlen("\nGroups:");

	    pos=strchr(sep, '\n');
	    if (pos) *pos='\0';

	    snprintf(search, 8, " %i ", (int) group);
	    pos=strstr(sep, search);

	    if (pos) {

		/* group found */

		nreturn=1;

	    } else {

		int len=snprintf(search, 8, " %i", (int) group);

		*(search+len)='\0';
		pos=strstr(sep, search);

		if (pos) {

		    /* group found */

		    nreturn=1;

		}

	    }

	}

	break;

    }

    out:

    free(buff);
    buff=NULL;

    return nreturn;


}

int get_groups_pid(pid_t pid, struct gidlist_struct *gidlist)
{
    int nreturn=0, fd=0, res;
    char path[64], *sep;
    char *buff=NULL;
    size_t size=1024;

    snprintf(path, 64, "/proc/%i/task/%i/status", (int) pid, (int) pid);

    logoutput("get_groups_pid: open %s", path);

    while(1) {

	if (buff) {

	    buff=realloc(buff, size);

	} else {

	    buff=malloc(size);

	}

	if (! buff) {

	    nreturn=-ENOMEM;
	    goto out;

	}

	fd=open(path, O_RDONLY);

	if (fd<0) {

	    nreturn=-errno;
	    break;

	}

	res=read(fd, buff, size);
	close(fd);

	if (res==-1) {

	    nreturn=-errno;
	    break;

	} else if (res==size) {

	    size+=1024;
	    continue;

	}

	sep=strstr(buff, "\nGroups:");

	if (sep) {
	    char *pos;
	    char search[8];

	    sep+=strlen("\nGroups: ");

	    while(1) {

		unsigned long value=strtoul(sep, &pos, 0);

		if (pos==sep) break;

		sep=pos;

		if (nreturn<gidlist->len) gidlist->list[nreturn]=value;
		nreturn++;

	    }

	}

	break;

    }

    out:

    free(buff);
    buff=NULL;

    return nreturn;

}

    /* deny execute and write on everything but directories */

//    if ( ! S_ISDIR(st->st_mode)) {

//        if ( mask & ( X_OK | W_OK) ) {

            /* deny execute and write */

//            nreturn=-EACCES;
//            goto out;

//        }

//    } else {

	/* when a directory, everybody can read */

//	if ( mask==R_OK ) {

//	    nreturn=1;
//	    goto out;

//	}

//    }

static int get_new_groupslist(pid_t pid, struct gidlist_struct *gidlist)
{
    int nreturn=0, res;

    /* if not allocated the gid array do it now */

    if (!gidlist->list) {

	gidlist->len=LINUX_GIDLIST_SIZE;
	gidlist->list=malloc(gidlist->len * sizeof(gid_t));

	if ( ! gidlist->list) {

	    nreturn=-ENOMEM;
	    gidlist->len-=LINUX_GIDLIST_SIZE;
	    goto out;

	}

    }

    retry:

    res=get_groups_pid(pid, gidlist);

    if (res<0) {

	nreturn=res;
	goto out;

    } else if (res>gidlist->len) {

	/* too much groups to fit into array: increase array and retry */

	res+=5; /* to be sure.. */
	gidlist->list=realloc(gidlist->list, res * sizeof(gid_t));

	if ( ! gidlist->list) {

	    nreturn=-ENOMEM;
	    goto out;

	}

	gidlist->len=res;
	goto retry;

    } else {

	gidlist->nr=res;
	nreturn=res;

    }

    out:

    return nreturn;

}


int check_supplgroups_pid(pid_t pid, gid_t gid, struct gidlist_struct *gidlist)
{
    int nreturn=0, i;

    if ( ! gidlist->list) {

	/* only get a new groups list when not already */

	int res=get_new_groupslist(pid, gidlist);

	if (res<0) {

	    nreturn=res;
	    goto out;

	} else {

	    if (gidlist->nr==0) {

		/* no group found: ready */
		goto out;

	    }

	}

    }

    for (i=0;i<gidlist->nr;i++) {

	if (gidlist->list[i]==gid) {

	    /* value found */

	    nreturn=1;
	    break;

	}

    }

    out:

    return nreturn;

}

/* check access (mask) to a file (st) using user context (ctx)
   return:
   - <0 : access denied with error
   - =0 : access denied
   - =1 : access granted

*/

unsigned char check_access_process(pid_t pid, uid_t uid, gid_t gid, struct stat *st, int mask, struct gidlist_struct *gidlist)
{

    if (uid==0) {

	/* root has always access */

	return 1;

    }

    if ( uid==st->st_uid ) {

        /* check the owner mode */

        int res=(st->st_mode & S_IRWXU) >> 6;

        if (res & mask) return 1;

    }


    if ( gid==st->st_gid ) {

        /* check the group mode */

        int res=(st->st_mode & S_IRWXG) >> 3;

        if (res & mask) return 1;

    }

    /* check the supplementary groups*/

    if (check_supplgroups_pid(pid, st->st_gid, gidlist)==1) {

        int res=(st->st_mode & S_IRWXG) >> 3;

        if (res & mask) return 1;

    }

    /* TODO check the ACL via Xattr 
    */

    /* check the other mask */

    if (st->st_mode & S_IRWXO & mask) return 1;

    out:

    return 0;

}

int check_access_fusefs(pid_t pid, uid_t uid, gid_t gid, struct stat *st, int mask, struct gidlist_struct *gidlist)
{
    int nreturn=0, res;

    if (uid==0) {

	nreturn=1;
	goto out;

    }

    /* can user read entries: x and r have to be set for user/group */

    if ( uid==st->st_uid ) {

        /* check the owner mode */

        res=(st->st_mode & S_IRWXU) >> 6;

        if (res & mask) {

	    nreturn=1;
            goto out;

        }

        /* in case of a directory allow the "creating" of entries when rx access */

        if ( mask&W_OK && S_ISDIR(st->st_mode)) {

            /* allow when read and execute permission */

            if (res & ( R_OK | X_OK )) {

		nreturn=1;
        	goto out;

	    }

        }

    }


    if ( gid==st->st_gid ) {

        /* check the group mode */

        res=(st->st_mode & S_IRWXG) >> 3;

        if (res & mask) {

	    nreturn=1;
	    goto out;

	}

        /* in case of a directory allow the "creating" of entries when rx access */

        if ( mask&W_OK && S_ISDIR(st->st_mode)) {

            /* allow when read and execute permission */

            if (res & ( R_OK | X_OK )) {

		nreturn=1;
		goto out;

	    }

        }

    }

    /* check the supplementary groups*/

    res=check_supplgroups_pid(pid, st->st_gid, gidlist);

    if ( res==1 ) {

        res=(st->st_mode & S_IRWXG) >> 3;

        if (res & mask) {

	    nreturn=1;
            goto out;

        }

        /* in case of a directory allow the "creating" of entries when rx access */

        if ( mask&W_OK && S_ISDIR(st->st_mode)) {

            /* allow when read and execute permission */

            if (res & ( R_OK | X_OK )) {

		nreturn=1;
                goto out;

            }

        }

    }

    /* TODO check the ACL via Xattr 
    */

    /* check the other mask */


    res=(st->st_mode & S_IRWXO);

    if (res & mask) {

        nreturn=1;
        goto out;

    }

    /* in case of a directory allow the "creating" of entries */

    if ( mask&W_OK && S_ISDIR(st->st_mode)) {

        /* allow when read and execute permission */

        if (res & ( R_OK | X_OK )) {

    	    nreturn=1;

        }

    }

    out:

    return (nreturn>0) ? 1 : 0;

}


