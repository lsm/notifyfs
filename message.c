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

#include <inttypes.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#define LOG_LOGAREA LOG_LOGAREA_MESSAGE

#include "logging.h"
#include "client.h"
#include "message.h"
#include "mountstatus.h"


uint64_t uniquemessagectr=0;
pthread_mutex_t uniquectr_mutex=PTHREAD_MUTEX_INITIALIZER;

#ifdef USE_CMSGHDR

    /* create the control buffer, a control message plus unsigned char */

    msg.msg_controllen=CMSG_SPACE(sizeof(unsigned char));

    logoutput("send_mesage, controllen: %i", msg.msg_controllen);

    controlbuffer=malloc(msg.msg_controllen);

    if ( ! controlbuffer ) {

	nreturn=-ENOMEM;
	goto out;

    }

    msg.msg_control=controlbuffer;

    /* first header is used to indicate what kind of message this is (stored in unsigned char) */

    cmptr=CMSG_FIRSTHDR(&msg);
    cmptr->cmsg_len=CMSG_LEN(sizeof(unsigned char));
    cmptr->cmsg_type=0;
    //cmptr->cmsg_level=SOL_SOCKET;

    cmptr->cmsg_level=0;

    typeptr=CMSG_DATA(cmptr);

    memcpy(typeptr, &typemessage, sizeof(unsigned char));

#endif

int test_message_size(struct msghdr *msg)
{
    int size, i;

    size=sizeof(struct msghdr);

    logoutput("test_message_size A: %i", size);

    size+=msg->msg_iovlen * sizeof(struct iovec);

    logoutput("test_message_size B: %i", size);

    for (i=0;i<msg->msg_iovlen;i++) {

	size+=msg->msg_iov[i].iov_len;

    }

    logoutput("test_message_size C: %i", size);

    return size;
}

/* function to send a raw message */

int send_message(int fd, struct notifyfs_message_body *message, void *data1, int len1, void *data2, int len2)
{
    struct msghdr msg;
    // char *controlbuffer;
    struct iovec io_vector[3];
    int nreturn=0;

    msg.msg_controllen=0;
    msg.msg_control=NULL;

    msg.msg_name=NULL;
    msg.msg_namelen=0;

    io_vector[0].iov_base=(void *) message;
    io_vector[0].iov_len=sizeof(struct notifyfs_message_body);

    io_vector[1].iov_base=data1;
    io_vector[1].iov_len=len1;

    io_vector[2].iov_base=data2;
    io_vector[2].iov_len=len2;

    msg.msg_iov=io_vector;
    msg.msg_iovlen=3;

    /* the actual sending */

    nreturn=sendmsg(fd, &msg, 0);

    if ( nreturn==-1 ) {

	nreturn=-errno;

    }

    logoutput("send_message: return %i, message size computed: %i", nreturn, test_message_size(&msg));

    // free(controlbuffer);

    out:

    return nreturn;

}

/* general function send a fsevent to client, like:
   - notify event on watch like IN_ATTRIB, IN_DELETE, IN_MOVED 
   - set a watch in path or inode 
   - remove, wake or sleep a watch
*/

int send_fsevent_message(int fd, unsigned char typemessage, unsigned long id, int mask, char *path, int size)
{
    struct notifyfs_message_body message;
    int nreturn=0;

    message.type=NOTIFYFS_MESSAGE_TYPE_FSEVENT;

    message.body.fsevent.type=typemessage;
    message.body.fsevent.id=id;
    message.body.fsevent.mask=mask;
    message.body.fsevent.len=size;
    message.body.fsevent.unique=new_uniquectr();

    nreturn=send_message(fd, &message, (void *) path, size, NULL, 0);

    return nreturn;

}

/* specific function to send a setwatch message using the path*/

int send_setwatch_bypath_message(int fd, unsigned long id, int mask, char *path)
{

    return send_fsevent_message(fd, NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYPATH, id, mask, (void *)path, strlen(path));

}

/* specific function to send a setwatch message using the ino*/

int send_setwatch_byino_message(int fd, unsigned long id, int mask, unsigned long long ino)
{

    return send_fsevent_message(fd, NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYINO, id, mask, (void *)&ino, sizeof(unsigned long long));

}

