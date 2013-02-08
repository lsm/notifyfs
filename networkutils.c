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

#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netdb.h>
#include <pwd.h>

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

#include "logging.h"
#include "notifyfs-io.h"
#include "path-resolution.h"
#include "epoll-utils.h"
#include "socket.h"
#include "networkutils.h"
#include "message.h"
#include "handlemessage.h"
#include "utils.h"
#include "options.h"


struct remotehost_info_struct {
    char *host;
    int len_host;
    char *user;
    int len_user;
    char *path;
    int len_path;
    char *ipv4address;
};

static struct notifyfs_server_struct *mount_serverbackend[MOUNTTABLE_SIZE];
static unsigned char initialized=0;
static pthread_mutex_t mountbackend_list_mutex=PTHREAD_MUTEX_INITIALIZER;

static struct notifyfs_server_struct notifyfs_servers_list[32];
static pthread_mutex_t servers_list_mutex=PTHREAD_MUTEX_INITIALIZER;
static int servers_ctr=0;

extern struct notifyfs_options_struct notifyfs_options;


/* function to determine the ipv4 address, given a name of a remote host */

char *get_ipv4address(const char *host, const char *service)
{
    struct addrinfo *ailist, *aip, ai_hint;
    int res;
    char *ipv4address=NULL;

    ai_hint.ai_family=AF_INET; /* only ipv4 for now....*/
    ai_hint.ai_flags=AI_CANONNAME | AI_NUMERICSERV;
    ai_hint.ai_socktype=0;
    ai_hint.ai_protocol=0;

    ai_hint.ai_canonname=NULL;
    ai_hint.ai_addr=NULL;
    ai_hint.ai_next=NULL;

    ipv4address=malloc(INET_ADDRSTRLEN);

    if ( ! ipv4address ) goto out;

    memset(ipv4address, '\0', INET_ADDRSTRLEN);

    res=getaddrinfo(host, service, &ai_hint, &ailist);

    if ( res!=0 ) {

	logoutput("get_remote_address: error %s", gai_strerror(res));
	free(ipv4address);
	ipv4address=NULL;
	goto out;

    }

    aip=ailist;

    while (aip) {

	/* use only AF_INET (ipv4) for now... */

	if(aip->ai_family==AF_INET) {
	    struct sockaddr_in *networksocket=(struct sockaddr_in *) aip->ai_addr;

	    /* convert to ipv4 text format */

	    if ( ! inet_ntop(AF_INET, &networksocket->sin_addr, ipv4address, INET_ADDRSTRLEN) ) {

		logoutput("get_remote_address: unable to get address, error %i", errno);

	    } else {

		logoutput("get_remote_address: ipv4 address found %s", ipv4address);
		break;

	    }

	}

	aip=aip->ai_next;

    }

    freeaddrinfo(ailist);

    out:

    return ipv4address;

}

/* function to get the value of field from the options */

void get_value_from_options(char *options, const char *option, char *value, int len)
{
    char *poption;

    if ( ! options || ! option ) return;

    poption=strstr(options, option);

    if ( poption ) {
	char *endoption=strchrnul(poption, ',');
	char *issign=strchr(poption, '=');

	if ( issign ) {

	    if ( issign<endoption && endoption-issign < len ) {

		memcpy(value, issign+1, endoption-issign-1);

	    }

	}

    }

}

