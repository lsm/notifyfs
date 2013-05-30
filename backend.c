/*
 
  2010, 2011, 2012, 2013 Stef Bon <stefbon@gmail.com>

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
#include <linux/netdevice.h>
#include <pwd.h>

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

#include "logging.h"
#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"
#include "path-resolution.h"
#include "epoll-utils.h"
#include "socket.h"
#include "networkutils.h"
#include "utils.h"
#include "options.h"
#include "filesystem.h"
#include "entry-management.h"
#include "backend.h"
#include "client.h"

extern struct notifyfs_options_struct notifyfs_options;
extern int process_connection_event(struct notifyfs_connection_struct *connection, uint32_t events);

/* to maintain a list */
static pthread_mutex_t backends_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct notifyfs_backend_struct *backends=NULL;

/* for backends which are local */
static struct notifyfs_backend_struct local_backend;


struct notifyfs_backend_struct *get_local_backend()
{
    return &local_backend;
}

void add_backend_to_list_unlocked(struct notifyfs_backend_struct *backend)
{
    /* add to list */

    if (backends) backends->prev=backend;
    backend->next=backends;
    backends=backend;

}

void add_backend_to_list(struct notifyfs_backend_struct *backend, unsigned char locked)
{

    if (locked==0) pthread_mutex_lock(&backends_mutex);
    add_backend_to_list_unlocked(backend);
    if (locked==0) pthread_mutex_unlock(&backends_mutex);

}

void lock_backends()
{
    pthread_mutex_lock(&backends_mutex);
}

void unlock_backends()
{
    pthread_mutex_unlock(&backends_mutex);
}

struct notifyfs_backend_struct *get_next_backend(struct notifyfs_backend_struct *backend)
{
    if (backend) return backend->next;
    return backends;
}


struct notifyfs_backend_struct *create_notifyfs_backend()
{
    struct notifyfs_backend_struct *notifyfs_backend=NULL;

    notifyfs_backend=malloc(sizeof(struct notifyfs_backend_struct));

    if (notifyfs_backend) {

	notifyfs_backend->type=0;
	notifyfs_backend->status=NOTIFYFS_BACKENDSTATUS_NOTSET;

	notifyfs_backend->buffer=NULL;
	notifyfs_backend->lenbuffer=0;
	notifyfs_backend->connection=NULL;

	notifyfs_backend->error=0;
	notifyfs_backend->connect_time.tv_sec=0;
	notifyfs_backend->connect_time.tv_nsec=0;

	pthread_mutex_init(&notifyfs_backend->mutex, NULL);

	notifyfs_backend->next=NULL;
	notifyfs_backend->prev=NULL;

	notifyfs_backend->data=NULL;
	notifyfs_backend->refcount=0;

    }

    return notifyfs_backend;

}

void init_local_backend()
{

    local_backend.type=NOTIFYFS_BACKENDTYPE_LOCALHOST;
    local_backend.status=NOTIFYFS_BACKENDSTATUS_UP;

    local_backend.buffer=NULL;
    local_backend.lenbuffer=0;
    local_backend.connection=NULL;

    local_backend.error=0;
    local_backend.connect_time.tv_sec=0;
    local_backend.connect_time.tv_nsec=0;

    pthread_mutex_init(&local_backend.mutex, NULL);

    local_backend.data=NULL;
    local_backend.refcount=0;

    local_backend.next=NULL;
    local_backend.prev=NULL;

    add_backend_to_list(&local_backend, 0);

}

void change_status_backend(struct notifyfs_backend_struct *backend, unsigned char status)
{

    pthread_mutex_lock(&backend->mutex);
    backend->status=status;
    pthread_mutex_unlock(&backend->mutex);

}


/* function to lookup existing servers using the domain/ipaddress */

static struct notifyfs_backend_struct *lookup_notifyfs_server_byhost(char *host)
{
    struct notifyfs_backend_struct *backend=NULL;
    int res;

    /*
	compare with existing connections/backends
	(do this by looking at the hostname, nu ip address)
    */

    backend=get_next_backend(NULL);

    while(backend) {

	if (backend->type==NOTIFYFS_BACKENDTYPE_SERVER) {
	    struct notifyfs_connection_struct *connection=backend->connection;

	    if (connection) {

		if (is_ipv4(connection)==1) {
		    char hostname[1024];

		    if (getnameinfo((struct sockaddr *) &connection->socket.inet, sizeof(struct sockaddr_in), hostname, 1024, NULL, 0, 0)==0) {

			if (strcmp(hostname, host)==0) break;

		    }

		} else if (is_ipv6(connection)==1) {
		    char hostname[1024];

		    if (getnameinfo((struct sockaddr *) &connection->socket.inet6, sizeof(struct sockaddr_in6), hostname, 1024, NULL, 0, 0)==0) {

			if (strcmp(hostname, host)==0) break;

		    }

		}

	    }

	}

	backend=get_next_backend(backend);

    }

