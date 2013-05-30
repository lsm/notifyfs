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

#define NOTIFYFS_REMOTESERVERS_FILE_DEFAULT	"/etc/notifyfs/servers"

struct notifyfs_options_struct {
    pathstring socket;
    char *mountpoint;
    pathstring pidfile;
    char *conffile;
    char *remoteserversfile;
    unsigned char logging;
    int logarea;
    unsigned char accessmode;
    unsigned char testmode;
    unsigned char enablefusefs;
    unsigned char forwardlocal;
    unsigned char forwardnetwork;
    unsigned char listennetwork;
    unsigned char hidesystemfiles;
    unsigned char ipv6;
    gid_t shm_gid;
    int networkport;
    double attr_timeout;
    double entry_timeout;
    double negative_timeout;
};

// Prototypes

int parse_arguments(int argc, char *argv[], struct fuse_args *notifyfs_fuse_args);
int read_global_settings_from_file(char *path);

#endif
