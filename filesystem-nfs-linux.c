/*
    function to extract the backend and the username used to connect from the mountsource and options for the nfs filesystem

*/

static int getNFSremotehost(char *source, char *options, char *host, int len, unsigned char *islocal)
{
    char *separator=strrchr(source, ':');
    int nreturn=0;

    /* remote host is in superoptions */

    /* nfs is very simple:
       the mountsource is like server:/path/to/share

	what to do with user?? assume transparant id handling
	actually user is not required here

    */

    if (separator) {
	int len1;

	/* host part */

	len1=separator-source;

	if (len1 <= len) {
	    char port[16];
	    int len0=strlen(source);
	    char server[len1+1];

	    memset(server, '\0', len1+1);
	    memset(port, '\0', 16);
	    memcpy(server, source, len1);

	    /*
		get the portnumber from the option port in the options
		(this works for all nfs versions ??)
	    */

	    if (get_value_mountoptions(options, "port", port, 16)>0) {

		nreturn=get_hostname(server, port, host, len, islocal);

	    } else {

		nreturn=get_hostname(server, "2049", host, len, islocal);

	    }

	} else {

	    nreturn=-E2BIG;

	}

    } else {

	nreturn=-ENOENT;

    }

    return nreturn;

}

/* nfs source is of format:

    remoteserver:/dir/ec/to/ry

*/

static void constructNFSurl(char *source, char *options, char *path, char *url, int len)
{
    char *sep=strchr(source, ':');

    if (sep) {
	int len0=strlen(sep+1);
	int len1=strlen(path);

	/* does it fit ? */

	if (strlen("nfs://") + len0 + len1 < len) {
	    int pos=0;

	    pos+=snprintf(url, len, "nfs://");

	    memcpy(url+pos, sep+1, len0);
	    pos+=len0;

	    memcpy(url+pos, path, len1);
	    pos+=len1;

	    *(url+pos)='\0';

	}

    }

}

static char *processNFSurl(char *url)
{
    char *path=NULL;
    char *sep1;

    sep1=strchr(url, '@');

    if (sep1) {

	/* skip the user part */

	path=strdup(sep1+1);

    } else {

	sep1=url;

	/* skip starting slashes */

	while(*sep1=='/') sep1++;

	path=strdup(sep1);

    }

    return path;

}

/*

    translate a path on a service on the local service to the real path

*/

static char *getNFSlocalpath(char *fs, char *options, char *source, char *relpath)
{
    char *path=NULL;
    char *sep=strchr(source, ':');

    if (sep) {
	int len0=strlen(relpath);

	if (len0>0) {
	    int len1=strlen(sep+1);

	    path=malloc(len1 + 2 + len0);

	    if (path) {

		memcpy(path, sep+1, len1);
		*(path+len1)='/';
		memcpy(path+len1+1, relpath, len0);
		*(path+len1+1+len0)='\0';

	    }

	} else {

	    path=strdup(sep+1);

	}

    }

    return path;

}

static unsigned char apply_nfs(char *fs, unsigned char kernelfs)
{
    if (kernelfs==1) {

	if (strcmp(fs, "nfs")==0 || strcmp(fs, "nfs4")==0) return 1;

    }

    return 0;
}