static int determine_remotehost_sshfs(char *mountsource, char *superoptions, struct remotehost_info_struct *remotehost_info)
{
    char *separator1, *separator2;
    int nreturn=0;

    /* remote host is in mountsource */

    /* sshfs uses the user@xxx.xxx.xxx.xxx:/path format when in ipv4
	user is optional, which defaults to the user making the connection
        path is also optional, it defaults to the home directory of the user on the remote host
    */

    if (! mountsource || ! superoptions) return -EINVAL;


    /* look for user part */

    separator1=strchr(mountsource, '@');

    if ( separator1 ) {

	/* there is a user part */

	if ( separator1-mountsource < remotehost_info->len_user ) {

	    memcpy(remotehost_info->user, mountsource, separator1-mountsource);

	}

	separator1++;

    } else {

	separator1=mountsource;

    }

    /* look for host part (there must be a : )*/

    separator2=strchr(separator1, ':');

    if ( separator2 ) {
	int len;

	if ( separator2-separator1<remotehost_info->len_host ) {

	    memcpy(remotehost_info->host, separator1, separator2-separator1);

	}

	len=strlen(separator2+1);

	if ( len>0 ) {

	    /* there is a path part */

	    if ( len<remotehost_info->len_path ) {

		memcpy(remotehost_info->path, separator2+1, len);

	    }

	}

    } else {

	/* error: no ":" separator */

	nreturn=-EINVAL;
	goto out;

    }


    if ( strlen(remotehost_info->user)==0 ) {

	/* if no user part: get it from options 
	    with sshfs the uid is used of the user running sshfs 
	    note this is a number and not a name

	    it's only required to know the user to determine the home directory on the remote host
	    this is a task of the remote notifyfs process

	*/

	get_value_from_options(superoptions, "user_id", remotehost_info->user, remotehost_info->len_user);

    }

    out:

    return nreturn;

}

static int determine_remotehost_cifs(char *mountsource, char *superoptions, struct remotehost_info_struct *remotehost_info)
{

    /* remote "directory" is not set, but the share in SMB world. store this in stead of path */

    /* remote host is in superoptions */

    /* cifs stores the remote host in field addr= in options 
       and the user in username 
	only ipv4 for now...

	it's up to the remote notifyfs process to translate the smb share format into a directory

    */

    get_value_from_options(superoptions, "addr", remotehost_info->host, remotehost_info->len_host);
    get_value_from_options(superoptions, "username", remotehost_info->user, remotehost_info->len_user);

    return 0;

}

static int determine_remotehost_nfs(char *mountsource, char *superoptions, struct remotehost_info_struct *remotehost_info)
{
    char *separator=strrchr(mountsource, ':');

    /* remote host is in superoptions */

    /* nfs is very simple:
       the mountsource is like server:/path/to/share

	what to do with user?? assume transparant id handling
	actually user is not required here

    */

    if (separator) {
	int len;

	/* host part */

	len=separator-mountsource;

	if (len < remotehost_info->len_host) {

	    memcpy(remotehost_info->host, mountsource, len);

	}

	/* path part */

	len=strlen(separator+1);

	if (len<remotehost_info->len_path) {

	    memcpy(remotehost_info->path, separator, len);

	}

    }

    return 0;

}


/* function to get remote host and information about the directory on that remote host from the mountsource 
   with sshfs this is simple
   it gets more difficult with cifs, mountsource is in netbios format
   and with ...*/

int determine_remotehost(char *fs, char *source, char *options, struct remotehost_info_struct *remotehost_info)
{
    int nreturn=0;

    memset(remotehost_info->host, '\0', remotehost_info->len_host);
    memset(remotehost_info->path, '\0', remotehost_info->len_path);
    memset(remotehost_info->user, '\0', remotehost_info->len_user);

    if ( strcmp(fs, "fuse.sshfs")==0 ) {

	nreturn=determine_remotehost_sshfs(source, options, remotehost_info);

	if (nreturn>=0) {

	    if(strlen(remotehost_info->host)>0) {

		/* is remote host is found: try to get the remote addr in the right format 
		    TODO: what to do when the service is not 22 */

		remotehost_info->ipv4address=get_ipv4address(remotehost_info->host, "22");

	    }

	}

    } else if ( strcmp(fs, "cifs")==0 ) {

	nreturn=determine_remotehost_cifs(source, options, remotehost_info);

	if (nreturn>=0) {

	    if(strlen(remotehost_info->host)>0) {

		/* is remote host is found: try to get the remote addr in the right format 
		    TODO: what to do when the service is not 445*/

		remotehost_info->ipv4address=get_ipv4address(remotehost_info->host, "445");

	    }

	}

    } else if (strcmp(fs, "nfs")==0) {

	nreturn=determine_remotehost_nfs(source, options, remotehost_info);

	if (nreturn>=0) {

	    if(strlen(remotehost_info->host)>0) {

		/* is remote host is found: try to get the remote addr in the right format 
		    TODO: what to do when the service is not 2049*/

		remotehost_info->ipv4address=get_ipv4address(remotehost_info->host, "2049");

	    }

	}

    }

    return nreturn;

}

