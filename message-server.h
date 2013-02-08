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

void send_reply_message(int fd, uint64_t unique, int error, void *buffer, size_t size);
void send_delwatch_message(int fd, uint64_t unique, unsigned long id);
void send_changewatch_message(int fd, uint64_t unique, unsigned long id, unsigned char action);
void send_fsevent_message(int fd, uint64_t unique, unsigned long id, struct fseventmask_struct *fseventmask, int entryindex, struct timespec *detect_time);

#endif
