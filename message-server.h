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
#ifndef NOTIFYFS_MESSAGE_SERVER_H
#define NOTIFYFS_MESSAGE_SERVER_H

// Prototypes

void assign_message_callback_server(unsigned char type, void *callback);

int send_mount_message(int fd, struct mount_entry_struct *mount_entry, uint64_t unique);
int send_fd_message(int fd, int fdtosend);

int receive_message_from_client(int fd, struct client_struct *client);

#endif
