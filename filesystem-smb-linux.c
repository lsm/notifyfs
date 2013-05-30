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

/* functions to extract the local path out of the url, with smb on linux this is not so easy 

    the url is like:

    smb://user@netbiosname/share/path/to/object

*/

#define SAMBA_CONF_FILE "/etc/samba/smb.conf"

/*
    function to find the value of option set in share in the smb.conf file
    the contents of this file is read into buffer/size

    so look for value in

    [share]
    ..
    ..
    option=value

    here used when option is "path", and then the value is the path to the share

*/

static int search_argument_smb_conf(char *buffer, size_t size, char *share, char *option, char **value)
{
    char *argument=NULL;
    int nreturn=0;

    logoutput("search_argument_smb_conf: look for %s", share);

    if (buffer) {
	char *pos=NULL, *end;
	int len0=strlen(share);
	char section[len0+3];

	/* construct the section for share [share] */

	section[0]='[';
	memcpy(&section[1], share, len0);
	section[len0+1]=']';
	section[len0+2]='\0';

	pos=strstr(buffer, section);

	if (pos) {

	    while(pos<buffer+size) {

		end=strchr(pos, '\n');

		if (! end || end==buffer+size) {

		    break;

		} else {
		    int len=end-pos;
		    char line[len+1], *sep;

		    memcpy(line, pos, len);
		    *(line+len)='\0';

		    convert_to(line, UTILS_CONVERT_SKIPSPACE);

		    sep=strchr(line, '=');

		    if (sep) {

			*sep='\0';

			if (strcmp(line, option)==0) {

			    *value=strdup(sep+1);

			    if (! *value) {

				nreturn=-ENOMEM;

			    } else {

				nreturn=strlen(sep+1)+1;

			    }

			    break;

			}

		    } else {

			if (*line=='[') {

			    if (strcmp(line, section)!=0) break;

			}

		    }

		}

		pos=end+1;

	    }

	} else {

	    nreturn=-ENOENT;

	}

    }

    return nreturn;

}

/*
    translate a template (as found in smb.conf) to a real path

    it does this by expanding the variables %U, %S and %H

*/


int parse_smb_conf_path(char *path, char *buffer, size_t size, char *user, struct passwd **pw)
{
    char *sep=NULL;
    char *pos0=path, *pos1, *pos2=buffer;
    int len0=strlen(path), len1=0;
    unsigned char conversion=0;

    logoutput("parse_smb_conf_path");

    while(1) {

	pos1=pos0;
	pos0=strchrnul(pos0, '%');

	if (*pos0=='%') {


	    /* possible:
		%U
		%S
		%H
	    */

	    if (*(pos0+1)=='U' || *(pos0+1)=='S') {
		int len3=strlen(user);

		/* add the username */

		if (pos0>pos1) {

		    len1+=pos0-pos1;

		    if (buffer) {

			if (len1<size) {

			    memcpy(pos2, pos1, pos0-pos1);
			    pos2+=pos0-pos1;

			}

		    }

		}

		len1+=len3;
		conversion=1;

		if (buffer) {

		    if (len1<size) {

			memcpy(pos2, user, len3);
			pos2+=len3;

		    }

		}

		pos0+=2;

	    } else if (*(sep+1)=='H') {
		int len3=0;

		if (! *pw) {

		    errno=0;

		    *pw=getpwnam(user);

		    if (! *pw) {

			if (errno>0) logoutput("parse_smb_conf_path: error %i getting properties user %s", errno, user);
			goto out;

		    }

		}

		len3=strlen((*pw)->pw_dir);
		conversion=1;

		/* add the homedirectory */

		if (pos0>pos1) {

		    len1+=pos0-pos1;

		    if (buffer) {

			if (len1<size) {

			    memcpy(pos2, pos1, pos0-pos1);
			    pos2+=pos0-pos1;

			}

		    }

		}

		len1+=len3;

		if (buffer) {

		    if (len1<size) {

			memcpy(pos2, (*pw)->pw_dir, len3);
			pos2+=len3;

		    }

		}

		pos0+=2;

	    } else {

		logoutput("parse_smb_conf_path: %s not reckognized", path);

		pos0++;

	    }

	} else {

	    if (pos0>pos1) {

		len1+=pos0-pos1;

		if (buffer) {

		    if (len1<size) {

			memcpy(pos2, pos1, pos0-pos1);
			pos2+=pos0-pos1;

		    }

		}

	    }

	    *pos1='\0';
	    break;

	}

    }

    out:

    if (conversion==0) len1=0;

    return len1;

}

