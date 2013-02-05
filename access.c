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

#include <pthread.h>

#include <fuse/fuse_lowlevel.h>

#include "entry-management.h"
#include "logging.h"

#include "access.h"

#define LOG_LOGAREA LOG_LOGAREA_PATH_RESOLUTION

unsigned char call_path_lock;
pthread_mutex_t call_path_mutex;
pthread_cond_t call_path_condition;

struct call_path_struct *call_path_list=NULL;
struct call_path_struct *call_path_unused=NULL;

pthread_mutex_t call_path_unused_mutex;

static int check_groupmember(fuse_req_t req, gid_t group)
{
    int nrgrps=25;
    int nreturn=0;

    while(1) {
        gid_t grplist[nrgrps];
        int res=0, i;

        res=fuse_req_getgroups(req, nrgrps, grplist);

        if ( res<0 ) {

            nreturn=res;
            break;

        } else if ( res>nrgrps ) {

            nrgrps *= 2;
            continue;

        } else if ( res==0 ) {

            nreturn=1;
            goto out;

        }

        for (i=0; i<res; i++) {

            logoutput3("check_groupmember, compare group %i with %i", (int) grplist[i], (int) group);

            if ( group==grplist[i] ) {

                nreturn=1;
                goto out;

            }

        }

        break;

    }

    out:

    return nreturn;

}


/* check access (mask) to a file (st) using user context (ctx)
   return:
   - <0 : access denied with error
   - =0 : access denied
   - =1 : access granted

*/

int check_access(fuse_req_t req, struct stat *st, int mask)
{
    int nreturn=0, res;
    const struct fuse_ctx *ctx=fuse_req_ctx(req);

    if (ctx->uid==0) {

	nreturn=1;
	goto out;

    }



    /* deny execute and write on everything but directories */

    if ( ! S_ISDIR(st->st_mode)) {

        if ( mask & ( X_OK | W_OK) ) {

            /* deny execute and write */

            nreturn=-EACCES;
            goto out;

        }

    } else {

	/* when a directory, everybody can read */

	if ( mask==R_OK ) {

	    nreturn=1;
	    goto out;

	}

    }


    /* can user read entries: x and r have to be set for user/group */

    if ( ctx->uid==st->st_uid ) {

        /* check the owner mode */

        logoutput2("CHECK_ACCESS, testing owner permissions");

        res=(st->st_mode & S_IRWXU) >> 6;

	logoutput2("comparing %i with mask %i", res, mask);

        nreturn=(res & mask);

        if ( nreturn ) {

            logoutput2("CHECK_ACCESS granted");
            goto out;

        }

        /* in case of a directory allow the "creating" of entries when rx access */

        if ( mask&W_OK && S_ISDIR(st->st_mode)) {

            /* allow when read and execute permission */

            nreturn=(res & ( R_OK | X_OK ));

            if ( nreturn ) {

                logoutput2("CHECK_ACCESS granted (directory with RX)");
                goto out;

            }

        }

    }


    if ( ctx->gid==st->st_gid ) {

        /* check the group mode */

        logoutput2("CHECK_ACCESS, testing group permissions");

        res=(st->st_mode & S_IRWXG) >> 3;

	logoutput2("comparing %i with mask %i", res, mask);

        nreturn=(res & mask);

        if ( nreturn ) {

            logoutput2("CHECK_ACCESS granted");
            goto out;

        }

        /* in case of a directory allow the "creating" of entries when rx access */

        if ( mask&W_OK && S_ISDIR(st->st_mode)) {

            /* allow when read and execute permission */

	    logoutput2("comparing %i with mask %i", res, mask);

            nreturn=(res & ( R_OK | X_OK ));

            if ( nreturn ) {

                logoutput2("CHECK_ACCESS granted (directory with RX)");
                goto out;

            }

        }

    }

    /* check the supplementary groups*/

    logoutput2("CHECK_ACCESS, testing group permissions (suppl)");

    res=check_groupmember(req, st->st_gid);

    if ( res==1 ) {

        res=(st->st_mode & S_IRWXG) >> 3;

	logoutput2("comparing %i with mask %i", res, mask);

        nreturn=(res & mask);

        if ( nreturn ) {

            logoutput2("CHECK_ACCESS granted");
            goto out;

        }

        /* in case of a directory allow the "creating" of entries when rx access */

        if ( mask&W_OK && S_ISDIR(st->st_mode)) {

            /* allow when read and execute permission */

            nreturn=(res & ( R_OK | X_OK ));

            if ( nreturn ) {

                logoutput2("CHECK_ACCESS granted (directory with RX)");
                goto out;

            }

        }

    }

    /* TODO check the ACL via Xattr 
    */

    /* check the other mask */

    logoutput2("CHECK_ACCESS, testing other permissions");

    res=(st->st_mode & S_IRWXO);

    logoutput2("comparing %i with mask %i", res, mask);

    nreturn=(res & mask);

    if ( nreturn ) {

        logoutput2("CHECK_ACCESS granted (%i)", nreturn);
        goto out;

    }

    /* in case of a directory allow the "creating" of entries */

    if ( mask&W_OK && S_ISDIR(st->st_mode)) {

        /* allow when read and execute permission */

        nreturn=(res & ( R_OK | X_OK ));

        if ( nreturn ) {

            logoutput2("CHECK_ACCESS granted (directory with RX)");
            goto out;

        }

    }

    out:

    logoutput2("check_access: value nreturn %i", nreturn);

    return (nreturn>0) ? 1 : 0;

}