unsigned char is_networkfs(char *fs)
{

    logoutput("is_networkfs: check %s", fs);

    if (strcmp(fs, "fuse.sshfs")==0 || strcmp(fs, "cifs")==0 || strcmp(fs, "nfs")==0) {

	return 1;

    }

    return 0;

}

unsigned char is_fusefs(char *fs)
{

    logoutput("is_fusefs: check %s", fs);

    if (strncmp(fs, "fuse.", 5)==0) {

	return 1;

    }

    return 0;

}

/* look for notifyfsserver using mount */

static struct notifyfs_server_struct *lookup_notifyfsserver_permount(int mountindex)
{
    if (mountindex<MOUNTTABLE_SIZE) return mount_serverbackend[mountindex];

    return NULL;

}

/* look for notifyfsserver using the remote ipv4address */

static struct notifyfs_server_struct *lookup_notifyfsserver_peripv4(char *ipv4address)
{
    struct notifyfs_server_struct *notifyfs_server=NULL;
    int i;

    if ( ! ipv4address ) goto out;

    for (i=0;i<servers_ctr;i++) {

	notifyfs_server=&notifyfs_servers_list[i];

	if (notifyfs_server->type==NOTIFYFS_SERVERTYPE_NETWORK) {

	    if (notifyfs_server->data) {

		if (strcmp((char *) notifyfs_server->data, ipv4address)==0) break;

	    } else {
		struct notifyfs_connection_struct *connection=notifyfs_server->connection;

		if (connection) {
		    struct sockaddr_in address;
		    socklen_t len=sizeof(struct sockaddr_in);

		    if (getpeername(connection->fd, (struct sockaddr *) &address, &len)==0) {

			if (address.sin_addr.s_addr==inet_addr(ipv4address)) break;

		    }

		}

	    }

	}

        notifyfs_server=NULL;

    }

    out:

    return notifyfs_server;

}

void init_notifyfs_server(struct notifyfs_server_struct *notifyfs_server)
{

    notifyfs_server->type=NOTIFYFS_SERVERTYPE_NOTSET;
    notifyfs_server->status=NOTIFYFS_SERVERSTATUS_NOTSET;
    notifyfs_server->error=0;
    notifyfs_server->data=NULL;

    notifyfs_server->connect_time.tv_sec=0;
    notifyfs_server->connect_time.tv_nsec=0;

    pthread_mutex_init(&notifyfs_server->mutex, NULL);

    notifyfs_server->connection=NULL;

}

void lock_notifyfs_server(struct notifyfs_server_struct *notifyfs_server)
{
    int res=pthread_mutex_lock(&notifyfs_server->mutex);
}

void unlock_notifyfs_server(struct notifyfs_server_struct *notifyfs_server)
{
    int res=pthread_mutex_unlock(&notifyfs_server->mutex);
}


void init_networkutils()
{
    int i;

    pthread_mutex_lock(&mountbackend_list_mutex);

    for (i=0;i<MOUNTTABLE_SIZE;i++) {

	mount_serverbackend[i]=NULL;


    }

    pthread_mutex_unlock(&mountbackend_list_mutex);

    pthread_mutex_lock(&servers_list_mutex);

    for (i=0;i<32;i++) {

	init_notifyfs_server(&notifyfs_servers_list[i]);

    }

    pthread_mutex_unlock(&servers_list_mutex);

}


struct notifyfs_server_struct *create_notifyfs_server()
{
    struct notifyfs_server_struct *notifyfs_server=NULL;

    /* get server from list */

    pthread_mutex_lock(&servers_list_mutex);

    if (servers_ctr<32) {

	notifyfs_server=&notifyfs_servers_list[servers_ctr];
	servers_ctr++;

    }

    pthread_mutex_unlock(&servers_list_mutex);

    return notifyfs_server;

}

static int process_message_from_backend(struct notifyfs_connection_struct *connection, uint32_t events)
{

    /* just receive the message: the callback should be intelligent enough to detect it's a message from the remote notifyfs server */

    return receive_message(connection->fd, connection->data, events, is_remote(connection));

}

struct notifyfs_server_struct *compare_notifyfs_servers(int fd)
{
    struct sockaddr_in sock0;
    socklen_t len0=sizeof(struct sockaddr_in);
    struct notifyfs_server_struct *notifyfs_server=NULL;

