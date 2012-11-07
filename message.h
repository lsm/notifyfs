/*
  2010, 2011, 2012 Stef Bon <stefbon@gmail.com>

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
#ifndef NOTIFYFS_MESSAGE_H
#define NOTIFYFS_MESSAGE_H

#define NOTIFYFS_MESSAGE_TYPE_NOTSET           	0
#define NOTIFYFS_MESSAGE_TYPE_REGISTER		1
#define NOTIFYFS_MESSAGE_TYPE_LIST		2
#define NOTIFYFS_MESSAGE_TYPE_LISTREPLY		3

#define NOTIFYFS_MESSAGE_REGISTER_SIGNIN	1
#define NOTIFYFS_MESSAGE_REGISTER_SIGNOFF	2

#define NOTIFYFS_MESSAGEENTRYTYPE_DIR		1
#define NOTIFYFS_MESSAGEENTRYTYPE_NONDIR	2

struct notifyfs_register_message {
    uint64_t unique;
    unsigned char type;
    int messagemask;
    pid_t pid;
    pid_t tid;
};

struct notifyfs_list_message {
    uint64_t unique;
    int client_watch_id;
    int maxentries;
    unsigned char typeentry;
};

struct notifyfs_listreply_message {
    uint64_t unique;
    int sizereply;
    int count;
    unsigned char last;
};


struct notifyfs_message_body {
    unsigned char type;
    union {
	struct notifyfs_register_message register_message;
	struct notifyfs_list_message list_message;
	struct notifyfs_listreply_message listreply_message;
    } body;
};


// Prototypes

int send_message(int fd, struct notifyfs_message_body *message, void *data, int len);
uint64_t new_uniquectr();

#endif
