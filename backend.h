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
#ifndef NOTIFYFS_BACKEND_H
#define NOTIFYFS_BACKEND_H

#define NOTIFYFS_BACKENDSTATUS_NOTSET				0
#define NOTIFYFS_BACKENDSTATUS_DOWN				1
#define NOTIFYFS_BACKENDSTATUS_UP				2
#define NOTIFYFS_BACKENDSTATUS_ERROR				3
#define NOTIFYFS_BACKENDSTATUS_REGISTER				4
#define NOTIFYFS_BACKENDSTATUS_MOUNT				5

// Prototypes

void init_local_backend();
struct notifyfs_backend_struct *get_local_backend();

void lock_backends();
void unlock_backends();
struct notifyfs_backend_struct *get_next_backend(struct notifyfs_backend_struct *backend);
void add_backend_to_list(struct notifyfs_backend_struct *backend, unsigned char locked);
struct notifyfs_backend_struct *create_notifyfs_backend();

void change_status_backend(struct notifyfs_backend_struct *backend, unsigned char status);
void set_supermount_backend(struct supermount_struct *supermount, struct notifyfs_mount_struct *mount, char *mountpoint);
void read_remote_servers(char *path);

#endif