    if (getpeername(fd, (struct sockaddr *) &sock0, &len0)==0) {
	int i;

	pthread_mutex_lock(&servers_list_mutex);

	for (i=0;i<servers_ctr;i++) {

	    notifyfs_server=&notifyfs_servers_list[i];

	    if (notifyfs_server->type==NOTIFYFS_SERVERTYPE_NETWORK) {
		struct notifyfs_connection_struct *connection=notifyfs_server->connection;

		if (connection) {
		    struct sockaddr_in sock1;
		    socklen_t len1=sizeof(struct sockaddr_in);

		    if (getpeername(connection->fd, (struct sockaddr *) &sock1, &len1)==0) {

			/* here compare both connections 
			    if they are the same: return -1
			*/

			if (sock0.sin_addr.s_addr==sock1.sin_addr.s_addr) break;

		    }

		} else if (notifyfs_server->data) {

		    /* if connection the server data remains, is not removed.. just set a flag it's down/error 
			the ipv4 address is stored in data, compare that */

		    if (strcmp((char *) notifyfs_server->data, inet_ntoa(sock0.sin_addr))==0) break;

		}

	    }

	    notifyfs_server=NULL;

	}

	pthread_mutex_unlock(&servers_list_mutex);

    }

    return notifyfs_server;

}


struct notifyfs_server_struct *get_mount_backend(struct notifyfs_mount_struct *mount)
{
    struct notifyfs_server_struct *notifyfs_server=NULL;
    int res=0;

    logoutput("get_mount_backend: testing on %s", mount->mountsource);

    if (initialized==0) {

	init_networkutils();

	initialized=1;

    }

    pthread_mutex_lock(&mountbackend_list_mutex);

    notifyfs_server=lookup_notifyfsserver_permount(mount->index);

    pthread_mutex_unlock(&mountbackend_list_mutex);

    return notifyfs_server;

}

void unset_mount_backend(struct notifyfs_mount_struct *mount)
{

    pthread_mutex_lock(&mountbackend_list_mutex);

    mount_serverbackend[mount->index]=NULL;

    pthread_mutex_unlock(&mountbackend_list_mutex);

}



void set_mount_backend(struct notifyfs_mount_struct *mount)
{
    struct notifyfs_server_struct *notifyfs_server=NULL;

    logoutput("set_mount_backend: testing on %s", mount->mountsource);

    if (initialized==0) {

	init_networkutils();

	initialized=1;

    }

    pthread_mutex_lock(&mountbackend_list_mutex);

    notifyfs_server=lookup_notifyfsserver_permount(mount->index);

    if (notifyfs_server) {

	logoutput("set_mount_backend: backend already set on %s", mount->mountsource);

    } else {

	if (is_fusefs(mount->filesystem)==1) {

	    /* lookup for a connection for this fuse fs 
		normally a fuse fs has to setup a connection first before mounting */

	    logoutput("set_mount_backend: %s is a fuse filesystem, ignoring for now", mount->filesystem);

	}

	if ( ! notifyfs_server && is_networkfs(mount->filesystem)==1) {
	    struct remotehost_info_struct remotehost_info;
	    int len=strlen(mount->superoptions), res;
	    char host[len];
	    char remote_path[len];
	    char user[len];

	    /* lookup server using ipv4 address */

	    remotehost_info.host=host;
	    remotehost_info.len_host=len;

	    remotehost_info.path=remote_path;
	    remotehost_info.len_path=len;

	    remotehost_info.user=user;
	    remotehost_info.len_user=len;

	    remotehost_info.ipv4address=NULL;

	    res=determine_remotehost(mount->filesystem, mount->mountsource, mount->superoptions, &remotehost_info);

	    if (res>=0) {

		/* translation from mountinfo to host, path and user no error */

		if (remotehost_info.ipv4address) {

		    logoutput("set_mount_backend: address %s found, check for backend", remotehost_info.ipv4address);

		    pthread_mutex_lock(&servers_list_mutex);

		    notifyfs_server=lookup_notifyfsserver_peripv4(remotehost_info.ipv4address);

		    pthread_mutex_unlock(&servers_list_mutex);

		    if ( notifyfs_server ) {

			logoutput("set_mount_backend: backend %s found", remotehost_info.ipv4address);

    			mount_serverbackend[mount->index]=notifyfs_server;

			if ( ! notifyfs_server->data) {

			    notifyfs_server->data=(void *) remotehost_info.ipv4address;
			    remotehost_info.ipv4address=NULL;

			}

		    }

		    if (remotehost_info.ipv4address) {

			free(remotehost_info.ipv4address);
			remotehost_info.ipv4address=NULL;

		    }

		} else {

		    logoutput("set_networkfs_backend: no ip4address found ... ");

		}

	    }

	}

    }

    unlock:

    pthread_mutex_unlock(&mountbackend_list_mutex);

}

