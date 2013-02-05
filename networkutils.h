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
#ifndef NOTIFYFS_NETWORKUTILS_H
#define NOTIFYFS_NETWORKUTILS_H

#define NOTIFYFS_SERVERTYPE_NOTSET				0
#define NOTIFYFS_SERVERTYPE_FUSE				1
#define NOTIFYFS_SERVERTYPE_NETWORK				2

#define NOTIFYFS_SERVERSTATUS_NOTSET				0
#define NOTIFYFS_SERVERSTATUS_DOWN				1
#define NOTIFYFS_SERVERSTATUS_UP				2
#define NOTIFYFS_SERVERSTATUS_ERROR				3

struct notifyfs_filesystem_struct {
    char filesystem[32];
    unsigned char networkfs;
    unsigned char fusefs;
    struct notifyfs_filesystem_struct *next;
    struct notifyfs_filesystem_struct *prev;
};

struct notifyfs_server_struct {
    unsigned char type;
    struct notifyfs_connection_struct *connection;
    unsigned char status;
    int error;
    struct timespec connect_time;
    void *data;
};

// Prototypes

struct notifyfs_server_struct *get_mount_backend(struct notifyfs_mount_struct *mount);

void set_mount_backend(struct notifyfs_mount_struct *mount);
void unset_mount_backend(struct notifyfs_mount_struct *mount);

void connect_remote_notifyfs_server(char *ipv4address);
void read_remote_servers(char *path);
void init_networkutils();

void determine_remotepath(struct notifyfs_mount_struct *mount, char *path, char *notifyfs_url, int len);

#endif