/* specific function to send a delwatch message*/

int send_delwatch_message(int fd, unsigned long id)
{

    return send_fsevent_message(fd, NOTIFYFS_MESSAGE_FSEVENT_DELWATCH, id, 0, NULL, 0);

}

/* specific function to send a sleepwatch message*/

int send_sleepwatch_message(int fd, unsigned long id)
{

    return send_fsevent_message(fd, NOTIFYFS_MESSAGE_FSEVENT_SLEEPWATCH, id, 0, NULL, 0);

}

/* specific function to send a wakewatch message*/

int send_wakewatch_message(int fd, unsigned long id)
{

    return send_fsevent_message(fd, NOTIFYFS_MESSAGE_FSEVENT_WAKEWATCH, id, 0, NULL, 0);

}

/* specific function to send a notify message*/

int send_notify_message(int fd, unsigned long id, int mask, char *path, int len)
{

    return send_fsevent_message(fd, NOTIFYFS_MESSAGE_FSEVENT_DELWATCH, id, mask, (void *) path, len);

}

/* reply to a request with ok or error 
   when nerror=0 it's ok
   */

int reply_message(int fd, uint64_t unique, int nerror)
{
    struct notifyfs_message_body message;
    int nreturn=0;

    message.type=NOTIFYFS_MESSAGE_TYPE_REPLY;

    if ( nerror == 0 ) {

	message.body.reply.type=NOTIFYFS_MESSAGE_REPLY_OK;

    } else {

	message.body.reply.type=NOTIFYFS_MESSAGE_REPLY_ERROR;

    }

    message.body.reply.error=nerror;
    message.body.reply.status=0;
    message.body.reply.unique=unique;

    nreturn=send_message(fd, &message, NULL, 0, NULL, 0);

    return nreturn;

}

/* send a client message, from client to server, like:
   - register a client as app or as fs or both
   - signoff as client at server
   - give messagemask, to inform the server about what messages to receive, like mountinfo
   */

int send_client_message(int fd, unsigned char typemessage, char *path, int mask)
{
    struct notifyfs_message_body message;
    int nreturn=0;

    message.type=NOTIFYFS_MESSAGE_TYPE_CLIENT;

    message.body.client.type=typemessage;
    message.body.client.messagemask=mask;
    message.body.client.unique=new_uniquectr();

    if ( path ) {

	nreturn=send_message(fd, &message, (void *) path, strlen(path), NULL, 0);

    } else {

	nreturn=send_message(fd, &message, NULL, 0, NULL, 0);

    }

    return nreturn;

}

/* send a mount info request 
   this is typically send from a client wanting to know informataion about current mounts
   base of the request is a mount info struct which is a selector of the mounts the
   client is interested in:
   fstype: only mounts of type fstype
   basedir: only mounts at or in a subdir of basedir

   if every things works ok, the server will reply with a bunch of mountinfo
   messages, using the same unique id, and terminating qith a reply ok message, 
   also using the same unqiue id
   the client is responsible for using an unique id
   */

int send_mountinfo_request(int fd, const char *fstype, const char *basedir, uint64_t unique)
{
    struct notifyfs_message_body message;
    struct notifyfs_mount_message *mount_message;
    int nreturn=0;

    message.type=NOTIFYFS_MESSAGE_TYPE_MOUNTINFO_REQ;

    mount_message=&(message.body.mountinfo);

    memset(mount_message, 0, sizeof(struct notifyfs_mount_message));
    memset(mount_message->fstype, '\0', sizeof(mount_message->fstype));
    memset(mount_message->mountsource, '\0', sizeof(mount_message->mountsource));
    memset(mount_message->superoptions, '\0', sizeof(mount_message->superoptions));

    mount_message->unique=unique;

    if (fstype) {
	int len=strlen(fstype);

	if (len<sizeof(mount_message->fstype)) {

	    memcpy(mount_message->fstype, fstype, len);

	}

    }

    if (basedir) {

	nreturn=send_message(fd, &message, (void *) basedir, strlen(basedir), NULL, 0);

    } else {

	nreturn=send_message(fd, &message, NULL, 0, NULL, 0);

    }

    return nreturn;

}