void connect_remote_notifyfs_server(char *ipv4address)
{
    struct notifyfs_server_struct *notifyfs_server=NULL;

    logoutput("connect_remote_notifyfs_server: connect to %s", ipv4address);

    if (initialized==0) {

	init_networkutils();

	initialized=1;

    }

    /* get server from list */

    notifyfs_server=create_notifyfs_server();

    if ( notifyfs_server) {
	struct notifyfs_connection_struct *connection=NULL;
	int res;

	init_notifyfs_server(notifyfs_server);

	notifyfs_server->status=NOTIFYFS_SERVERSTATUS_DOWN;
	notifyfs_server->type=NOTIFYFS_SERVERTYPE_NETWORK;

	connection=malloc(sizeof(struct notifyfs_connection_struct));

	if (connection) {

	    notifyfs_server->connection=connection;

	    /* here try to connect to the remote notifyfs server 
		what callback here, it's the receiving of the fsevent messages coming from the remote server
	    */

	    res=create_inet_clientsocket(ipv4address, notifyfs_options.networkport, connection, NULL, process_message_from_backend);

	    if (res<0) {

		notifyfs_server->status=NOTIFYFS_SERVERSTATUS_ERROR;
		notifyfs_server->error=abs(res);

		logoutput("connect_remote_notifyfs_server: error %i when connecting to %s", notifyfs_server->error, ipv4address);

		notifyfs_server->connection=NULL;
		free(connection);
		connection=NULL;

	    } else if (res>0) {

		notifyfs_server->status=NOTIFYFS_SERVERSTATUS_UP;

		logoutput("connect_remote_notifyfs_server: succesfull connected to %s", ipv4address);

	    }

	    notifyfs_server->data=(void *) strdup(ipv4address);

	} else {

	    notifyfs_server->status=NOTIFYFS_SERVERSTATUS_ERROR;
	    notifyfs_server->error=ENOMEM;

	}

    }

}

static unsigned char isvalid_ipv4(char *address)
{
    struct in_addr tmp_addr;

    if (inet_aton(address, &tmp_addr)==0) return 0;

    return 1;

}


void read_remote_servers(char *path)
{
    FILE *fp;
    char line[512], *sep;

    fp=fopen(path, "r");

    if  ( !fp) {

	logoutput("read_remote_servers_file: error %i opening the file", errno);
	return;

    }

    while( ! feof(fp)) {

	if ( ! fgets(line, 512, fp)) continue;

	sep=strchr(line, '\n');
	if (sep) *sep='\0';

	convert_to(line, UTILS_CONVERT_SKIPSPACE);

	if (isvalid_ipv4(line)==1) {
	    struct notifyfs_server_struct *notifyfs_server;

	    notifyfs_server=lookup_notifyfsserver_peripv4(line);

	    if (! notifyfs_server) connect_remote_notifyfs_server(line);

	}

    }

    fclose(fp);

}

static void determine_remotepath_nfs(char *source, char *path, char *notifyfs_url, int len)
{
    char *sep=strchr(source, ':');

    if (sep) {
	int len0=strlen(sep+1);
	int len1=strlen(path);

	/* does it fit ? */

	if (strlen("nfs:") + len0 + len1 < len) {
	    int pos=0;

	    pos+=snprintf(notifyfs_url, len, "nfs:");

	    memcpy(notifyfs_url+pos, sep+1, len0);
	    pos+=len0;

	    memcpy(notifyfs_url+pos, path, len1);
	    pos+=len1;

	    *(notifyfs_url+pos)='\0';

	}

    }

}

/* cifs source is of format:

    //netbioshost/share

*/

