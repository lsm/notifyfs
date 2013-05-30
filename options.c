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

#include <sys/stat.h>
#include <sys/param.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <pwd.h>
#include <grp.h>

#include "logging.h"
#include "notifyfs-fsevent.h"
#include "notifyfs-io.h"
#include "path-resolution.h"
#include "options.h"
#include "utils.h"


extern struct notifyfs_options_struct notifyfs_options;

static void print_usage(const char *progname)
{
	fprintf(stdout, "usage: %s [opts]"
	                "          --socket=FILE\n"
	                "          [--logging=NR,]\n"
	                "          [--logarea=NR,]\n"
	                "          [--accessmode=NR,]\n"
	                "          [--testmode]\n"
	                "          [--enablefusefs=0/1, default 1]\n"
	                "          [--fuseoptions=STRING]\n"
	                "          [--forwardlocal=0/1, default 0]\n"
	                "          [--forwardnetwork=0/1, default 0]\n"
	                "          [--listennetwork=0/1, default 0]\n"
	                "          [--remoteserversfile=FILE, default /etc/notifyfs/servers]\n"
	                "          [--networkport=NR, default 790]\n"
	                "          --mountpoint=PATH\n", progname);

}

static void print_help() {
    unsigned char defaultloglevel=1;

#ifdef LOG_DEFAULT_LEVEL
    defaultloglevel=LOG_DEFAULT_LEVEL;
#endif

    fprintf(stdout, "General options:\n");
    fprintf(stdout, "    --opt                      notifyfs options\n");
    fprintf(stdout, "    -h   --help                print help\n");
    fprintf(stdout, "    -V   --version             print version\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "Notifyfs options:\n");
    fprintf(stdout, "    --socket=FILE              socket where clients can connect to\n");

#ifdef LOGGING
    fprintf(stdout, "    --logging=NUMBER           set loglevel\n");
    fprintf(stdout, " 			            when omitted no logging\n");
    fprintf(stdout, " 			            without number take the default: %i\n", defaultloglevel);
    fprintf(stdout, " 			            NUMBER indicates level of logging: 0 - 3, 3 is highest level\n");
    fprintf(stdout, "    --logarea=NUMBER           set logarea mask (0=no area)\n");
#endif

    fprintf(stdout, "    --access=NUMBER            set accessmode (0=no check,1=root has access,2=check client)\n");
    fprintf(stdout, "    --testmode[=0/1]           enable testmode (1=testmode, 0=default)\n");
    fprintf(stdout, "    --enablefusefs[=0/1]       enable testmode (1=enable fuse fs, 0=default)\n");
    fprintf(stdout, "    --forwardlocal[=0/1]       try to forward a watch to a local fs like a fuse fs (0=default)\n");
    fprintf(stdout, "    --forwardnetwork[=0/1]     try to forward a watch to a remote notifyfs server (0=default)\n");
    fprintf(stdout, "    --listennetwork[=0/1]      listen on the network for forwarded watches (0=default)\n");
    fprintf(stdout, "    --networkport              the networkport to listen to (790=default)\n");
    fprintf(stdout, "    --remoteserversfile=FILE   file with ipv4 addresses of remote servers (default /etc/notifyfs/servers\n");
    fprintf(stdout, "    --fuseoptions=opt1,opt2,.. add extra options to fuse:\n");
    fprintf(stdout, "      auto_unmount             auto unmount on process termination\n");
    fprintf(stdout, "      nonempty                 allow mounts over non-empty file/dir\n");
    fprintf(stdout, "      fsname=NAME              set filesystem name\n");
    fprintf(stdout, "      subtype=NAME             set filesystem type\n");
    fprintf(stdout, "\n");

}

static void print_version()
{

    printf("Notifyfs version %s\n", PACKAGE_VERSION);
    //printf("Fuse version %s\n", fuse_version());
    /* here kernel module version... */

}

/* function which processes one fuse option by adding it to the fuse arguments list 
   important here is that every fuse option has to be prefixed by a -o */

static int parsefuseoption(struct fuse_args *notifyfs_fuse_args, char *fuseoption)
{
    int len=strlen("-o")+strlen(fuseoption)+1;
    char tmpoption[len];

    memset(tmpoption, '\0', len);
    snprintf(tmpoption, len, "-o%s", fuseoption);

    return fuse_opt_add_arg(notifyfs_fuse_args, tmpoption);

}

/* function to convert a string with options like nodev,nosuid,nonempty to a list
   fuse can process 
   and additional default options are set */

static int parsefuseoptions(struct fuse_args *notifyfs_fuse_args, char *fuseoptions)
{
    int nreturn=0;
    unsigned char subtypeparsed=0;
    unsigned char allowotherparsed=0;
    unsigned char nodevparsed=0;
    unsigned char nosuidparsed=0;

    if ( fuseoptions ) {
	char *pcomma;
	char *fuseoption;

	fuseoption=fuseoptions;

	checkoption:

	if ( strlen(fuseoption)==0 ) goto out;

	/* look where option ends */

	pcomma=strchr(fuseoption, ',');

	if ( pcomma ) {

	    /* replace temporary by a string terminator to isolate the option */

	    *(pcomma)='\0';

	    nreturn=parsefuseoption(notifyfs_fuse_args, fuseoption);
	    if (nreturn<0) goto out;

	    if (strcmp(fuseoption, "subtype")==0) subtypeparsed=1;
	    if (strcmp(fuseoption, "allow_other")==0) allowotherparsed=1;
	    if (strcmp(fuseoption, "nodev")==0) nodevparsed=1;
	    if (strcmp(fuseoption, "nosuid")==0) nosuidparsed=1;

	    /* put comma back */

	    *(pcomma)=',';

	    fuseoption=pcomma+1;

	    goto checkoption;

	} else {

	    nreturn=fuse_opt_add_arg(notifyfs_fuse_args, fuseoption);

	}

    }

    if ( subtypeparsed==0 ) {

	/* subtype must be specified */

	nreturn=parsefuseoption(notifyfs_fuse_args, "subtype=notifyfs");

    }

    if ( allowotherparsed==0 ) {

	/* is allowother already specified, is required for this fs*/

	nreturn=parsefuseoption(notifyfs_fuse_args, "allow_other");

    }

    if ( nodevparsed==0 ) {

	/* is nodev already specified, is a good option for this fs*/

	nreturn=parsefuseoption(notifyfs_fuse_args, "nodev");

    }

    if ( nosuidparsed==0 ) {

	/* is nosuid already specified, is a good option for this fs*/

	nreturn=parsefuseoption(notifyfs_fuse_args, "nosuid");

    }

    out:
    return nreturn;

}

static int parse_socket_path(char *path)
{
    int nreturn=0;
    struct stat st;
    struct sockaddr_un localsock;

    if ( strlen(path) >= sizeof(localsock.sun_path) ) {

	fprintf(stderr, "Length of socket %s is too big.\n", path);
	nreturn=-1;
	goto out;

    }

    if ( stat(path, &st)==0 ) {

	/* does exist */

	fprintf(stderr, "Socket %s does exist already, cannot continue.", path);
	nreturn=-1;
	goto out;


    } else {
	char *lastslash;

	/* check the dirname, it must exist */

	lastslash=strrchr(path, '/');

	if ( lastslash ) {
	    unsigned char lenname=strlen(path)+path-lastslash+1;
	    char socketname[lenname];
	    unsigned char lenpath=0;

	    /* store the name in temporary string */
	    memset(socketname, '\0', lenname);
	    strcpy(socketname, lastslash+1);

	    /* replace the slash temporarly by a null byte, making the string terminate here */

	    *lastslash='\0';

	    if ( strlen(path)==0 ) {

		nreturn=-1;
		fprintf(stderr, "Error:option --socket=%s cannot be parsed: path in root. Cannot continue.\n", path);
		goto out;

	    } else if ( ! realpath(path, notifyfs_options.socket) ) {

		nreturn=-1;
		fprintf(stderr, "Error:(%i) option --socket=%s cannot be parsed. Cannot continue.\n", errno, path);
		goto out;

	    }

	    /* check the rare case it does not fit */

	    lenpath=strlen(notifyfs_options.socket);

	    if ( lenpath + 1 + lenname > sizeof(localsock.sun_path)) {

		nreturn=-1;
		fprintf(stderr, "Error: option --socket=%s cannot be parsed: path too long. Cannot continue.\n", path);
		goto out;

	    }

	    *(notifyfs_options.socket+lenpath)='/';
	    lenpath++;

	    memcpy(notifyfs_options.socket+lenpath, socketname, lenname);

	} else {

	    /* no slash ???*/

	    /* ignore, no relative paths */

	    nreturn=-1;
	    fprintf(stderr, "Error: option --socket=%s cannot be parsed: don't parse relative paths. Cannot continue.\n", path);
	    goto out;

	}

    }

    out:

    return nreturn;

}


/* function to parse all the commandline arguments, and split the normal notifyfs arguments 
   and the arguments meant for fuse
   normal options are specified as long options, like --logging
   fuse options are specified in a "single" option with -osomefuseoption,anotherfuseoption*/

int parse_arguments(int argc, char *argv[], struct fuse_args *notifyfs_fuse_args)
{
    static struct option long_options[] = {
	{"help", 		optional_argument, 		0, 0},
	{"version", 		optional_argument, 		0, 0},
	{"logging", 		optional_argument, 		0, 0},
	{"logarea", 		optional_argument, 		0, 0},
	{"testmode", 		optional_argument,      	0, 0},
	{"enablefusefs", 	optional_argument,      	0, 0},
	{"accessmode", 		optional_argument,		0, 0},
	{"socket", 		optional_argument,		0, 0},
	{"listennetwork",	optional_argument,		0, 0},
	{"forwardnetwork",	optional_argument,		0, 0},
	{"forwardlocal",	optional_argument,		0, 0},
	{"conffile",		optional_argument,		0, 0},
	{"remoteserversfile",	optional_argument,		0, 0},
	{"mountpoint", 		optional_argument,		0, 0},
	{"fuseoptions", 	optional_argument, 		0, 0},
	{0,0,0,0}
	};
    int res, long_options_index=0, nreturn=0;
    char *fuseoptions=NULL;
    struct stat st;

    /* set defaults */

    /* no logging*/

    notifyfs_options.logging=0;

    /* only the filesystem logging */

    notifyfs_options.logarea=LOG_LOGAREA_FILESYSTEM;

    /* testmode for now default (in future not) */

    notifyfs_options.testmode=0;

    /* everyone access */

    notifyfs_options.accessmode=0;

    /* enable the mounting of fuse fs */

    notifyfs_options.enablefusefs=1;

    /* socket */

    memset(notifyfs_options.socket, '\0', PATH_MAX);

    /* pidfile */

    memset(notifyfs_options.pidfile, '\0', PATH_MAX);

    /* mountpoint */

    notifyfs_options.mountpoint=NULL;

    /* conf file */

    notifyfs_options.conffile=NULL;

    /* file with remote servers */

    notifyfs_options.remoteserversfile=NULL;

    /* network port */

    notifyfs_options.networkport=790;

    /* do not use ipv6 for now */

    notifyfs_options.ipv6=0;

    /* forwarding */

    notifyfs_options.forwardlocal=0;
    notifyfs_options.forwardnetwork=0;
    notifyfs_options.listennetwork=0;

    /* hide system files */

    notifyfs_options.hidesystemfiles=1;

    /* the group id for the shared memory */

    notifyfs_options.shm_gid=0;

    /* start the fuse options with the program name, just like the normal argv */

    logoutput("parse_options: add fuse arg %s", argv[0]);

    nreturn=fuse_opt_add_arg(notifyfs_fuse_args, argv[0]);
    if (nreturn<0) goto out;


    while(1) {

	res=getopt_long(argc, argv, "", long_options, &long_options_index);

	if ( res==-1 ) {

	    break;

	}

	switch(res) {

	    case 0:

		/* a long option */

		if ( strcmp(long_options[long_options_index].name, "help")==0 ) {

		    print_usage(argv[0]);
		    print_help();
		    nreturn=-1;
		    goto out;


		} else if ( strcmp(long_options[long_options_index].name, "version")==0 ) {

		    print_version(argv[0]);
		    nreturn=-1;
		    goto out;


		} else if ( strcmp(long_options[long_options_index].name, "logging")==0 ) {

		    if ( optarg ) {

			notifyfs_options.logging=atoi(optarg);

		    } else {

			fprintf(stderr, "Warning: option --logging requires an argument. Taking default.\n");

			notifyfs_options.logging=1;

		    }

		} else if ( strcmp(long_options[long_options_index].name, "logarea")==0 ) {

		    if ( optarg ) {

			notifyfs_options.logarea=atoi(optarg);

		    } else {

			fprintf(stderr, "Warning: option --logarea requires an argument. Taking default.\n");

		    }

		} else if ( strcmp(long_options[long_options_index].name, "accessmode")==0 ) {

		    if ( optarg ) {

			notifyfs_options.accessmode=atoi(optarg);

		    } else {

			fprintf(stderr, "Warning: option --accessmode requires an argument. Taking default.\n");

		    }

		} else if ( strcmp(long_options[long_options_index].name, "testmode")==0 ) {

		    if ( optarg ) {

			notifyfs_options.testmode=atoi(optarg);

		    } else {

			fprintf(stderr, "Warning: option --testmode requires an argument. Taking default.\n");

		    }

		} else if ( strcmp(long_options[long_options_index].name, "enablefuse")==0 ) {

		    if ( optarg ) {

			notifyfs_options.enablefusefs=(atoi(optarg)>0) ? 1 : 0;

		    } else {

			fprintf(stderr, "Warning: option --enablefusefs requires an argument. Taking default.\n");

		    }

		} else if ( strcmp(long_options[long_options_index].name, "socket")==0 ) {

		    if ( optarg ) {

			parse_socket_path(optarg);

		    } else {

			fprintf(stderr, "Error: option --socket requires an argument. Abort.\n");
			nreturn=-1;
			goto out;

		    }

		} else if ( strcmp(long_options[long_options_index].name, "conffile")==0 ) {

		    if ( optarg ) {

			notifyfs_options.conffile=check_path(optarg);

			if ( ! notifyfs_options.conffile ) {

			    nreturn=-1;
			    goto out;

			}

		    } else {

			fprintf(stderr, "Warning: option --conffile requires an argument. Ignore.\n");

		    }

		} else if ( strcmp(long_options[long_options_index].name, "remoteserversfile")==0 ) {

		    if ( optarg ) {

			notifyfs_options.remoteserversfile=check_path(optarg);

			if ( ! notifyfs_options.remoteserversfile ) {

			    nreturn=-1;
			    goto out;

			}

		    } else {

			fprintf(stderr, "Warning: option --remoteserversfile requires an argument. Ignore.\n");

		    }

		} else if ( strcmp(long_options[long_options_index].name, "fuseoptions")==0 ) {

		    if ( optarg ) {

			fuseoptions=strdup(optarg);

			if ( ! fuseoptions ) {

			    nreturn=-1;
			    goto out;

			}


		    } else {

			fprintf(stderr, "Warning: option --fuseoptions requires an argument. Ignoring.\n");

		    }

		} else if ( strcmp(long_options[long_options_index].name, "mountpoint")==0 ) {

		    if ( optarg ) {

			notifyfs_options.mountpoint=check_path(optarg);

			if ( ! notifyfs_options.conffile ) {

			    nreturn=-1;
			    fprintf(stderr, "Error:(%i) option --mountpoint=%s cannot be parsed. Cannot continue.\n", errno, optarg);
			    goto out;

			}

		    } else {

			fprintf(stderr, "Error: option --mountpoint requires an argument. Cannot continue.\n");
			nreturn=-1;
			goto out;

		    }

		}

	    case '?':

		break;

	    default:

		fprintf(stdout,"Warning: getoption returned character code 0%o!\n", res);

	}

    }

    /* parse the fuse options */

    //nreturn=parsefuseoptions(notifyfs_fuse_args, fuseoptions);
    //if ( nreturn<0 ) goto out;

    out:

    return nreturn;

}

int read_global_settings_from_file(char *path)
{
    FILE *fp;
    char line[512];
    char *sep, *option, *value;
    int nreturn=0;

    fp=fopen(path, "r");

    if  ( !fp) {

	nreturn=-errno;
	goto out;

    }

    while( ! feof(fp)) {

	if ( ! fgets(line, 512, fp)) continue;

	sep=strchr(line, '\n');
	if (sep) *sep='\0';

	sep=strchr(line, '=');
	if (!sep) continue;

	*sep='\0';
	option=line;
	value=sep+1;

	convert_to(option, UTILS_CONVERT_SKIPSPACE);
	convert_to(option, UTILS_CONVERT_TOLOWER);


	if ( strcmp(option, "general.logging")==0 ) {

	    if (strlen(value)>0) {

		notifyfs_options.logging=atoi(value);

	    }

	} else if ( strcmp(option, "general.logarea")==0 ) {

	    if (strlen(value)>0) {

		notifyfs_options.logarea=atoi(value);

	    }

	} else if ( strcmp(option, "general.enablefusefs")==0 ) {

	    if (strlen(value)>0) {

		notifyfs_options.enablefusefs=(atoi(value)>0) ? 1 : 0;

	    }

	} else if ( strcmp(option, "local.socket")==0 ) {

	    if ( strlen(notifyfs_options.socket)>0 ) {

		logoutput("read_global_settings: socket already set (%s)", notifyfs_options.socket);
		continue;

	    } else {

		if ( strlen(value)>0) {

		    convert_to(value, UTILS_CONVERT_SKIPSPACE);

		    parse_socket_path(value);

		    logoutput("read_global_settings: socket set to %s", notifyfs_options.socket);

		} else {

		    logoutput("read_global_settings: error: option local.socket requires an argument. Abort.");
		    nreturn=-1;
		    goto out;

		}

	    }


	} else if ( strcmp(option, "local.forward")==0 ) {

	    if (strlen(value)>0) {

		notifyfs_options.forwardlocal=(atoi(value)>0) ? 1 : 0;

	    }

	} else if ( strcmp(option, "network.forward")==0 ) {

	    if (strlen(value)>0) {

		notifyfs_options.forwardnetwork=(atoi(value)>0) ? 1 : 0;

	    }

	} else if ( strcmp(option, "network.listen")==0 ) {

	    if (strlen(value)>0) {

		notifyfs_options.listennetwork=(atoi(value)>0) ? 1 : 0;

	    }

	} else if ( strcmp(option, "shm.group")==0 ) {

	    struct group *grp;

    	    grp=getgrnam(value);

    	    if ( grp ) {

        	notifyfs_options.shm_gid=grp->gr_gid;
        	logoutput("found shm group %s, gid %i", value, notifyfs_options.shm_gid);

    	    } else {

		logoutput("shm group %s not found", value);

	    }

	} else if ( strcmp(option, "network.port")==0 ) {

	    if (strlen(value)>0) {

		notifyfs_options.networkport=atoi(value);

	    }

	} else if ( strcmp(option, "network.serversfile")==0 ) {

	    if (notifyfs_options.remoteserversfile) {

		logoutput("read_global_settings: remoteserversfile already set");
		continue;

	    } else {

		if (strlen(value)>0) {

		    notifyfs_options.remoteserversfile=check_path(value);

		    if ( ! notifyfs_options.remoteserversfile ) {

			nreturn=-1;
			goto out;

		    }

		} else {

		    logoutput("Warning: option serversfile requires an argument. Ignore.");

		}

	    }

	}

    }

    out:

    if (fp) fclose(fp);

    return nreturn;

}
