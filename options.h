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
#ifndef NOTIFYFS_OPTIONS_H
#define NOTIFYFS_OPTIONS_H

#define NOTIFYFS_OPT(t, p, v) { t, offsetof(struct notifyfs_commandline_options_struct, p), v }

enum {
     KEY_HELP,
     KEY_VERSION,
};

struct notifyfs_commandline_options_struct {
     char *socket;
     unsigned char logging;
     unsigned char logarea;
     unsigned char accessmode;
     unsigned char testmode;
};

// Prototypes

int notifyfs_options_output_proc(void *data, const char *arg, int key, struct fuse_args *outargs);

#endif



