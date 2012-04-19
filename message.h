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
#ifndef NOTIFYFS_MESSAGE_H
#define NOTIFYFS_MESSAGE_H

/* different types of messages:
   - an app registers (client->server)
   - a fs registers (client->server)
   - an app signs off (client->server)
   - this service sends a notify event to an app client (server->client)
   - this service sends a notify watch to client fs (server->client)
   - this service receives an notify event from a client fs (client->server)
*/



#define NOTIFYFS_MESSAGE_TYPE_NOTSET           	0
#define NOTIFYFS_MESSAGE_TYPE_FSEVENT          	1
#define NOTIFYFS_MESSAGE_TYPE_MOUNTINFO        	2
#define NOTIFYFS_MESSAGE_TYPE_CLIENT		3
#define NOTIFYFS_MESSAGE_TYPE_REPLY		4
#define NOTIFYFS_MESSAGE_TYPE_FD		5
#define NOTIFYFS_MESSAGE_TYPE_MOUNTINFO_REQ	6

#define NOTIFYFS_MESSAGE_CLIENT_REGISTERAPP      1
#define NOTIFYFS_MESSAGE_CLIENT_REGISTERFS       2
#define NOTIFYFS_MESSAGE_CLIENT_SIGNOFF          4

#define NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYPATH  1
#define NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYINO   2
#define NOTIFYFS_MESSAGE_FSEVENT_NOTIFY           3
#define NOTIFYFS_MESSAGE_FSEVENT_DELWATCH         4
#define NOTIFYFS_MESSAGE_FSEVENT_SLEEPWATCH       5
#define NOTIFYFS_MESSAGE_FSEVENT_WAKEWATCH        6

#define NOTIFYFS_MESSAGE_REPLY_OK                 1
#define NOTIFYFS_MESSAGE_REPLY_ERROR              2
#define NOTIFYFS_MESSAGE_REPLY_REPLACE            3

/* more replies follow like 
   reply by a client fs to a setwatch message from the server, to set a watch on another 
   location on the same host
   this is typically the case with simple FUSE overlay fs
   another example is the error message as reply*/

/* the mask a client sends about the type messages it wants to receive */
/*  what more here ?? 
    - apps by default receive messages about the messages set
*/

#define NOTIFYFS_MESSAGE_MASK_MOUNT             1

typedef char pathstring[PATH_MAX+1];

struct notifyfs_fsevent_message {
    uint64_t unique;
    unsigned char type;
    unsigned long id;
    int mask;
    unsigned len;
};

struct notifyfs_mount_message {
    uint64_t unique;
    char fstype[32];
    char mountsource[64];
    char superoptions[256];
    int major;
    int minor;
    unsigned char isbind;
    unsigned char isroot;
    unsigned char isautofs;
    unsigned char autofs_indirect;
    unsigned char autofs_mounted;
    unsigned char status;
};

struct notifyfs_client_message {
    uint64_t unique;
    unsigned char type;
    int messagemask;
};

struct notifyfs_reply_message {
    uint64_t unique;
    unsigned char type;
    unsigned status;
    int error;
};

struct notifyfs_message_body {
    unsigned char type;
    union {
	struct notifyfs_mount_message mountinfo;
	struct notifyfs_fsevent_message fsevent;
	struct notifyfs_client_message client;
	struct notifyfs_reply_message reply;
    } body;
};

struct notifyfs_message_callbacks {
    void (*fsevent) (struct client_struct *client, struct notifyfs_fsevent_message *fsevent_message, void *data1, int len1);
    void (*mountinfo) (struct notifyfs_mount_message *mount_message, char *mountpoint, char *rootpath);
    void (*client) (struct client_struct *client, struct notifyfs_client_message *client_message, void *data1, int len1);
    void (*reply) (struct client_struct *client, struct notifyfs_reply_message *reply_message, void *data1, int len1);
    void (*mirequest) (struct client_struct *client, struct notifyfs_mount_message *mount_message, void *data1, void *data2);
};

// Prototypes

int send_message(int fd, struct notifyfs_message_body *message, void *data1, int len1, void *data2, int len2);

int send_fsevent_message(int fd, unsigned char typemessage, unsigned long id, int mask, char *path, int size);
int send_setwatch_bypath_message(int fd, unsigned long id, int mask, char *path);
int send_setwatch_byino_message(int fd, unsigned long id, int mask, unsigned long long ino);
int send_delwatch_message(int fd, unsigned long id);
int send_sleepwatch_message(int fd, unsigned long id);
int send_wakewatch_message(int fd, unsigned long id);
int send_notify_message(int fd, unsigned long id, int mask, char *path, int len);

int reply_message(int fd, uint64_t unique, int nerror);

int send_client_message(int fd, unsigned char typemessage, char *path, int mask);

int send_mountinfo_request(int fd, const char *fstype, const char *basedir, uint64_t unique);


uint64_t new_uniquectr();
void assign_message_callback(unsigned char type, void *callback, struct notifyfs_message_callbacks *cbs);
int receive_message(int fd, struct client_struct *client, struct notifyfs_message_callbacks *message_cb);

#endif
