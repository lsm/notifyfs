
static unsigned char apply_ssh(char *fs, unsigned char kernelfs)
{
    unsigned char apply=0;

    if (kernelfs==0) {

	if (strcmp(fs, "fuse.sshfs")==0 || strcmp(fs, "fuseblk.sshfs")==0 || strcmp(fs, "sshfs")==0) apply=1;

    }

    return apply;

}

/*
    function to extract the backend and the username used to connect from the mountsource and options for the sshfs filesystem

*/

static int getSSHremotehost(char *source, char *options, char *host, int len, unsigned char *islocal)
{
    char *separator1, *separator2;
    int nreturn=0;

    /* remote host is in mountsource */

    /* sshfs uses the user@xxx.xxx.xxx.xxx:/path format when in ipv4
	user is optional, which defaults to the user making the connection
        path is also optional, it defaults to the home directory of the user on the remote host
    */

    if (! source || ! options) {

	nreturn=-EINVAL;
	goto out;

    }


    /* look for user part */

    separator1=strchr(source, '@');

    if ( separator1 ) {

	/* there is a user part */

	separator1++;

    } else {

	separator1=source;

    }

    /* look for host part (there must be a : )*/

    separator2=strchr(separator1, ':');

    if ( separator2 ) {

	if ( separator2-separator1<=len ) {
	    int len0=separator2-separator1;
	    char address[len0];

	    memset(address, '\0', len0);
	    memcpy(address, separator1, len0);

	    /* what to do when the port is different from the default(=22) */

	    nreturn=get_hostname(address, "22", host, len, islocal);

	} else {

	    nreturn=-E2BIG;

	}

    } else {

	/* error: no ":" separator */

	nreturn=-EINVAL;

    }

    out:

    return nreturn;

}

/* nfs source is of format:

    remoteserver:/dir/ec/to/ry

*/

static void constructSSHurl(char *source, char *options, char *path, char *url, int len)
{
    char *sep1=NULL, *pathstart=NULL;
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

	get_value_mountoptions(options, "user_id", user, 64);

	if (strlen(user)>0) {
	    uid_t uidnr=atoi(user);
	    struct passwd *pwd;

	    memset(user, '\0', 64);

	    pwd=getpwuid(uidnr);

	    if (pwd) strncpy(user, pwd->pw_name, 64);

	}

    }

    if (strlen(user)>0) {

	pos=snprintf(url, len, "sshfs:%s@", user);

    } else {

	pos=snprintf(url, len, "sshfs:");

    }

    sep1=strchr(source, ':');

    /* look for the starting path in source */

    if (sep1) {
	int len0=strlen(sep1+1);

	if (len0>0) {

	    if (pos+len0<len) {

		memcpy(url+pos, sep1+1, len0);
		pathstart=url+pos;
		pos+=len0;

	    }

	} else {

	    /* no path in source: with ssh the home is used of the user */

	    if (strlen(user)>0) {

		/* use a template */

		len0=strlen("%HOME%");

		if (pos+len0<len) {

		    memcpy(url+pos, "%HOME%", len0);
		    pos+=len0;

		}

	    } else {

		/* if no user found .... what to do ?? */

		logoutput("construct_url_sshfs: no user and no starting path found....");

	    }

	}

    }

    len1=strlen(path);

    if (len1>0) {

	if (pos+len1<len) {

	    memcpy(url+pos, path, len1);
	    if (! pathstart) pathstart=url+pos;
	    pos+=len1;

	    *(url+pos)='\0';

	}

    }

    if (pathstart) unslash(pathstart);

}

static char *processSSHurl(char *url)
{
    char *sep1;
    char *user=NULL;
    char *path=NULL;

    sep1=strchr(url, '@');

    if (sep1) {

	/* when there is a @, the first part is a user */

	*sep1='\0';
	user=url;
	sep1++;

    } else {

	sep1=url;

    }

    if (strncmp(sep1, "%HOME%", 6)==0) {

	if (!user) {

	    logoutput("process_url_ssh: HOME set in url, but user not set");
	    goto out;

	} else if (strlen(user)==0) {

	    logoutput("process_url_ssh: HOME set in url, but user is empty");
	    goto out;

	} else if (strlen(user)>0) {
	    struct passwd *pwd;
	    int len0, len1;

	    errno=0;

	    /* TODO: use of central functions to do a lookup of users properties */

	    pwd=getpwnam(user);

	    if ( ! pwd) {

		logoutput("process_url_ssh: user %s not found", user);
		goto out;

	    }

	    len0=strlen(pwd->pw_dir);
	    sep1+=6; /* len of %HOME% */
	    len1=strlen(sep1);

	    if (len1==0) {

		/* extra path empty */

		path=malloc(len0+1);

		if (path) {

		    memcpy(path, pwd->pw_dir, len0);
		    *(path+len0)='\0';

		}

	    } else if (*(sep1)=='/') {

		/* extra path starts with a slash */

		path=malloc(len0+len1+1);

		if (path) {

		    memcpy(path, pwd->pw_dir, len0);
		    memcpy(path+len0, sep1, len1);
		    *(path+len0+len1)='\0';

		}

	    } else {

		/* extra path does not start with a slash */

		path=malloc(len0+len1+2);

		if (path) {

		    memcpy(path, pwd->pw_dir, len0);
		    *(path+len0)='/';
		    memcpy(path+len0+1, sep1, len1);
		    *(path+len0+len1+1)='\0';

		}

	    }

	}

    } else {
	int len1=strlen(sep1);

	if (len1>=6 && strncmp(sep1, "%ROOT%", 6)==0) {

	    sep1+=6;
	    len1=strlen(sep1);

	}

	if (len1==0 || (len1==1 && strcmp(sep1, "/")==0)) {

	    /* path empty -> assume root */

	    path=malloc(2);

	    if (path) {

		*path='/';
		*(path+1)='\0';

	    }

	} else {

	    path=malloc(len1+1);

	    if (path) {

		memcpy(path, sep1, len1);
		*(path+len1)='\0';

	    }

	}

    }

    out:

    return path;

}

/*
    translate a path on a service on the local service to the real path

    with ssh translate the source, which is of the form:

    user@address:/path

    or

    address:/path

    where:
    user is the remote user, maybe omitted, but then the path may not be empty
    if the path is empty, it defaults to the users homedirectory

*/

static char *getSSHlocalpath(char *fs, char *options, char *source, char *relpath)
{
    char *path=NULL;
    char *sep=NULL;

    sep=strchr(source, ':');

    if (sep) {

	if (strlen(sep+1)>0) {

	    /* there is a path */

	    path=strdup(sep+1);
	    goto out;

	}

    }

    if (! path) {

	sep=strchr(source, '@');

	if (sep) {
	    int len=sep-source;
	    char username[len+1];
	    struct passwd *pwd;

	    memcpy(username, source, len);
	    *(username+len)='\0';

	    pwd=getpwnam(username);

	    if (pwd) path=strdup(pwd->pw_dir);

	}

    }

    out:

    return path;

}
