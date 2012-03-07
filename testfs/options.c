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

#include <fcntl.h>
#include <dirent.h>

#include <inttypes.h>
#include <ctype.h>

#include <sys/stat.h>
#include <sys/param.h>

#include <fuse/fuse_lowlevel.h>

#include "options.h"

static void usage(const char *progname, FILE *f)
{
	fprintf(f, "usage: %s [opts] -o socket=FILE\n             [logging=NR,]\n             [logarea=NR,]\n             mountpoint\n", progname);
}


int testfs_options_output_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	(void) arg;
	(void) data;

	switch (key) {
	case KEY_HELP:
		usage(outargs->argv[0], stdout);
		printf(
		"General options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"    -d   -o debug          enable debug output (implies -f)\n"
		"    -f                     foreground operation\n"
		"\n"
		"notifyfs options:\n"
		"    -o socket=FILE         socket where clients connect to\n"
		"    -o logging=NUMBER      set loglevel (0=no logging)\n"
		"    -o logarea=NUMBER      set logarea (0=no area)\n"
		"\n"
		"FUSE options:\n"
                "    -o allow_other         allow access to other users\n"
                "    -o allow_root          allow access to root\n"
                "    -o auto_unmount        auto unmount on process termination\n"
                "    -o nonempty            allow mounts over non-empty file/dir\n"
                "    -o default_permissions enable permission checking by kernel\n"
                "    -o fsname=NAME         set filesystem name\n"
                "    -o subtype=NAME        set filesystem type\n");
		exit(0);
	case KEY_VERSION:
		printf("notifyfs version %s\n", PACKAGE_VERSION);
		fflush(stdout);
		dup2(1, 2);
		fuse_opt_add_arg(outargs, "--version");
		fuse_parse_cmdline(outargs, NULL, NULL, NULL);
		fuse_lowlevel_new(outargs, NULL, 0, NULL);
		exit(0);
	}
	return 1;
}

