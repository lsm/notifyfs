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
#include <time.h>

#include <sys/stat.h>
#include <sys/param.h>
#include <sys/inotify.h>



#include "logging.h"
#include "utils.h"

#include "determinechanges.h"


#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

/* 	compare two stats by looking at the mtime
	*/

unsigned char compare_file_simple(struct stat *st1, struct stat *st2)
{
    unsigned char statchanged=FSEVENT_FILECHANGED_NONE;

    /* modification time eg file size , contents of the file, or in case of a directory, the number of entries 
       has changed... 
       on linux this means normally that mtime has been changed */

    if ( st1->st_mtime != st2->st_mtime ) {

	statchanged|=FSEVENT_FILECHANGED_FILE;

    }

#ifdef  __USE_MISC

    /* time defined as timespec */

    if ( st1->st_mtim.tv_nsec != st2->st_mtim.tv_nsec ) {

	statchanged|=FSEVENT_FILECHANGED_FILE;

    }

#else

    if ( st1->st_mtimensec != st2->st_mtimensec ) {

	statchanged|=FSEVENT_FILECHANGED_FILE;

    }


#endif

    return statchanged;

}



/* 	compare two stats by looking at the ctime
	*/

unsigned char compare_metadata_simple(struct stat *st1, struct stat *st2)
{
    unsigned char statchanged=FSEVENT_FILECHANGED_NONE;

    /* metadata changed like file owner, permissions, mode AND eventual extended attributes 
       on linux this means normally that ctime has been changed */

    if ( st1->st_ctime != st2->st_ctime ) {

	statchanged|=FSEVENT_FILECHANGED_METADATA;

    }

#ifdef  __USE_MISC

    /* time defined as timespec */

    if ( st1->st_ctim.tv_nsec != st2->st_ctim.tv_nsec ) {

	statchanged|=FSEVENT_FILECHANGED_METADATA;

    }

#else

    if ( st1->st_ctimensec != st2->st_ctimensec ) {

	statchanged|=FSEVENT_FILECHANGED_METADATA;

    }


#endif


    return statchanged;

}

/* function which determines and reads the changes when an event is reported 
   20120401: only called after inotify event, todo also call at other backends

   when something happens on a watch, this function reads the changes from the underlying fs and 
   store/cache that in notifyfs, and compare that with what it has in it's cache
   if there is a difference send a messages about that to clients....

   more todo: read the xattr
   when handling also the xattr an administration for these is also required

   */


unsigned char determinechanges(struct stat *cached_st, int mask, struct stat *st)
{
    int res;
    unsigned char filechanged=FSEVENT_FILECHANGED_NONE;

    if ( mask & IN_ATTRIB ) {
	unsigned char statchanged=FSEVENT_FILECHANGED_NONE;

	/* attributes have changed: read from underlying fs and compare */

	statchanged=compare_metadata_simple(cached_st, st);

	if ( statchanged & FSEVENT_FILECHANGED_METADATA ) {

	    filechanged|=statchanged;

	}

	/* here also the xattr ?? */
	/* under linux the metadata is changed when the extended attributes are changed */

    }

    if ( mask & IN_MODIFY ) {
	unsigned char statchanged=FSEVENT_FILECHANGED_NONE;

	/* attributes have changed: read from underlying fs and compare */

	statchanged=compare_file_simple(cached_st, st);

	if ( statchanged & FSEVENT_FILECHANGED_FILE ) {

	    filechanged|=statchanged;

	}

    }

    if ( filechanged&(FSEVENT_FILECHANGED_FILE|FSEVENT_FILECHANGED_METADATA) ) {

	if ( mask & ( IN_ATTRIB | IN_MODIFY )) {

	    copy_stat(cached_st, st);

	}

    }

    out:

    return filechanged;

}

void update_timespec(struct timespec *laststat)
{

    if ( laststat ) clock_gettime(CLOCK_REALTIME, laststat);

}