    return backend;

}

static struct notifyfs_backend_struct *lookup_notifyfs_server_byip(int family, char *ipaddress)
{
    struct notifyfs_backend_struct *backend=NULL;
    int res;

    /*
	compare with existing connections/backends
	(do this by looking at the hostname, nu ip address)
    */

    backend=get_next_backend(NULL);

    while(backend) {

	if (backend->type==NOTIFYFS_BACKENDTYPE_SERVER) {
	    struct notifyfs_connection_struct *connection=backend->connection;

	    if (connection) {

		if (family==AF_INET && is_ipv4(connection)==1) {
		    char ipv4[INET_ADDRSTRLEN];

		    if (inet_ntop(family, &connection->socket.inet.sin_addr, ipv4, INET_ADDRSTRLEN)) {

			if (strcmp(ipaddress, ipv4)==0) break;

		    }

		} else if (family==AF_INET6 && is_ipv6(connection)==1) {
		    char ipv6[INET6_ADDRSTRLEN];

		    if (inet_ntop(family, &connection->socket.inet6.sin6_addr, ipv6, INET6_ADDRSTRLEN)) {

			if (strcmp(ipaddress, ipv6)==0) break;

		    }

		}

	    }

	}

	backend=get_next_backend(backend);

    }

    return backend;

}



/* set the backend when a mount shows up

   right now it does check the mount is a network fs, (nfs, cifs or sshfs) and on the remote server another notifyfs server is present

*/

void set_supermount_backend(struct supermount_struct *supermount, struct notifyfs_mount_struct *mount, char *mountpoint)
{
    struct notifyfs_filesystem_struct *fs=NULL;

    if (supermount->backend) {

	logoutput("set_supermount_backend: backend already set");
	return;

    }

    fs=supermount->fs;

    if ( ! fs) {

	logoutput("set_supermount_backend: filesystem is not set");
	return;

    }

    logoutput("set_supermount_backend: fs %s", fs->filesystem);

    if (! (fs->mode & NOTIFYFS_FILESYSTEM_KERNEL)) {
	struct notifyfs_backend_struct *backend=NULL;
	struct client_struct *client=NULL;

	/*
	    look for a connection for this non kernel fs
	    normally a fuse fs has to setup a connection first before mounting
	    do this by comparing the mountpoint by the mountpath the fuse backend has send

	*/

	logoutput("set_supermount_backend: %s is a non kernel filesystem, look for backend, lookup client", fs->filesystem);

	lock_clientslist();

	client=get_next_client(NULL);

	while(client) {

	    if (client->type==NOTIFYFS_CLIENTTYPE_FUSEFS && client->data) {
		char *path=(char *) client->data;

		if (path && strcmp(path, mountpoint)==0) {

		    logoutput("set_supermount_backend: found client fs with mountpoint %s", path);

		    backend=create_notifyfs_backend();

		    if (backend) {

			backend->type=NOTIFYFS_BACKENDTYPE_FUSEFS;
			backend->status=NOTIFYFS_BACKENDSTATUS_UP;

			backend->buffer=client->buffer;
			backend->lenbuffer=client->lenbuffer;
			backend->connection=client->connection;

			if (backend->connection) {

			    backend->connection->data=(void *) backend;
			    backend->connection->typedata=NOTIFYFS_OWNERTYPE_BACKEND;

			}

			backend->data=NULL;
			backend->refcount=1;

			add_backend_to_list(backend, 0);

    			supermount->backend=backend;

			client->buffer=NULL;
			client->lenbuffer=0;
			client->connection=NULL;

			remove_client_from_list(client);

			pthread_mutex_destroy(&client->mutex);

			free(client);

			client=NULL;

			add_backend_to_list(backend, 0);
			break;

		    }

		}

	    }

	    client=get_next_client(client);

	}

	unlock_clientslist();

    }

    if (supermount->backend) return;

    if (fs->mode & NOTIFYFS_FILESYSTEM_KERNEL) {

	/*
	    here try a netlink connection
	    the big issue is here of course howto determine it's a network filesystem
	*/

	logoutput("set_supermount_backend: try netlink (TODO)");

    }

    if (fs->fsfunctions) {

	if (fs->fsfunctions->get_remotehost) {
	    char host[1024];
	    unsigned char islocal=0;
	    int res;

	    memset(host, '\0', 1024);

	    res=(*fs->fsfunctions->get_remotehost) (supermount->source, supermount->options, host, 1024, &islocal);

	    if (res>=0 && strlen(host)>0) {

		if (islocal==0) {
		    struct notifyfs_backend_struct *backend=NULL;

		    logoutput("set_supermount_backend: address %s found, is remote, test remote server is connected", host);

		    pthread_mutex_lock(&backends_mutex);

		    if (isvalid_ipv4(host)==1) {

			backend=lookup_notifyfs_server_byip(AF_INET, host);

		    } else if (isvalid_ipv6(host)==1) {

			backend=lookup_notifyfs_server_byip(AF_INET6, host);

		    } else {

			backend=lookup_notifyfs_server_byhost(host);

		    }

		    pthread_mutex_unlock(&backends_mutex);

		    if (backend) {

			logoutput("set_supermount_backend: backend %s found for %s", host, supermount->source);

    			supermount->backend=backend;
			backend->refcount++;

		    }

		} else if (islocal==1) {

		    /* here: connection is local... so the backend is the host self...
			take the local_backend
		    */

		    logoutput("set_supermount_backend: take the localhost for %s", supermount->source);

    		    supermount->backend=&local_backend;
		    local_backend.refcount++;

		}

	    } else {

		logoutput("set_supermount_backend: no address found for %s", supermount->source);

	    }

	}

    }

}