uint64_t new_uniquectr()
{
    int res;

    res=pthread_mutex_lock(&uniquectr_mutex);

    uniquemessagectr++;

    res=pthread_mutex_unlock(&uniquectr_mutex);

    return uniquemessagectr;
}

void assign_message_callback(unsigned char type, void *callback, struct notifyfs_message_callbacks *cbs)
{
    if ( type==NOTIFYFS_MESSAGE_TYPE_CLIENT ) {

	cbs->client=callback;

    }  else if ( type==NOTIFYFS_MESSAGE_TYPE_FSEVENT ) {

	cbs->fsevent=callback;

    } else if ( type==NOTIFYFS_MESSAGE_TYPE_MOUNTINFO ) {

	cbs->mountinfo=callback;

    } else if ( type==NOTIFYFS_MESSAGE_TYPE_REPLY ) {

	cbs->reply=callback;

    } else if ( type==NOTIFYFS_MESSAGE_TYPE_MOUNTINFO_REQ ) {

	cbs->mirequest=callback;

    } else {

	logoutput("assign_server_message_callback: type %i not reckognized", type);

    }

}

int receive_preview_message(int fd, struct msghdr *msg)
{
    struct iovec buf_vector[3];
    int lendata0=sizeof(struct notifyfs_message_body);
    int lendata1=sizeof(pathstring);
    int lendata2=sizeof(pathstring);
    char messagebuffer[lendata0];
    char data1[lendata1];
    char data2[lendata2];
    ssize_t lenread;

    memset(msg, '\0', sizeof(struct msghdr));

    memset(messagebuffer, '\0', lendata0);
    memset(data1, '\0', lendata1);
    memset(data2, '\0', lendata2);

    buf_vector[0].iov_base=(void *) messagebuffer;
    buf_vector[0].iov_len=lendata0;

    buf_vector[1].iov_base=(void *) data1;
    buf_vector[1].iov_len=lendata1;

    buf_vector[2].iov_base=(void *) data2;
    buf_vector[2].iov_len=lendata2;

    msg->msg_iov=buf_vector;
    msg->msg_iovlen=3;

    msg->msg_control=NULL;
    msg->msg_controllen=0;

    msg->msg_name=NULL;
    msg->msg_namelen=0;

    lenread=recvmsg(fd, msg, MSG_PEEK);

    if ( lenread<=0 ){

	if ( lenread<0 ) {

	    logoutput("receive_preview_message: error %i reading the buffer", errno);
	    lenread=-errno;

	}

    }

    return lenread;

}


/* function to receive a message, reacting on data on fd via callbacks*/