static void determine_remotepath_cifs(char *source, char *path, char *notifyfs_url, int len)
{
    char *sep=strchr(source, '/');

    /* test the source is in //host/share format */

    if (sep) {
	int len0;
	int len1;

	if (*(sep+1)=='/') sep++;

	len0=strlen(sep);
	len1=strlen(path);

	if (strlen("cifs:") + len0 + len1 < len) {

	    int pos=0;

	    pos+=snprintf(notifyfs_url, len, "cifs:");

	    memcpy(notifyfs_url+pos, sep+1, len0);
	    pos+=len0;

	    memcpy(notifyfs_url+pos, path, len1);
	    pos+=len1;

	    *(notifyfs_url+pos)='\0';

	}

    } else {

	sep=strchr(source, '\\');

	if (sep) {
	    int len0;
	    int len1;

	    if (*(sep+1)=='\\') sep++;

	    len0=strlen(sep);
	    len1=strlen(path);

	    if (strlen("cifs:") + len0 + len1 < len) {
		int pos=0;

		pos+=snprintf(notifyfs_url, len, "cifs:");

		memcpy(notifyfs_url+pos, sep+1, len0);
		pos+=len0;

		memcpy(notifyfs_url+pos, path, len1);
		pos+=len1;

		*(notifyfs_url+pos)='\0';

		/* here replace the backslashes with normal slashes */

		sep=notifyfs_url;

		while(1) {

		    sep=strchr(sep, '\\');

		    if (!sep) {

			break;

		    } else {

			*sep='/';
			sep++;

		    }

		}

	    }

	}

    }

}

static void determine_remotepath_sshfs(char *source, char *options, char *path, char *notifyfs_url, int len)
{
    char *sep1=NULL;
    int pos=0, len1;
    char user[64];

    memset(user, '\0', 64);

    sep1=strchr(source, '@');

    if (sep1) {

	if (sep1-source<len) {

	    memcpy(user, source, sep1-source);

	}

    }

    if (strlen(user)==0) {

	/* no user part yet: get it from he mountoptions */

	get_value_from_options(options, "user_id", user, 64);

	if (strlen(user)>0) {
	    uid_t uidnr=atoi(user);
	    struct passwd *pwd;

	    memset(user, '\0', 64);

	    pwd=getpwuid(uidnr);

	    if (pwd) strncpy(user, pwd->pw_name, 64);

	}

    }

    if (strlen(user)>0) {

	pos=snprintf(notifyfs_url, len, "sshfs:%s@", user);

    } else {

	pos=snprintf(notifyfs_url, len, "sshfs:");

    }

    sep1=strchr(source, ':');

    /* look for the starting path in source */

    if (sep1) {
	int len0=strlen(sep1+1);

	if (len0>0) {

	    if (pos+len0<len) {

		memcpy(notifyfs_url+pos, sep1+1, len0);
		pos+=len0;

	    }

	} else {

	    /* no path in source: with ssh the home is used of the user */

	    if (strlen(user)>0) {

		/* use a template */

		len0=strlen("%HOME%");

		if (pos+len0<len) {

		    memcpy(notifyfs_url+pos, "%HOME%", len0);
		    pos+=len0;

		}

	    } else {

		/* if no user found .... what to do ?? */

		logoutput("determine_remotepath_sshfs: no user and no starting path found....");

	    }

	}

    }

    len1=strlen(path);

    if (pos+len1<len) {

	memcpy(notifyfs_url+pos, path, len1);
	pos+=len1;

	*(notifyfs_url+pos)='\0';

    }

}

void determine_remotepath(struct notifyfs_mount_struct *mount, char *path, char *notifyfs_url, int len)
{

    if (strcmp(mount->filesystem, "cifs")==0) {

	determine_remotepath_cifs(mount->mountsource, path, notifyfs_url, len);

    } else if (strcmp(mount->filesystem, "nfs")==0) {

	determine_remotepath_nfs(mount->mountsource, path, notifyfs_url, len);

    } else if (strcmp(mount->filesystem, "fuse.sshfs")==0) {

	determine_remotepath_sshfs(mount->mountsource, mount->superoptions, path, notifyfs_url, len);

    }

    logoutput("determine_remotepath: notifyfs url %s", notifyfs_url);

}