struct notifyfs_backend_struct *connect_remote_notifyfs_server(int family, char *ipaddress)
{
    struct notifyfs_backend_struct *backend=NULL;
    struct notifyfs_connection_struct *connection=NULL;
    int res;

    logoutput("connect_remote_notifyfs_server: connect to %s", ipaddress);

    connection=malloc(sizeof(struct notifyfs_connection_struct));

    if (connection) {

	/* here try to connect to the remote notifyfs server 
	    what callback here, it's the receiving of the fsevent messages coming from the remote server
	*/

	res=create_inet_clientsocket(family, ipaddress, notifyfs_options.networkport, connection, NULL, process_connection_event);

	if (res<0) {

	    logoutput("connect_remote_notifyfs_server: error %i when connecting to %s", errno, ipaddress);

	    free(connection);
	    connection=NULL;

	} else if (res>=0) {


	    logoutput("connect_remote_notifyfs_server: succesfull connected to %s", ipaddress);

	    backend=create_notifyfs_backend();

	    if ( backend) {

		backend->type=NOTIFYFS_BACKENDTYPE_SERVER;
		backend->connection=connection;

		connection->typedata=NOTIFYFS_OWNERTYPE_SERVER;
		connection->data=(void *) backend;

		backend->status=NOTIFYFS_BACKENDSTATUS_UP;
		backend->data=NULL;

	    } else {

		if (connection->fd>0) {

		    close(connection->fd);
		    connection->fd=0;

		}

		free(connection);
		connection=NULL;

	    }

	}

    }

    return backend;

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
	    struct notifyfs_backend_struct *backend;

	    pthread_mutex_lock(&backends_mutex);

	    backend=lookup_notifyfs_server_byip(AF_INET, line);

	    if (! backend) {

		backend=connect_remote_notifyfs_server(AF_INET, line);
		if (backend) add_backend_to_list(backend, 1);

	    }

	    pthread_mutex_unlock(&backends_mutex);

	} else if (isvalid_ipv6(line)==1) {
	    struct notifyfs_backend_struct *backend;

	    pthread_mutex_lock(&backends_mutex);

	    backend=lookup_notifyfs_server_byip(AF_INET6, line);

	    if (! backend) {

		backend=connect_remote_notifyfs_server(AF_INET6, line);
		if (backend) add_backend_to_list(backend, 1);

	    }

	    pthread_mutex_unlock(&backends_mutex);

	}


    }

    fclose(fp);

}

char *process_notifyfsurl(char *url)
{
    char *sep=NULL;
    char *path=NULL;

    /*
	find the first part, which is the name of the service, like:

	smb://
	nfs://
	ssh://

    */

    sep=strchr(url, ':');

    if (sep) {
	struct fsfunctions_struct *fsfunctions=NULL;

	/* seperate the service name from the url */

	*sep='\0';

	/* skip the starting slashes */

	sep++;
	while(*sep=='/') sep++;

	/* lookup the fs functions matching the service */

	fsfunctions=lookup_fsfunctions_byservice(url);

	if (fsfunctions) {

	    /* if fs functions found: call the process_url for these functions with the stripped url */

	    if (fsfunctions->process_url) {

		path=(*fsfunctions->process_url) (sep);

	    }

	}

    }

    return path;

}