int receive_message(int fd, struct client_struct *client, struct notifyfs_message_callbacks *message_cb)
{
    struct msghdr bufmsg, *msg;
    int nreturn=0;
    ssize_t lenread;


    /* determine the size of the first io element: it can be of different types */

    msg=&bufmsg;

    readbuffer:

    lenread=receive_preview_message(fd, msg);

    if ( lenread<=0 ){

	nreturn=lenread;
	goto out;

    } else if ( msg->msg_controllen==0 ) {

	/* a normal message,
          create the buffers just large enough with information */

	int lendata0=msg->msg_iov[0].iov_len;
	int lendata1=msg->msg_iov[1].iov_len;
	int lendata2=msg->msg_iov[2].iov_len;
	char data0[lendata0];
	char data1[lendata1];
	char data2[lendata2];
	struct iovec buf_vector[3];

	memset(msg, '\0', sizeof(struct msghdr));

	memset(data0, '\0', lendata0);
	memset(data1, '\0', lendata1);
	memset(data2, '\0', lendata2);

	buf_vector[0].iov_base=(void *) data0;
	buf_vector[0].iov_len=lendata0;

	buf_vector[1].iov_base=(void *) data1;
	buf_vector[1].iov_len=lendata1;

	buf_vector[2].iov_base=(void *) data2;
	buf_vector[2].iov_len=lendata2;

	msg->msg_iov=buf_vector;
	msg->msg_iovlen=3;

	msg->msg_control=NULL;
	msg->msg_controllen=0;

	msg->msg_name=NULL;
	msg->msg_namelen=0;

	/* do the real reading */

	lenread=recvmsg(fd, msg, MSG_WAITALL);

	if ( lenread<0 ) {

	    logoutput("receive_message_from_server: fd: %i, error %i", fd, errno);

	    nreturn=-errno;
	    goto out;

	} else {
	    struct notifyfs_message_body *message;
	    unsigned char typemessage;

	    logoutput("receive_message_from_server: fd: %i, %i bytes read", fd, lenread);

	    message=(struct notifyfs_message_body *) data0;
	    typemessage=message->type;

	    if ( typemessage==NOTIFYFS_MESSAGE_TYPE_MOUNTINFO ) {
		struct notifyfs_mount_message *mount_message;

		logoutput("received a mount info message");

		/* dealing with a message to parse a mount entry */
		/* body contains mount information, and the iovector mountpoint and possible rootpath */

		mount_message=&(message->body.mountinfo);

		if ( mount_message->status==MOUNT_STATUS_UP ) {

		    logoutput("mount message: up %s on %s type %s", mount_message->mountsource, (char *) msg->msg_iov[1].iov_base, mount_message->fstype);

		} else if ( mount_message->status==MOUNT_STATUS_REMOVE ) {

		    logoutput("mount message: down %s on %s type %s", mount_message->mountsource, (char *) msg->msg_iov[1].iov_base, mount_message->fstype);

		} else if ( mount_message->status==MOUNT_STATUS_SLEEP ) {

		    logoutput("mount message: down/sleep %s on %s type %s", mount_message->mountsource, (char *) msg->msg_iov[1].iov_base, mount_message->fstype);

		}

		if ( message_cb->mountinfo ) {

		    (*message_cb->mountinfo) (mount_message, (char *) msg->msg_iov[1].iov_base, (char *) msg->msg_iov[2].iov_base);

		}

	    } else if ( typemessage==NOTIFYFS_MESSAGE_TYPE_FSEVENT ) {

		logoutput("received a fs event message");

		if ( message_cb->fsevent ) {
		    struct notifyfs_fsevent_message *fsevent_message;

		    fsevent_message=(struct notifyfs_fsevent_message *) &(message->body.fsevent);

		    (*message_cb->fsevent) (client, fsevent_message, msg->msg_iov[1].iov_base, msg->msg_iov[1].iov_len);

		}

	    } else if ( typemessage==NOTIFYFS_MESSAGE_TYPE_CLIENT ) {

		logoutput("received a client message");

		if ( message_cb->client ) {
		    struct notifyfs_client_message *client_message;

		    logoutput("received a client message: before callback A");

		    client_message=(struct notifyfs_client_message *) &(message->body.client);

		    logoutput("received a client message: before callback B");

		    (*message_cb->client) (client, client_message, msg->msg_iov[1].iov_base, msg->msg_iov[1].iov_len);

		} else {

		    logoutput("received a client message: not callback C");

		}

	    } else if ( typemessage==NOTIFYFS_MESSAGE_TYPE_REPLY ) {

		logoutput("received a reply message");

		if ( message_cb->reply ) {
		    struct notifyfs_reply_message *reply_message;

		    reply_message=(struct notifyfs_reply_message *) &(message->body.reply);

		    (*message_cb->reply) (client, reply_message, msg->msg_iov[1].iov_base, msg->msg_iov[1].iov_len);

		}

	    } else if ( typemessage==NOTIFYFS_MESSAGE_TYPE_MOUNTINFO_REQ ) {

		logoutput("received a mountinfo request message");

		if ( message_cb->mirequest ) {
		    struct notifyfs_mount_message *mount_message;

		    /* dealing with a message to get mount info */

		    mount_message=&(message->body.mountinfo);

		    (*message_cb->mirequest) (client, mount_message, msg->msg_iov[1].iov_base, msg->msg_iov[2].iov_base);

		}

	    }

	}

	goto readbuffer;

    }

    out:

    return nreturn;

}
