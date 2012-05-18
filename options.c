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

#include "logging.h"
#include "notifyfs.h"

#include "options.h"

extern struct notifyfs_options_struct notifyfs_options;

static void print_usage(const char *progname)
{
	fprintf(stdout, "usage: %s [opts]"
	                "          --socket=FILE\n"
	                "          [--logging=NR,]\n"
	                "          [--logarea=NR,]\n"
	                "          [--accessmode=NR,]\n"
	                "          [--testmode]\n"
	                "          [--filesystems]\n"
			"          [--forwardovernetwork]\n"
			"          [--listennetwork]\n"
			"          [--networkport=NR]\n"
	                "          [--fuseoptions=STRING]\n"
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
    fprintf(stdout, "    --filesystems[=0/1]        enable filesystems as backend (0=no filesystems, 1=default)\n");
    fprintf(stdout, "    --forwardovernetwork[=0/1] enable forwarding of watches over network (0=no forwarding,default)\n");
    fprintf(stdout, "    --listennetwork[=0/1]      enable on network for incoming watches (0=not listening,default)\n");
    fprintf(stdout, "    --networkport=NR           networkport when forwarding or listening\n");
    fprintf(stdout, "    --fuseoptions=opt1,opt2,.. add extra options to fuse:\n");
    fprintf(stdout, "      auto_unmount 	    auto unmount on process termination\n");
    fprintf(stdout, "      nonempty 	    	    allow mounts over non-empty file/dir\n");
    fprintf(stdout, "      fsname=NAME 	            set filesystem name\n");
    fprintf(stdout, "      subtype=NAME 	    set filesystem type\n");
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
	{"accessmode", 		optional_argument,		0, 0},
	{"filesystems", 	optional_argument,		0, 0},
	{"socket", 		optional_argument,		0, 0},
	{"mountpoint", 		optional_argument,		0, 0},
	{"fuseoptions", 	optional_argument, 		0, 0},
	{"networkport", 	optional_argument, 		0, 0},
	{"forwardovernetwork", 	optional_argument, 		0, 0},
	{"listennetwork",	optional_argument, 		0, 0},
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

    /* socket */

    memset(notifyfs_options.socket, '\0', UNIX_PATH_MAX);

    /* mountpoint */

    memset(notifyfs_options.mountpoint, '\0', PATH_MAX);

    /* network port */

    notifyfs_options.networkport=790;

    /* forward watches over network */

    notifyfs_options.forwardovernetwork=0;

    /* listen to incoming connections from other hosts */

    notifyfs_options.listennetwork=0;

    /* start the fuse options with the program name, just like the normal argv */

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

		} else if ( strcmp(long_options[long_options_index].name, "socket")==0 ) {

		    if ( optarg ) {

			if ( strlen(optarg) >= UNIX_PATH_MAX ) {

			    fprintf(stderr, "Length of socket %s is too big.\n", optarg);
			    nreturn=-1;
			    goto out;

			}

			if ( stat(optarg, &st)!=-1 ) {

			    /* does exist */

			    fprintf(stderr, "Socket %s does exist already, cannot continue.", notifyfs_options.socket);
			    nreturn=-1;
			    goto out;


			} else {
			    char *lastslash;

			    /* check the dirname, it must exist */

			    lastslash=strrchr(optarg, '/');

			    if ( lastslash ) {
				unsigned char lenname=strlen(optarg)+optarg-lastslash+1;
				char socketname[lenname];
				unsigned char lenpath=0;

				/* store the name in temporary string */
				memset(socketname, '\0', lenname);
				strcpy(socketname, lastslash+1);

				/* replace the slash temporarly by a null byte, making the string terminate here */

				*lastslash='\0';

				if ( strlen(optarg)==0 ) {

				    nreturn=-1;
				    fprintf(stderr, "Error:option --socket=%s cannot be parsed: path in root. Cannot continue.\n", optarg);
				    goto out;

				} else if ( ! realpath(optarg, notifyfs_options.socket) ) {

				    nreturn=-1;
				    fprintf(stderr, "Error:(%i) option --socket=%s cannot be parsed. Cannot continue.\n", errno, optarg);
				    goto out;

				}

				/* check the rare case it does not fit */

				lenpath=strlen(notifyfs_options.socket);

				if ( lenpath + 1 + lenname > UNIX_PATH_MAX ) {

				    nreturn=-1;
				    fprintf(stderr, "Error: option --socket=%s cannot be parsed: path too long. Cannot continue.\n", optarg);
				    goto out;

				}

				*(notifyfs_options.socket+lenpath)='/';
				lenpath++;

				memcpy(notifyfs_options.socket+lenpath, socketname, lenname);


			    } else {

				/* no slash ???*/

				/* ignore, no relative paths */

				nreturn=-1;
				fprintf(stderr, "Error: option --socket=%s cannot be parsed: don't parse relative paths. Cannot continue.\n", optarg);
				goto out;

			    }

			}

			fprintf(stdout, "Taking socket %s.\n", notifyfs_options.socket);

		    } else {

			fprintf(stderr, "Error: option --socket requires an argument. Abort.\n");
			nreturn=-1;
			goto out;

		    }

		} else if ( strcmp(long_options[long_options_index].name, "filesystems")==0 ) {

		    if ( optarg ) {

			notifyfs_options.filesystems=atoi(optarg);

			if ( notifyfs_options.filesystems!=0) notifyfs_options.filesystems=1;

		    } else {

			fprintf(stderr, "Warning: option --filesystems requires an argument. Enable.\n");

			notifyfs_options.filesystems=1;

		    }

		} else if ( strcmp(long_options[long_options_index].name, "forwardovernetwork")==0 ) {

		    if ( optarg ) {

			notifyfs_options.forwardovernetwork=(atoi(optarg)>0) ? 1 : 0;

		    } else {

			fprintf(stderr, "Enable forwarding of watches over network.\n");

			notifyfs_options.forwardovernetwork=1;

		    }

		} else if ( strcmp(long_options[long_options_index].name, "listennetwork")==0 ) {

		    if ( optarg ) {

			notifyfs_options.listennetwork=(atoi(optarg)>0) ? 1 : 0;

		    } else {

			fprintf(stderr, "Enable listening on network.\n");

			notifyfs_options.listennetwork=1;

		    }

		} else if ( strcmp(long_options[long_options_index].name, "networkport")==0 ) {

		    if ( optarg ) {

			notifyfs_options.networkport=atoi(optarg);

		    } else {

			fprintf(stderr, "Warning: listening on networkport requires an argument. Cannot continue.\n");

			notifyfs_options.networkport=0;
			nreturn=-1;
			goto out;

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

			if ( ! realpath(optarg, notifyfs_options.mountpoint)) {

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

    /* check networkoptions */

    if ( notifyfs_options.listennetwork==1 || notifyfs_options.forwardovernetwork==1 ) {

	/* the network port must be defined (and the same for every server) */

	if ( notifyfs_options.networkport==0 ) {

	    fprintf(stderr, "Error: when forwarding watches over network or listening on the network a port must be set. Cannot continue.\n");
	    nreturn=-1;
	    goto out;

	}

    } else {

	/* not listening and not forwarding over the network: the port doesn't have to be set 
           when that is the case issue a warning */

	if ( notifyfs_options.networkport>0 ) {

	    fprintf(stderr, "Warning: when not forwarding watches over network and not listening on the network setting a networkport is useless.\n");

	}

    }

    /* parse the fuse options */

    nreturn=parsefuseoptions(notifyfs_fuse_args, fuseoptions);
    if ( nreturn<0 ) goto out;

    out:

    return nreturn;

}

