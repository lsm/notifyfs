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
#include <dirent.h>

#include "logging.h"
#include "testfs.h"

#include "options.h"

extern struct testfs_options_struct testfs_options;

static void print_usage(const char *progname)
{
    char *slash;
    char output[80];
    unsigned char len;

    slash=strrchr(progname, '/');

    if ( slash ) {

	len=snprintf(output, 80, "Usage : %s ", slash+1);

    } else {

	len=snprintf(output, 80, "Usage : %s ", progname);

    }

    fprintf(stdout, "%s [--logging[=NR]]\n", output);

    memset(output, ' ', len);
    *(output+len+1)='\0';

    fprintf(stdout, "%s [--notifyfssocket=PATH]\n", output);
    fprintf(stdout, "%s [--logarea=NR]\n", output);
    fprintf(stdout, "%s [--fuseoptions=STRING]\n", output);
    fprintf(stdout, "%s --mountpoint=PATH\n", output);
    fprintf(stdout, "\n");

}

static void print_help() {
    unsigned char defaultloglevel=1;

#ifdef LOG_DEFAULT_LEVEL
    defaultloglevel=LOG_DEFAULT_LEVEL;
#endif

    fprintf(stdout, "General options:\n");
    fprintf(stdout, "    --opt                      testfs option\n");
    fprintf(stdout, "    --help                     print help\n");
    fprintf(stdout, "    --version                  print version\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "Testfs options:\n");
    fprintf(stdout, "    --notifyfssocket=FILE      socket of notifyfs\n");

#ifdef LOGGING
    fprintf(stdout, "    --logging=NUMBER           set loglevel\n");
    fprintf(stdout, "                               when omitted no logging\n");
    fprintf(stdout, "                               without number take the default: %i\n", defaultloglevel);
    fprintf(stdout, "                               NUMBER indicates level of logging: 0 - 3, 3 is highest level\n");
    fprintf(stdout, "    --logarea=NUMBER           set logarea mask (0=no area)\n");
#endif

    fprintf(stdout, "    --fuseoptions=opt1,opt2,.. add extra options to fuse:\n");
    fprintf(stdout, "       auto_unmount            auto unmount on process termination\n");
    fprintf(stdout, "       nonempty                allow mounts over non-empty file/dir\n");
    fprintf(stdout, "       fsname=NAME             set filesystem name\n");
    fprintf(stdout, "       subtype=NAME            set filesystem type\n");
    fprintf(stdout, "\n");

}

static void print_version()
{

    printf("Testfs version %s\n", PACKAGE_VERSION);
    //printf("Fuse version %s\n", fuse_version());
    /* here kernel module version... */

}

/* function which processes one fuse option by adding it to the fuse arguments list 
   important here is that every fuse option has to be prefixed by a -o */

static int parsefuseoption(struct fuse_args *fs_fuse_args, char *fuseoption)
{
    int len=strlen("-o")+strlen(fuseoption)+1;
    char tmpoption[len];

    memset(tmpoption, '\0', len);
    snprintf(tmpoption, len, "-o%s", fuseoption);

    logoutput("parsefuseoption: add arg %s to fuse", tmpoption);

    return fuse_opt_add_arg(fs_fuse_args, tmpoption);

}

/* function to convert a string with options like nodev,nosuid,nonempty to a list
   fuse can process 
   and additional default options are set */

static int parsefuseoptions(struct fuse_args *fs_fuse_args, char *fuseoptions)
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

	    nreturn=parsefuseoption(fs_fuse_args, fuseoption);
	    if (nreturn<0) goto out;

	    if (strncmp(fuseoption, "subtype=",8)==0) subtypeparsed=1;
	    if (strcmp(fuseoption, "allow_other")==0) allowotherparsed=1;
	    if (strcmp(fuseoption, "nodev")==0) nodevparsed=1;
	    if (strcmp(fuseoption, "nosuid")==0) nosuidparsed=1;

	    /* put comma back */

	    *(pcomma)=',';

	    fuseoption=pcomma+1;

	    goto checkoption;

	} else {

	    nreturn=parsefuseoption(fs_fuse_args, fuseoption);

	    if (strncmp(fuseoption, "subtype=",8)==0) subtypeparsed=1;
	    if (strcmp(fuseoption, "allow_other")==0) allowotherparsed=1;
	    if (strcmp(fuseoption, "nodev")==0) nodevparsed=1;
	    if (strcmp(fuseoption, "nosuid")==0) nosuidparsed=1;


	}

    }

    if ( subtypeparsed==0 ) {

	/* subtype must be specified */

	nreturn=parsefuseoption(fs_fuse_args, "subtype=testfs");

    }

    if ( allowotherparsed==0 ) {

	/* is allowother already specified, is required for this fs*/

	nreturn=parsefuseoption(fs_fuse_args, "allow_other");

    }

    if ( nodevparsed==0 ) {

	/* is nodev already specified, is a good option for this fs*/

	nreturn=parsefuseoption(fs_fuse_args, "nodev");

    }

    if ( nosuidparsed==0 ) {

	/* is nosuid already specified, is a good option for this fs*/

	nreturn=parsefuseoption(fs_fuse_args, "nosuid");

    }

    out:
    return nreturn;

}


