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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>

#include <pthread.h>
#include <pwd.h>
#include <fcntl.h>

#include <arpa/inet.h>

#include "logging.h"
#include "utils.h"
#include "filesystem.h"
#include "filesystem-smb.h"
#include "networkutils.h"

/*
    functions to translate an url like smb://server/share/path into a local path
    since they depend on the system (is samba used??) this depends on the os

*/

#ifdef __gnu_linux__
#include "filesystem-smb-linux.c"
#else

static inline char *translateSMBshareintopath(char *user, char *share)
{
    return NULL;
}

static inline int getSMBremotehost(char *source, char *options, char *host, int len, unsigned char *islocal)
{
    return 0;
}

static inline void constructSMBurl(char *source, char *options, char *path, char *url, int len)
{
    memset(url, '\0', len);
}

static inline unsigned char apply_smb(char *fs, unsigned char kernelfs)
{
    return 0;
}

#endif



/*
    function to process the url it gets from a remote notifyfs server

    it's like :

    smb://user@netbiosname/share/some/dir

    extract the user, share and some/dir and use these to construct the path on this host

*/

char *processSMBurl(char *url)
{
    char *sep;
    char *user=NULL;
    char *share=NULL;
    char *sharepath=NULL;
    char *path=NULL;

    logoutput("process_url_smb: process %s", url);

    /* skip starting slashes */

    while(*url=='/') url++;

    sep=strchr(url, '@');

    if (sep) {

	*sep='\0';
	sep++;
	user=url;
	url=sep;

    }

    sep=strchr(url, '/');

    if (!sep) {

	logoutput("process_url_smb: no server found, cannot continue");
	goto out;

    }

    ///* skip the servername */

    sep++;
    url=sep;

    //sep=strchr(url, '/');

    //if (!sep) {

	//logoutput("process_url_smb: no share found, cannot continue");
	//goto out;

    //}

    /* the share */

    share=url;

    sep=strchr(share, '/');

    if (sep) {

	*sep='\0';
	sep++; /* the rest is the path in the share */

    }

    sharepath=translateSMBshareintopath(user, share);

    if (sharepath) {
	int len0=0;

	logoutput("process_notifyfsurl_smb: path %s found, test it does exist", sharepath);

	if (sep) len0=strlen(sep);

	if (len0>0) {
	    int len1=strlen(sharepath);

	    path=malloc(len1 + 2 + len0);

	    if (path) {

		memcpy(path, sharepath, len1);
		*(path+len1)='/';
		memcpy(path+len1+1, sep, len0);
		*(path+len1+1+len0)='\0';

	    }

	    free(sharepath);

	} else {

	    path=sharepath;

	}

    }

    out:

    return path;

}

/* translate a path on a service on the local service to the real path */

char *getSMBlocalpath(char *options, char *source, char *relpath)
{
    char *path=NULL;
    char *sep;
    char *sharepath=NULL;
    char user[64];

    memset(user, '\0', 64);

    /*
	get username from the mountoptions
    */

    get_value_mountoptions(options, "username", user, 64);

    /*
	replace every backslash 

    */

    sep=source;

    while(sep) {

	sep=strchr(sep, '\\');

	if (sep) *sep='/';

    }

    /*
	skip starting slashes
    */

    sep=source;
    while(*sep=='/') sep++;


    /*
	look for the first slash: this where the share starts
    */

    sep=strchr(sep, '/');

    if (sep) {
	sep++;

	/*
	    translate a SMB share (served by this computer) into a local path
	*/

	sharepath=translateSMBshareintopath(user, sep);

	if (sharepath) {
	    int len0=strlen(relpath);

	    if (len0>0) {
		int len1=strlen(sharepath);

		/* relative path to share is defined */

		path=malloc(len1 + 2 + len0);

		if (path) {

		    memcpy(path, sharepath, len1);
		    *(path+len1)='/';
		    memcpy(path+len1+1, relpath, len0);
		    *(path+len1+1+len0)='\0';

		}

		free(sharepath);

	    } else {

		path=sharepath;

	    }

	}

    }


    return path;

}

static struct fsfunctions_struct smb_functions={
		    .name		= "smb",
		    .apply		= apply_smb,
		    .get_remotehost	= getSMBremotehost,
		    .construct_url	= constructSMBurl,
		    .process_url	= processSMBurl,
		    .get_localpath	= getSMBlocalpath,
		    .next		= NULL
};

void register_smb_functions()
{
    add_fsfunctions(&smb_functions);

}