static char *translateSMBshareintopath(char *user, char *share)
{
    /* here parse the smb.conf file, looking for the share 
	if no share found with the exact this name, try the [homes] section
	if the path parameter is omitted, then take the homedirectory
	if the path parameter is present, parse that, take into account that it can be a template
    */

    int fd=0, res;
    char *path=NULL;
    char *buffer=NULL;
    struct stat st;

    if (user) {

	logoutput("get_samba_share_path: lookup share %s, user %s", share, user);

    } else {

	logoutput("get_samba_share_path: lookup share %s, user not set", share);

    }

    if (stat(SAMBA_CONF_FILE, &st)==-1) {

	logoutput("get_samba_share_path: cannot open %s, error %i", SAMBA_CONF_FILE, errno);
	goto out;

    }

    buffer=malloc(st.st_size+1);

    if (!buffer) {

	logoutput("get_samba_share_path: cannot allocate %i bytes", (int) st.st_size+1);
	goto out;

    }

    memset(buffer, '\0', st.st_size);
    *(buffer+st.st_size)='\n'; /* make sure a newline at the end so every line is terminated by a newline */

    fd=open(SAMBA_CONF_FILE, O_RDONLY);

    if (fd==-1) {

	logoutput("get_samba_share_path: cannot open %s, error %i", SAMBA_CONF_FILE, errno);
	goto out;

    }

    res=read(fd, (void *) buffer, st.st_size);
    close(fd);

    if (res==-1) {

	logoutput("get_samba_share_path: cannot read %s, error %i", SAMBA_CONF_FILE, errno);
	goto out;

    }

    res=search_argument_smb_conf(buffer, st.st_size+1, share, "path", &path);

    if (res>0) {

	logoutput("get_samba_share_path: found %s", path);

    } else if (res==0) {

	/* no share found with this name, test the homes section */

	if (user && strcmp(user, share)==0) {

	    /* share has the same name as the user, so it can be the "[homes]" share
	    look for a [home] section, and test an explicit path declaration (which can be a template) */

	    res=search_argument_smb_conf(buffer, st.st_size+1, "homes", "path", &path);

	    if (res>0) {
		int len0=res;

		/* there is a homes share */

		if (strstr(path, "%S") || strstr(path, "%U") || strstr(path, "%H")) {
		    struct passwd *pw=NULL;
		    int len=0;

		    len=parse_smb_conf_path(path, NULL, 0, user, &pw);

		    if (len>0) {

			if (len>len0) {
			    char *converted_path=NULL;

			    converted_path=malloc(len);

			    if (converted_path) {

				memset(converted_path, '\0', len);
				len=parse_smb_conf_path(path, converted_path, len, user, &pw);

				free(path);
				path=converted_path;

			    }

			} else {
			    char converted_path[len];

			    memset(converted_path, '\0', len);
			    len=parse_smb_conf_path(path, converted_path, len, user, &pw);

			    memset(path, '\0', len0);

			    memcpy(path, converted_path, len);

			}

		    }

		}

	    } else if (res==0) {
		struct passwd *pw=NULL;

		/*
		    path not found in homes section(which is present cause res==0)
		    use the home directory of user (this is the common case)

		*/

		pw=getpwnam(user);

		if (pw) {

		    path=strdup(pw->pw_dir);

		}

	    }

	}

    }

    out:

    if (buffer) {

	free(buffer);
	buffer=NULL;

    }

    return path;

}

/*
    function to extract the backend and the username used to connect 
    from the mountsource and options for the cifs filesystem

    with cifs the address is set in the options
    (this is always in ipv4 format ?)

*/

static int getSMBremotehost(char *source, char *options, char *host, int len, unsigned char *islocal)
{
    int res;
    int len0=(INET_ADDRSTRLEN>INET6_ADDRSTRLEN) ? INET_ADDRSTRLEN : INET6_ADDRSTRLEN;
    char address[len0];

    logoutput("get_remotehost_cifs: get host for source %s and options %s", source, options);

    /* get the address from the mount source */

    res=get_value_mountoptions(options, "addr", address, len0);

    if (res>0) {
	char port[16];

	memset(port, '\0', 16);

	res=get_value_mountoptions(options, "port", port, 16);

	if (res>0) {

	    /* if port explicit in mountoptions take that */

	    res=get_hostname(address, port, host, len, islocal);

	} else {

	    /* no port found in mountoptions: first take the default 445, then 139 */

	    res=get_hostname(address, "445", host, len, islocal);

	    if (res==-ENOENT) {

		res=get_hostname(address, "139", host, len, islocal);

	    }

	}

    }

    return res;

}

/* cifs source is of format:

    //netbioshost/share

*/

static void constructSMBurl(char *source, char *options, char *path, char *url, int len)
{
    char *sep=strchr(source, '/');
    char user[64];

    memset(user, '\0', 64);
    memset(url, '\0', len);

    get_value_mountoptions(options, "username", user, 64);

    /*
	first translate any backslash to a normal slash:

	convert a format like \\netbiosname\share
	to
	//netbiosname/share

    */

    sep=strchr(source, '\\');

    while(sep) {

	*sep='/';
	sep=strchr(sep+1, '\\');

    }

    sep=strchr(source, '/');

    if (sep) {
	int len0;
	int len1;

	/* skip the starting slashes */

	while (*sep=='/') sep++;

	len0=strlen(sep);
	len1=strlen(path);

	if (strlen(user)>0) {

	    if (strlen("smb://") + strlen(user) + 1 + len0 + len1 < len) {
		int pos=0;

		pos=snprintf(url, len, "smb://%s@", user);

		memcpy(url+pos, sep+1, len0);
		pos+=len0;

		memcpy(url+pos, path, len1);
		pos+=len1;

		*(url+pos)='\0';

	    }

	} else {

	    /* no user: using the guest account */

	    if (strlen("smb://") + len0 + len1 < len) {
		int pos=0;

		pos=snprintf(url, len, "smb://");

		memcpy(url+pos, sep+1, len0);
		pos+=len0;

		memcpy(url+pos, path, len1);
		pos+=len1;

		*(url+pos)='\0';

	    }

	}

    }

}

static unsigned char apply_smb(char *fs, unsigned char kernelfs)
{
    if (kernelfs==1) {

	if (strcmp(fs, "cifs")==0) return 1;

    }

    return 0;
}
