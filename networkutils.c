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
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#define LOG_LOGAREA LOG_LOGAREA_SOCKET

#include "logging.h"
#include "networkutils.h"

#define LISTEN_BACKLOG 50

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
