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

#include "logging.h"
#include "utils.h"
#include "filesystem.h"
#include "filesystem-nfs.h"
#include "networkutils.h"

#ifdef __gnu_linux__
#include "filesystem-nfs-linux.c"
#else

static int getNFSremotehost(char *source, char *options, char *host, int len, unsigned char *islocal)
{
    return 0;
}

static void constructNFSurl(char *source, char *options, char *path, char *url, int len)
{

}

static char *processNFSurl(char *url)
{
    return NULL;
}

static char *getNFSlocalpath(char *fs, char *options, char *source, char *relpath)
{
    return NULL;
}

static unsigned char apply_nfs(char *fs, unsigned char kernelfs)
{
    return 0;
}

#endif




static struct fsfunctions_struct nfs_functions={
	.name		= "nfs*",
	.apply		= apply_nfs,
	.process_url	= processNFSurl,
	.get_localpath	= getNFSlocalpath,
	.get_remotehost	= getNFSremotehost,
	.construct_url	= constructNFSurl,
	.next		= NULL
};

void register_nfs_functions()
{
    add_fsfunctions(&nfs_functions);

}

