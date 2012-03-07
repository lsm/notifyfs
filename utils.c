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



//
// unslash a path, remove double slashes and a trailing slash
//

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

    st_to->st_atim.tv_nsec=st_from->st_atim.tv_nsec
    st_to->st_mtim.tv_nsec=st_from->st_mtim.tv_nsec
    st_to->st_ctim.tv_nsec=st_from->st_ctim.tv_nsec

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