/* function to parse all the commandline arguments, and split the normal notifyfs arguments 
   and the arguments meant for fuse
   normal options are specified as long options, like --logging
   fuse options are specified in a "single" option with -osomefuseoption,anotherfuseoption*/

int parse_arguments(int argc, char *argv[], struct fuse_args *fs_fuse_args)
{
    static struct option long_options[] = {
	{"help", optional_argument, 			0, 0},
	{"version", optional_argument, 			0, 0},
	{"logging", optional_argument, 			0, 0},
	{"logarea", optional_argument, 			0, 0},
	{"notifyfssocket", optional_argument,		0, 0},
	{"mountpoint", optional_argument,		0, 0},
	{"fuseoptions", optional_argument, 		0, 0},
	{0,0,0,0}
	};
    int res, long_options_index=0, nreturn=0;
    char *fuseoptions=NULL;
    struct stat st;

    /* set defaults */

    logoutput("parse_arguments");

    /* no logging*/

    testfs_options.logging=0;

    /* only the filesystem logging */

    testfs_options.logarea=LOG_LOGAREA_FILESYSTEM;

    /* socket */

    memset(testfs_options.notifyfssocket, '\0', UNIX_PATH_MAX);

    /* mountpoint */

    memset(testfs_options.mountpoint, '\0', PATH_MAX);


    /* start the fuse options with the program name, just like the normal argv */

    nreturn=fuse_opt_add_arg(fs_fuse_args, argv[0]);
    if (nreturn<0) goto out;


    while(1) {

	res=getopt_long(argc, argv, "", long_options, &long_options_index);

	if ( res==-1 ) {

	    nreturn=-2;
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

		    /* setup a connection with the kernel to get kernel fuse version */

		    fuse_opt_add_arg(fs_fuse_args, "--version");

		    /* what does this? */

		    fuse_parse_cmdline(fs_fuse_args, NULL, NULL, NULL);

		    /* get version from kernel */

		    fuse_lowlevel_new(fs_fuse_args, NULL, 0, NULL);

		    nreturn=-1;
		    goto out;


		} else if ( strcmp(long_options[long_options_index].name, "logging")==0 ) {

		    if ( optarg ) {

			testfs_options.logging=atoi(optarg);

		    } else {

			fprintf(stderr, "Warning: option --logging requires an argument. Taking default.\n");

			testfs_options.logging=1;

		    }

		} else if ( strcmp(long_options[long_options_index].name, "logarea")==0 ) {

		    if ( optarg ) {

			testfs_options.logarea=atoi(optarg);

		    } else {

			fprintf(stderr, "Warning: option --logarea requires an argument. Taking default.\n");

		    }


		} else if ( strcmp(long_options[long_options_index].name, "notifyfssocket")==0 ) {

		    if ( optarg ) {

			if ( strlen(optarg) >= UNIX_PATH_MAX ) {

			    fprintf(stderr, "Length of socket %s is too big.\n", optarg);
			    nreturn=-2;
			    goto out;

			}

			if ( stat(optarg, &st)==-1 ) {

			    /* does exist */

			    fprintf(stderr, "Socket %s does not exist, cannot continue.", optarg);
			    nreturn=-2;
			    goto out;


			} else {

			    /* check the dirname, it must exist */

			     if ( ! realpath(optarg, testfs_options.notifyfssocket) ) {

				nreturn=-2;
				fprintf(stderr, "Error:(%i) option --notifyfssocket=%s cannot be parsed. Cannot continue.\n", errno, optarg);
				goto out;

			    }

			}

			fprintf(stdout, "Taking socket %s.\n", testfs_options.notifyfssocket);

		    } else {

			fprintf(stderr, "Error: option --socket requires an argument. Abort.\n");
			nreturn=-2;
			goto out;

		    }

		} else if ( strcmp(long_options[long_options_index].name, "fuseoptions")==0 ) {

		    if ( optarg ) {

			fuseoptions=strdup(optarg);

			if ( ! fuseoptions ) {

			    nreturn=-2;
			    goto out;

			}


		    } else {

			fprintf(stderr, "Warning: option --fuseoptions requires an argument. Ignoring.\n");

		    }

		} else if ( strcmp(long_options[long_options_index].name, "mountpoint")==0 ) {

		    if ( optarg ) {

			if ( ! realpath(optarg, testfs_options.mountpoint)) {

			    nreturn=-2;
			    fprintf(stderr, "Error:(%i) option --mountpoint=%s cannot be parsed. Cannot continue.\n", errno, optarg);
			    goto out;

			}

		    } else {

			fprintf(stderr, "Error: option --mountpoint requires an argument. Cannot continue.\n");
			nreturn=-2;
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

    nreturn=parsefuseoptions(fs_fuse_args, fuseoptions);

    out:

    return nreturn;

}

