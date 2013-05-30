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
#include <netdb.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <linux/netdevice.h>
#include <ifaddrs.h>

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

typedef char pathstring[PATH_MAX+1];

#include "logging.h"
#include "epoll-utils.h"
#include "socket.h"
#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"
#include "networkutils.h"
#include "utils.h"
#include "options.h"

unsigned char isvalid_ipv4(char *address)
{
    struct in_addr tmp_addr;

    if (inet_pton(AF_INET, address, &tmp_addr)==1) return 1;

    return 0;

}

unsigned char isvalid_ipv6(char *address)
{
    struct in6_addr tmp_addr;

    if (inet_pton(AF_INET6, address, &tmp_addr)==1) return 1;

    return 0;

}



unsigned char address_islocal(int family, struct sockaddr *address)
{
    struct ifaddrs *ifa_list=NULL;
    unsigned char islocal=0;

    if (getifaddrs(&ifa_list)==0) {
	struct ifaddrs *ifa;
	int len0=(family==AF_INET) ? INET_ADDRSTRLEN : INET6_ADDRSTRLEN;
	char ipaddress[len0];

	if (family==AF_INET) {
	    struct sockaddr_in *s_in = (struct sockaddr_in *) address;

	    if (!inet_ntop(family, &s_in->sin_addr, ipaddress, len0)) {

		logoutput("addr_islocal: error (%i) %s", errno, strerror(errno));
		goto out;

	    }

	} else if (family==AF_INET6) {
	    struct sockaddr_in6 *s_in6 = (struct sockaddr_in6 *) address;

	    if (!inet_ntop(family, &s_in6->sin6_addr, ipaddress, len0)) {

		logoutput("addr_islocal: error (%i) %s", errno, strerror(errno));
		goto out;

	    }

	} else {

	    goto out;

	}

	ifa=ifa_list;

	while(ifa) {

	    if (ifa->ifa_addr) {

		if (ifa->ifa_addr->sa_family==family) {

		    if (family==AF_INET) {
			char iplocal[INET_ADDRSTRLEN];
			struct sockaddr_in *s_in = (struct sockaddr_in *) ifa->ifa_addr;

			if (inet_ntop(family, &s_in->sin_addr, iplocal, INET_ADDRSTRLEN)) {

			    if (strcmp(ipaddress, iplocal)==0) {

				islocal=1;
				break;

			    }

			}

		    } else if (family==AF_INET6) {
			char iplocal[INET6_ADDRSTRLEN];
			struct sockaddr_in6 *s_in6 = (struct sockaddr_in6 *) ifa->ifa_addr;

			if (inet_ntop(family, &s_in6->sin6_addr, iplocal, INET6_ADDRSTRLEN)) {

			    if (strcmp(ipaddress, iplocal)==0) {

				islocal=1;
				break;

			    }

			}

		    }

		}

	    }

	    ifa=ifa->ifa_next;

	}

    }

    out:

    if (ifa_list) {

	freeifaddrs(ifa_list);
	ifa_list=NULL;

    }

    return islocal;

}

int get_hostname(char *address, const char *service, char *host, int len, unsigned char *islocal)
{
    struct addrinfo *ai_list, *ai_p, ai_hint;
    int res;

    memset(host, '\0', len);

    ai_hint.ai_family=AF_UNSPEC;
    ai_hint.ai_flags=AI_CANONNAME | AI_NUMERICSERV;
    ai_hint.ai_socktype=0;
    ai_hint.ai_protocol=0;

    ai_hint.ai_canonname=NULL;
    ai_hint.ai_addr=NULL;
    ai_hint.ai_next=NULL;

    res=getaddrinfo(address, service, &ai_hint, &ai_list);

    if (res==0) {

	if (ai_list) {
	    int len0=strlen(ai_list->ai_canonname);

	    if (len0<=len) {

		res=len0;
		memcpy(host, ai_list->ai_canonname, res);

		/* here test ai_list->ai_addr is local */

		*islocal=address_islocal(ai_list->ai_family, ai_list->ai_addr);

	    } else {

		res=-E2BIG;

	    }

	} else {

	    res=-ENOENT;

	}

    } else {

	res=-abs(res);

    }

    return res;

}



/*
    function to get the value of field from the options of the mountinfo 
*/

int get_value_mountoptions(char *options, const char *option, char *value, int len)
{
    char *poption;
    int nreturn=0;

    if ( ! options || ! option ) {

	nreturn=-EINVAL;
	goto out;

    }

    poption=strstr(options, option);

    if ( poption ) {
	char *endoption=strchrnul(poption, ',');
	char *issign=strchr(poption, '=');

	if ( issign ) {

	    if ( issign<endoption && endoption-issign < len ) {

		nreturn=endoption-issign-1;
		memcpy(value, issign+1, nreturn);

	    } else {

		nreturn=-E2BIG;

	    }

	} else {

	    nreturn=-ENOENT;

	}

    } else {

	nreturn=-ENOENT;

    }

    out:

    return nreturn;

}


/* function which compares a new connection on the network with existing connections 

    if these are the same, return that    
    this is usefull to deny multiple connections from one host

*/

struct notifyfs_connection_struct *compare_notifyfs_connections(struct notifyfs_connection_struct *new_connection)
{
    struct notifyfs_connection_struct *connection=NULL;
    struct epoll_eventloop_struct *eventloop=new_connection->xdata_socket.eventloop;
    struct epoll_extended_data_struct *epoll_xdata=NULL;
    char remotehost[1024];

    if (is_remote(new_connection)==0) goto out;

    memset(remotehost, '\0', 1024);

    /*  compare the connections using the hostname
	first get the name of the remote host
	I hope this works and the name of the remote host is an unique identifier
    */

    if (is_ipv4(new_connection)==1) {
	struct sockaddr_in sock0;
	socklen_t len0=sizeof(struct sockaddr_in);

	if (getpeername(new_connection->fd, (struct sockaddr *) &sock0, &len0)==0) {

	    if (getnameinfo((struct sockaddr *) &sock0, len0, remotehost, 1024, NULL, 0, 0)!=0) goto out;

	}

    } else if (is_ipv6(new_connection)==1) {
	struct sockaddr_in6 sock0;
	socklen_t len0=sizeof(struct sockaddr_in6);

	if (getpeername(new_connection->fd, (struct sockaddr *) &sock0, &len0)==0) {

	    if (getnameinfo((struct sockaddr *) &sock0, len0, remotehost, 1024, NULL, 0, 0)!=0) goto out;

	}

    }

    if (strlen(remotehost)==0) goto out;

    lock_eventloop(eventloop);

    epoll_xdata=get_next_xdata(eventloop, NULL);

    while(epoll_xdata) {

	connection=(struct notifyfs_connection_struct *) epoll_xdata->data;

	if (connection) {

	    if (is_ipv4(connection)==1) {
		char host[1024];

		if (getnameinfo((struct sockaddr *) &connection->socket.inet, sizeof(struct sockaddr_in), host, 1024, NULL, 0, 0)==0) {

		    if (strcmp(remotehost, host)==0) break;

		}

	    } else if (is_ipv6(connection)==1) {
		char host[1024];

		if (getnameinfo((struct sockaddr *) &connection->socket.inet6, sizeof(struct sockaddr_in6), host, 1024, NULL, 0, 0)==0) {

		    if (strcmp(remotehost, host)==0) break;

		}

	    }

	}

	epoll_xdata=get_next_xdata(eventloop, epoll_xdata);

    }

    unlock_eventloop(eventloop);

    out:

    return connection;

}
