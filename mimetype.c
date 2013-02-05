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
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#include    <magic.h>

#define LOG_LOGAREA LOG_LOGAREA_MESSAGE

#include "logging.h"
#include "notifyfs.h"
#include "utils.h"

static magic_t notifyfs_magic_set=NULL;

int open_mimetype_database()
{
    int nreturn=0;

    notifyfs_magic_set=magic_open(MAGIC_ERROR|MAGIC_MIME);

    if (notifyfs_magic_set) {

	if (magic_load(notifyfs_magic_set, NULL)==-1) nreturn=-1;

    } else {

	nreturn=-1;

    }

}

const char *get_mimetype(const char *file)
{

    if(notifyfs_magic_set) return magic_file(notifyfs_magic_set, file);

    return NULL;

}


void close_mimetype_database()
{
    magic_close(notifyfs_magic_set);

    notifyfs_magic_set=NULL;
}
