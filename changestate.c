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
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/inotify.h>

#include <fuse/fuse_lowlevel.h>

#include "entry-management.h"
#include "path-resolution.h"
#include "logging.h"
#include "notifyfs.h"

#include "utils.h"
#include "options.h"
#include "client.h"
#include "mountinfo.h"
#include "watches.h"
#include "epoll-utils.h"

#include "message.h"
#include "message-server.h"
#include "determinechanges.h"

#include "networksocket.h"
#include "networkutils.h"
#include "changestate.h"

#define LOG_LOGAREA LOG_LOGAREA_FILESYSTEM

extern struct notifyfs_options_struct notifyfs_options;
extern struct fuse_chan *notifyfs_chan;

pthread_mutex_t changestate_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t changestate_condition=PTHREAD_COND_INITIALIZER;

struct call_info_struct *changestate_call_info_first=NULL;
struct call_info_struct *changestate_call_info_last=NULL;


/* function to inform waiting clients interested in a specific watch about an event
   note:

   - name is without trailing string terminator, and len is the exact len of name
     to make this work append a terminator here
*/

void send_notify_message_clients(struct effective_watch_struct *effective_watch, int mask, int len, char *name, struct stat *st, unsigned char remote)
{
    struct watch_struct *watch=effective_watch->watches;
    int effmask, res;
    char stringtosend[len+1];

    if ( len>0 ) {

	memcpy(stringtosend, name, len);
	stringtosend[len]='\0';

    }

    logoutput("send_notify_message_clients");

    /* walk through watches/clients attached to inode */
    /* here lock the effective watch.... */

    while(watch) {

        if (watch->client) {

	    if ( watch->client->status_app==NOTIFYFS_CLIENTSTATUS_UP ) {

        	/* what will be sent is the combination of what happened and the client is interested in */

        	effmask=(watch->mask & mask);

        	if ( effmask>0 ) {

            	    /* send client data */

            	    /* here something like send_inotify_message_client(client, reference of wd, effmask) 
            	    what is missing here is a reference of the wd which makes sense to the client
                    obviously the client must have a reference back when reporting an event
                    it's not that easy cause the watch has been initiated/set using xattr
                    maybe just use the path, or use a unique id per client, also to be get
                    via xattr (system.inotifyfs_watchid)
                    */

		    if ( len==0 ) {

            		res=send_notify_message(watch->client->fd, watch->client_watch_id, mask, name, len, st, remote);

		    } else {

			res=send_notify_message(watch->client->fd, watch->client_watch_id, mask, stringtosend, len+1, st, remote);

		    }

            	    if (res<0) logoutput0("Error (%i) sending inotify event to client on fd %i.", abs(res), watch->client->fd);

        	}

	    }

        }

	watch=watch->next_per_watch;

    }

}

/* function to inform waiting clients interested in a specific watch about removing/waking up/set to sleep the watch
*/

void send_status_message_clients(struct effective_watch_struct *effective_watch, unsigned char typemessage)
{
    struct watch_struct *watch=effective_watch->watches;
    int effmask, res;

    /* walk through watches/clients attached to inode */
    /* here lock the effective watch.... */

    while(watch) {

        if (watch->client) {

	    if ( watch->client->status_app==NOTIFYFS_CLIENTSTATUS_UP ) {

		if ( typemessage==NOTIFYFS_MESSAGE_FSEVENT_DELWATCH ) {

            	    res=send_delwatch_message(watch->client->fd, watch->id);

		} else if ( typemessage==NOTIFYFS_MESSAGE_FSEVENT_SLEEPWATCH ) {

            	    res=send_sleepwatch_message(watch->client->fd, watch->id);

		} else if ( typemessage==NOTIFYFS_MESSAGE_FSEVENT_WAKEWATCH ) {

            	    res=send_wakewatch_message(watch->client->fd, watch->id);

		}

            	if (res<0) logoutput0("Error (%i) sending inotify event to client on fd %i.", abs(res), watch->client->fd);

	    }

        }

	watch=watch->next_per_watch;

    }

}



/* function to read the contents of a directory when a watch is set on that directory 

   a. this function is called when a watch is set. this function reads the contents of the directory to 
   get a copy of every inode/entry of the underlying fs. This is usefull when an event arrives from 
   inotify or backend. It is required to be compared with the cached values first.

   b. this function can also be called when a autofs managed mounted is mounted again, after previously
   unmounted. In the meantime, while it has been unmounted, changes on that fs can occur. When mounted again
   this function compares the new state with the last known and send messages when changes found.

   only do this for autofs mounted filesystems??


*/

void sync_directory_entries(char *path, struct effective_watch_struct *effective_watch, unsigned char atinit, unsigned char lockset)
{
    DIR *dp=NULL;
    struct notifyfs_entry_struct *entry=NULL;
    int res, lenpath;
    pathstring tmppath;

    logoutput("sync_directory_entries: directory %s", effective_watch->path);

    if ( effective_watch->inode ) entry=effective_watch->inode->alias;

    if ( ! entry ) return;

    if ( lockset==0 ) res=lock_effective_watch(effective_watch);

    if ( entry->child ) {
	struct notifyfs_entry_struct *child_entry=entry->child;

	while (child_entry) {

	    child_entry->synced=0;
	    child_entry=child_entry->dir_next;

	}

    }

    lenpath=strlen(path);

    dp=opendir(tmppath);

    if ( dp ) {
	struct dirent *de;
	char *name;
	struct notifyfs_entry_struct *tmpentry;
	int res, lenname=0;
	unsigned char entrycreated=0;
	struct stat st;

	*(path+lenpath)='/';

	while(1) {

	    de=readdir(dp);

	    if ( ! de ) break;

	    /* here check the entry exist... */

	    name=de->d_name;
	    lenname=strlen(name);

	    if ( strcmp(name, ".")==0 ) {

		continue;

	    } else if ( strcmp(name, "..")==0 ) {

		continue;

	    }

	    /* add this entry to the base path */

	    memcpy(path+lenpath+1, name, lenname);
	    *(path+lenpath+1+lenname)='\0';

	    /* read the stat */

	    res=stat(path, &st);

	    if ( res==-1 ) {

		/* huh??? should not happen, ignore */
		continue;

	    } else {

		/* find the matching entry in this fs 
		   if not found create it */

		tmpentry=find_entry(entry->inode->ino, name);
		entrycreated=0;

		if ( ! tmpentry ) {

		    tmpentry=create_entry(entry, name, NULL);

		    /* here handle the not creating of the entry .... */

		    entrycreated=1;

		    /* insert at begin */

		    tmpentry->dir_prev=NULL;

		    if ( entry->child ) entry->child->dir_prev=tmpentry;
		    tmpentry->dir_next=entry->child;
		    entry->child=tmpentry;

		    assign_inode(tmpentry);
		    add_to_name_hash_table(tmpentry);
		    add_to_inode_hash_table(tmpentry->inode);

		}

		tmpentry->synced=1;

		if ( atinit==0 ) {

		    if (entrycreated==0) {
			unsigned char docompare=1;

			/* compare the cached and the new stat 
		    	    only when not at init and with an existing entry */

			/* if there is a watch attached to this entry, this has to take action on this */

			if ( ! tmpentry->inode ) {

			    docompare=0;

			} else if ( tmpentry->inode->effective_watch ) {

			    docompare=0;

			}

			if ( docompare==1 ) {
			    unsigned char statchanged=0, changestatus=FSEVENT_FILECHANGED_NONE;
			    int mask=0;

			    /* TODO: add more checks, like xattr 
			    note that the function compare_attributes also report 
			    changes (in metadata) when xattr have been changed
			    it does not report what, but is that a problem? 
			    in case of a xattr changed, the name of that xattr is sufficient */

			    statchanged=compare_metadata_simple(&(tmpentry->inode->st), &st);

			    if ( statchanged&FSEVENT_FILECHANGED_METADATA ) {

				changestatus|=FSEVENT_FILECHANGED_METADATA;

				/* file and/or metadata has changed: send a message */

				mask|=IN_ATTRIB;

			    }

			    statchanged=compare_file_simple(&(tmpentry->inode->st), &st);

			    if ( statchanged & FSEVENT_FILECHANGED_FILE ) {

				changestatus|=FSEVENT_FILECHANGED_FILE;
				mask|=IN_MODIFY;

			    }

			    if ( changestatus & ( FSEVENT_FILECHANGED_FILE | FSEVENT_FILECHANGED_METADATA ) ) {

				copy_stat(&(tmpentry->inode->st), &st);

				/* what caused this?? local or remote?? if local this must have been 
                                   detected by local fsevent method, so it has to be remote */

				send_notify_message_clients(effective_watch, mask, lenname, name, &st, 1);

			    }

			}

		    } else {

			/* entry is created */

			/* what caused this mew entry?? a move or a create?? it's not known and cannot 
			   be determined...so just take a IN_CREATE*/

			send_notify_message_clients(effective_watch, IN_CREATE, lenname, name, &st, 1);

			copy_stat(&(tmpentry->inode->st), &st);

		    }

		} else {

		    /* the first time: just cache */

		    copy_stat(&(tmpentry->inode->st), &st);

		}

	    }

	}

	closedir(dp);
	*(path+lenpath)='\0';

    }

    /* here check entries which are not detected with the opendir
       these are obviously deleted/moved */

    if ( entry->child ) {
	struct notifyfs_entry_struct *child_entry=entry->child, *next_entry;
	int lenname;

	*(path+lenpath)='/';

	while (child_entry) {

	    next_entry=child_entry->dir_next;

	    if ( child_entry->synced==0 ) {

		/* here: the entry is deleted */
		/*- IN_DELETE (or IN_MOVE, but it's not possible to say here what's causing this....)*/

		logoutput("entry %s is removed", child_entry->name);

		/* send a message about remove 
		   it's impossible to determine what caused this, a move or a delete, so just send a 
		   IN_DELETE */

		send_notify_message_clients(effective_watch, IN_DELETE, strlen(child_entry->name), child_entry->name, NULL, 1);

		/* what if inode has watch attached: remove that too */

		if ( child_entry->inode ) {
		    struct call_info_struct call_info;

		    init_call_info(&call_info, child_entry);

		    /* add this entry to the base path */

		    lenname=strlen(child_entry->name);
		    memcpy(path+lenpath+1, child_entry->name, lenname);
		    *(path+lenpath+1+lenname)='\0';

		    call_info.path=path;

		    changestate(&call_info, FSEVENT_INODE_ACTION_REMOVE);

		} else {

		    /* remove */

		    if ( entry->child==child_entry) entry->child=next_entry;
		    if ( child_entry->dir_prev ) child_entry->dir_prev->dir_next=next_entry;
		    if ( next_entry ) next_entry->dir_prev=child_entry->dir_prev;

		    remove_entry_from_name_hash(child_entry);

		    remove_entry(child_entry);

		}

	    }

	    child_entry=next_entry;

	}

	*(path+lenpath)='\0';

    }

    if ( lockset==0 ) res=unlock_effective_watch(effective_watch);

}

void del_watch_backend(struct effective_watch_struct *effective_watch)
{
    int res;

    if ( effective_watch->inotify_id>0 ) {

	res=inotify_rm_watch(notifyfs_options.inotify_fd, (int) effective_watch->inotify_id);

        if ( res==-1 ) {

            logoutput("del_watch_backend: deleting inotify watch %li gives error: %i", effective_watch->backend_id, errno);

        } else {

            effective_watch->inotify_id=0;

        }

    }

    if ( effective_watch->backendset==1 ) {
	struct client_struct *client=(struct client_struct *) effective_watch->backend;

	/* client has to be defined */

	if ( ! client ) return;

	if ( client->mount_entry ) {

	    logoutput("del_watch_backend: del watch %li for client fd %i/ pid %i/ path %s/ fs %s", effective_watch->backend_id, client->fd, client->pid, client->mount_entry->mountpoint, client->mount_entry->fstype);

	} else {

	    logoutput("del_watch_backend: del watch %li for client fd %i/ pid %i/ (unknown)", effective_watch->backend_id, client->fd, client->pid);

	}

	if ( client->status_fs == NOTIFYFS_CLIENTSTATUS_UP ) {

	    /* forward the removal of the watch to the fs by sending a message to fs */

	    logoutput("del_watch_backend: send delwatch message for watch id %li", effective_watch->backend_id);

	    res=send_delwatch_message(client->fd, effective_watch->backend_id);

	    if ( res>0 ) {

		/* suppose there is no error here, something to do...? some ack */

        	effective_watch->backend_id=0;
        	effective_watch->backendset=0;

	    } else {

		if ( res==0 ) {

		    logoutput("del_watch_backend: error, no bytes send for watch id %li", effective_watch->backend_id);

		} else {

		    logoutput("del_watch_backend: error %i when sending for watch id %li", res, effective_watch->backend_id);

		}

	    }

	} else {

	    logoutput("del_watch_backend: forward watch %li not possible: client is not up", effective_watch->backend_id);

	    effective_watch->backendset=0;

	}

    } else {

        logoutput("del_watch_backend: error: backend %i not reckognized.", effective_watch->typebackend);

    }

}

/* 	function to call backend notify methods like:
	- inotify_add_watch
	- send a forward message to client fs

	called when setting a watch via xattr, but also
	when a fs is unmounted normally or via autofs

*/

void set_watch_backend(struct effective_watch_struct *effective_watch, int newmask, unsigned char lockset)
{
    int res;
    pathstring path;

    /* create path of this watch 
       note the effective watch is relative to the mountpoint
       and if it's empty it's on the mountpoint*/

    if ( effective_watch->path ) {

	if ( is_rootmount(effective_watch->mount_entry) ) {

	    snprintf(path, PATH_MAX, "/%s", effective_watch->path);

	} else {

	    snprintf(path, PATH_MAX, "%s/%s", effective_watch->mount_entry->mountpoint, effective_watch->path);

	}

    } else {

	if ( is_rootmount(effective_watch->mount_entry) ) {

	    snprintf(path, PATH_MAX, "/");

	} else {

	    snprintf(path, PATH_MAX, "%s", effective_watch->mount_entry->mountpoint);

	}

    }

    /* always set a inotify watch */

    logoutput("set_watch_backend: setting inotify watch on %s with mask %i", path, newmask);

    if ( effective_watch->inotify_id==0 ) {

	sync_directory_entries(path, effective_watch, 1, lockset);

    }

    /* by default always set a inotify watch */

    logoutput("set_watch_backend: call inotify_add_watch on fd %i, path %s and mask %i", notifyfs_options.inotify_fd, path, newmask);

    res=inotify_add_watch(notifyfs_options.inotify_fd, path, newmask);

    if ( res==-1 ) {

        logoutput("set_watch_backend: setting inotify watch gives error: %i", errno);

    } else {

	if ( effective_watch->inotify_id>0 ) {

	    /*	when inotify_add_watch is called on a path where a watch has already been set, 
		the watch id should be the same, it's an update... 
		only log when this is not the case */

	    if ( res != effective_watch->inotify_id ) {

		logoutput("set_watch_backend: inotify watch returns a different id: %i versus %li", res, effective_watch->inotify_id);

	    }

	}

	logoutput("set_watch_backend: set backend id %i", res);

        effective_watch->inotify_id=(unsigned long) res;

    }

    if ( notifyfs_options.filesystems>0 ) {

	if ( effective_watch->backend ) {
	    struct client_struct *client=(struct client_struct *) effective_watch->backend;

	    /* here an additional check the client is indeed a fs */

	    if ( client->mount_entry ) {

		logoutput("set_watch_backend: set watch on %s : %li for client fd %i : pid %i : path %s : fs %s", path, effective_watch->backend_id, client->fd, client->pid, client->mount_entry->mountpoint, client->mount_entry->fstype);

	    } else {

		logoutput("set_watch_backend: set watch on %s : %li for client fd %i : pid %i : path unknown : fs unknown", path, effective_watch->backend_id, client->fd, client->pid);

	    }

	    if ( client->status_fs == NOTIFYFS_CLIENTSTATUS_UP ) {
		struct mount_entry_struct *mount_entry=client->mount_entry;

		/* forward the setting of the mask to the fs */

		if ( effective_watch->backend_id==0 ) {

		    /* no backend id yet: simple solution, use the global id */

		    effective_watch->backend_id=effective_watch->id;

		    logoutput("set_watch_backend: forward watch on %s with mask %i, setting id %li", path, newmask, effective_watch->backend_id);

		} else {


		    logoutput("set_watch_backend: forward watch on %s with mask %i, using id %li", path, newmask, effective_watch->backend_id);

		}

		if ( ! effective_watch->path ) {

		    /* path is empty: effective_watch on mount_entry */

		    logoutput("sending a message to set watch mask %i on %i/. (root of mount)", newmask, client->pid);

		    res=send_setwatch_bypath_message(client->fd, effective_watch->backend_id, newmask, ".");


		} else {

		    /* effective watch is on a real subdirectory */

		    logoutput("sending a message to set watch mask %i on %i/%s", newmask, client->pid, effective_watch->path);

		    res=send_setwatch_bypath_message(client->fd, effective_watch->backend_id, newmask, effective_watch->path);

		}

		if ( res>0 ) {

		    /* suppose there is no error here, something to do...? some ack */

        	    effective_watch->backendset=1;

		} else {

		    if ( res==0 ) {

			logoutput("set_watch_backend: error, no bytes send for watch id %li", effective_watch->backend_id);

		    } else {

			logoutput("set_watch_backend: error %i when sending for watch id %li", res, effective_watch->backend_id);

		    }

		}

	    } else {

		if ( client->mount_entry ) {

		    logoutput("set_watch_backend: forward watch on %s not possible: client %s is not up", path, client->mount_entry->fstype);

		} else {

		    logoutput("set_watch_backend: forward watch on %s not possible: client (unknown) is not up", path);

		}

		effective_watch->backendset=0;

	    }

	}

    }

}

/* function to remove every watch attached to an effective_watch 
   effective_watch must be locked
   when a watch is removed, a message is send to the client owning the watch
   */

int changestate_watches(struct effective_watch_struct *effective_watch, unsigned char typeaction)
{
    struct watch_struct *watch=effective_watch->watches, *next_watch;
    int nreturn=0;
    int res;

    while (watch) {

        next_watch=watch->next_per_watch;

        /* notify client */

        if ( watch->client ) {
            struct client_struct *client=watch->client;

            if ( client->status_app==NOTIFYFS_CLIENTSTATUS_UP ) {

                /* only send a message when client is up */

                if ( typeaction==WATCH_ACTION_REMOVE ) {

                    res=send_delwatch_message(client->fd, watch->id);

                    if (res<0) logoutput("error writing to %i when sending remove watch message", client->fd);

                } else if ( typeaction==WATCH_ACTION_SLEEP ) {

                    res=send_sleepwatch_message(client->fd, watch->id);

                    if (res<0) logoutput("error writing to %i when sending sleep watch message", client->fd);

                } else if ( typeaction==WATCH_ACTION_WAKEUP ) {

                    res=send_wakewatch_message(client->fd, watch->id);

                    if (res<0) logoutput("error writing to %i when sending wakeup watch message", client->fd);

                }

            }

            if ( typeaction==WATCH_ACTION_REMOVE ) {

                /* if remove remove from client */

		res=lock_client(client);

                client->lock=1;

                if ( client->watches==watch ) client->watches=watch->next_per_client;
                if ( watch->prev_per_client ) watch->prev_per_client->next_per_client=watch->next_per_client;
                if ( watch->next_per_client ) watch->next_per_client->prev_per_client=watch->prev_per_client;

                watch->prev_per_client=NULL;
                watch->next_per_client=NULL;
                watch->client=NULL;

                res=unlock_client(client);

            }

        }

        if ( typeaction==WATCH_ACTION_REMOVE ) free(watch);

        watch=next_watch;

    }

    return nreturn;

}

/*
   function to change the state of an effective watch 
   one of the possibilities is of course the removal, but also possible is
   the setting into sleep when the underlying fs is unmounted or
   waking it up when previously is has been set to sleep and the underlying
   fs is mounted
   this setting to sleep and waking up again is typically the case with autofs 
   managed filesystems
   - remove: remove the effective_watch, and the notify backend method
   - sleep: change the state of the effective_watch into sleep, and remove the notify backend method
   - wakeup: change the state of the effective_watch into active, and set the notify backend method
   this function calls change_state_watches to make changes effective to the individual watches

*/

int changestate_effective_watch(struct notifyfs_inode_struct *inode, unsigned char lockset, unsigned char typeaction, char *path)
{
    int nreturn=0;
    int res;

    if ( inode->effective_watch ) {
        struct effective_watch_struct *effective_watch=inode->effective_watch;

        /* try to lock the effective watch */

        res=lock_effective_watch(effective_watch);

        /* after here: the effective watch is locked by this thread */

        res=changestate_watches(effective_watch, typeaction);

        /* correct backend: backend and mount status and specific */

        if ( typeaction==WATCH_ACTION_REMOVE || typeaction==WATCH_ACTION_SLEEP ) {

	    /* when remove or sleep: remove the backend */

	    del_watch_backend(effective_watch);

        } else if ( typeaction==WATCH_ACTION_WAKEUP ) {

            /* when wake up and or set: set the backend */

	    set_watch_backend(effective_watch, effective_watch->mask, 1);

        }

        if ( typeaction==WATCH_ACTION_REMOVE ) {

	    if ( effective_watch->path ) {

		free(effective_watch->path);
		effective_watch->path=NULL;

	    }

            /* remove from global list */

            remove_effective_watch_from_list(effective_watch, 0);

            /* detach from inode */

            inode->effective_watch=NULL;
            effective_watch->inode=NULL;

	    res=unlock_effective_watch(effective_watch);

            /* free and destroy */

            pthread_mutex_destroy(&effective_watch->lock_mutex);
            pthread_cond_destroy(&effective_watch->lock_condition);

            free(effective_watch);

        } else {

	    res=unlock_effective_watch(effective_watch);

	}

    }

    return nreturn;

}

static void changestate_recursive(struct notifyfs_entry_struct *entry, char *path, int pathlen, unsigned char typeaction)
{
    int res, nlen, pathlen_orig=pathlen;

    logoutput("changestate_recursive: entry %s, action %i", entry->name, typeaction);

    if ( entry->child ) {
        struct notifyfs_entry_struct *child_entry=entry->child, *tmp_entry;
        /* check every child */

        while (child_entry) {

	    /* keep the next entry, since child_entry can be removed */

	    tmp_entry=child_entry->dir_next;

            /* here something like copy child_entry->name to path */

            *(path+pathlen)='/';
            pathlen++;
            nlen=strlen(child_entry->name);
            memcpy(path+pathlen, child_entry->name, nlen);
            pathlen+=nlen;

            changestate_recursive(child_entry, path, pathlen, typeaction ); /* recursive */

	    /* reset the path */

	    pathlen=pathlen_orig;
	    *(path+pathlen)='\0';

            child_entry=tmp_entry;

        }

    }

    /* when here the directory is empty */

    if ( typeaction==FSEVENT_INODE_ACTION_REMOVE ) {

	if ( entry->inode ) {

	    if ( entry->inode->effective_watch ) res=changestate_effective_watch(entry->inode, 0, WATCH_ACTION_REMOVE, path);

	    if ( entry->parent && entry->parent->inode ) {

        	/* send invalidate entry */
        	/* use notify_delete or notify_inval_entry ?? */

        	res=fuse_lowlevel_notify_delete(notifyfs_chan, entry->parent->inode->ino, entry->inode->ino, entry->name, strlen(entry->name));

	    }

	    entry->inode->status=FSEVENT_INODE_STATUS_REMOVED;

	}

	if ( entry->parent ) {

            /* remove from child list parent if not detached already */

            if ( entry->parent->child==entry ) entry->parent->child=entry->dir_next;
            if ( entry->dir_prev ) entry->dir_prev->dir_next=entry->dir_next;
            if ( entry->dir_next ) entry->dir_next->dir_prev=entry->dir_prev;

	}

	remove_entry_from_name_hash(entry);
	remove_entry(entry);

    } else if ( typeaction==FSEVENT_INODE_ACTION_SLEEP ) {

	if ( entry->inode ) {

	    if ( entry->inode->effective_watch ) res=changestate_effective_watch(entry->inode, 0, WATCH_ACTION_SLEEP, path);

	    entry->inode->status=FSEVENT_INODE_STATUS_SLEEP;

	}

    } else if ( typeaction==FSEVENT_INODE_ACTION_WAKEUP ) {

	if ( entry->inode ) {

	    if ( entry->inode->effective_watch ) res=changestate_effective_watch(entry->inode, 0, WATCH_ACTION_WAKEUP, path);

	    entry->inode->status=FSEVENT_INODE_STATUS_OK;

	}

    } else if ( typeaction==FSEVENT_INODE_ACTION_CREATE ) {

	/* this will never happen, cause an inode is never created this way */

	if ( entry->inode ) entry->inode->status=FSEVENT_INODE_STATUS_OK;

    }

}

/* function to test queueing a change state entry is necessary 

   note the queue has to be locked 
    TODO: take in account the type of the action, sleep versus remove, what to
    do when both in queue
   */

static unsigned char queue_changestate(struct notifyfs_entry_struct *entry, char *path1)
{
    unsigned char doqueue=0;

    logoutput("queue_changestate: entry %s and path %s", entry->name, path1);

    if ( ! changestate_call_info_first ) {

        /* queue is empty: put it on queue */

        doqueue=1;

    } else {
        struct call_info_struct *call_info_tmp=changestate_call_info_last;
        char *path2;
        int len1=strlen(path1), len2;

        doqueue=1;

        /* walk through queue to check there is a related call already there */

        while(call_info_tmp) {

            path2=call_info_tmp->path;
            len2=strlen(path2);

            if ( len2>len1 ) {

                /* test path2 is a subdirectory of path1 */

                if ( call_info_tmp!=changestate_call_info_first ) {

                    if ( strncmp(path2+len1, "/", 1)==0 && strncmp(path1, path2, len1)==0 ) {

                        /* here replace the already pending entry 2 remove by this one...
                        do this only when it's not the first!!*/

                        call_info_tmp->entry2remove=entry;
                        strcpy(path2, (const char *) path1); /* this possible cause len(path2)>len(path1) HA !!!! */
                        doqueue=0;

                        break;

                    }

                }

            } else {

                /* len2<=len1, test path1 is a subdirectory of path2 */

		if ( strcmp(path1, path2)==0 ) {

		    /* already queued: the same */

		    doqueue=0;

		    break;

		} else if ( strncmp(path1+len2, "/", 1)==0 && strncmp(path2, path1, len2)==0 ) {

                    /* ready: already queued... */
                    doqueue=0;

                    break;

                }

            }

            call_info_tmp=call_info_tmp->prev;

        }

    }

    return doqueue;

}

/* wait for the call_info to be the first in remove entry queue, and process when it's the first 
  the lock has to be set
*/

static void wait_in_queue_and_process(struct call_info_struct *call_info, unsigned char typeaction)
{
    int res;
    pathstring path;

    logoutput("wait_in_queue_and_process: action %i, path %s", typeaction, call_info->path);

    if ( call_info != changestate_call_info_first ) {

        /* wait till it's the first */

        while ( call_info != changestate_call_info_first ) {

            res=pthread_cond_wait(&changestate_condition, &changestate_mutex);

        }

    }

    /* call_info is the first in the queue, release the queue for other threads and go to work */

    res=pthread_mutex_unlock(&changestate_mutex);

    memset(path, '\0', sizeof(pathstring));
    strcpy(path, call_info->path);

    changestate_recursive(call_info->entry2remove, path, strlen(path), typeaction);

    /* remove from queue... */

    res=pthread_mutex_lock(&changestate_mutex);

    changestate_call_info_first=call_info->next;

    if ( changestate_call_info_first ) changestate_call_info_first->prev=NULL;

    call_info->next=NULL;
    call_info->prev=NULL;

    if ( changestate_call_info_last==call_info ) changestate_call_info_last=changestate_call_info_first;

    /* signal other threads if the queue is not empty */

    if ( changestate_call_info_first ) res=pthread_cond_broadcast(&changestate_condition);

}

/* important function to react on signals which imply that the state of entries and watches 
   has changed

   this is called by the fuse threads, but also from the mainloop, which reacts on inotify events,
   and mountinfo when mounts are removed or added

   to do so a queue is used
   every call is added to the tail, and when it has become the first, it's processed
   every time the first call is removed, and thus the next call has become the first, 
   a broadcast is done to the other waiting threads
   the one who has become first is taking action

   very nice of this queue is the ability to check other pending calls
   when there is a related request present in the queue, it's not necessary to queue this request again
   when in a tree different watches are set, and the whole tree is removed this will result in a burst
   of inotify events: this queue will filter out doubles
   */

void changestate(struct call_info_struct *call_info, unsigned char typeaction)
{
    int res;
    unsigned char doqueue=1;

    call_info->entry2remove=call_info->entry;
    call_info->next=NULL;
    call_info->prev=NULL;

    if ( call_info->entry ) {

	if ( call_info->entry->name ) {

    	    logoutput("changestate: processing %s, entry %s", call_info->path, call_info->entry->name);

	} else {

	    logoutput("changestate: processing %s, entry **NO NAME**", call_info->path);

	}

    } else {

        logoutput("changestate: processing %s, entry **NOT SET**", call_info->path);

    }

    /* lock the remove entry queue */

    res=pthread_mutex_lock(&changestate_mutex);

    doqueue=queue_changestate(call_info->entry, call_info->path);

    if ( doqueue==1 ) {

        if ( ! changestate_call_info_first ) {

            /* queue is empty: put it on queue */

            changestate_call_info_first=call_info;
            changestate_call_info_last=call_info;

        } else {

            /* add at tail of queue */

            changestate_call_info_last->next=call_info;
            call_info->prev=changestate_call_info_last;
            call_info->next=NULL;
            changestate_call_info_last=call_info;

        }

	if ( typeaction==FSEVENT_ACTION_TREE_UP ) {

    	    wait_in_queue_and_process(call_info, FSEVENT_INODE_ACTION_WAKEUP);

	} else if ( typeaction==FSEVENT_ACTION_TREE_REMOVE ) {

	    if ( call_info->mount_entry ) {

		if ( mounted_by_autofs(call_info->mount_entry)==1 ) {

		    wait_in_queue_and_process(call_info, FSEVENT_INODE_ACTION_SLEEP);

		} else {

		    wait_in_queue_and_process(call_info, FSEVENT_INODE_ACTION_REMOVE);

		}

	    } else {

		logoutput("changestate: cannot go futher, mount entry not set for %s", call_info->path);

	    }

	}

    }

    res=pthread_mutex_unlock(&changestate_mutex);

}

/* function which creates a path in notifyfs 

    it does this by testing every subpath and create that in notifyfs 

    */

struct notifyfs_entry_struct *create_notifyfs_path(char *path2create)
{
    char *path, *slash, *name;
    struct notifyfs_inode_struct *pinode; 
    struct notifyfs_entry_struct *pentry, *entry=NULL;
    unsigned char fullpath=0;
    int res;
    pathstring tmppath;
    struct stat st;

    pentry=get_rootentry();
    pinode=pentry->inode;

    strcpy(tmppath, path2create);
    path=tmppath;

    logoutput("create_notifyfs_path: creating %s", tmppath);

    /*  translate path into entry 
        suppose here safe that entry is a subdir of root entry...*/


    while(1) {

        /*  walk through path from begin to end and 
            check every part */

        slash=strchr(path, '/');

        if ( slash==tmppath ) {

            /* ignore the starting slash*/

            path++;

            /* if nothing more (==only a slash) stop here */

            if (strlen(slash)==0) {

        	entry=pentry;
        	break;

	    }

            continue;

        }

        if ( ! slash ) {

            fullpath=1;

        } else {

            *slash='\0';

        }

        name=path;

        if ( name ) {

            entry=find_entry(pinode->ino, name);

            if ( ! entry ) {

        	/* check the stat */

        	res=lstat(tmppath, &st);

        	if ( res==-1 ) {

            	    /* what to do here?? the path does not exist */
            	    entry=NULL;
            	    goto out;

        	}

                entry=create_entry(pentry, name, NULL);

                if (entry) {

                    assign_inode(entry);

		    if ( entry->inode ) {

                	/*  is there a function to signal kernel an inode and entry are created 
                    	    like fuse_lowlevel_notify_new_inode */

                	add_to_inode_hash_table(entry->inode);
                	add_to_name_hash_table(entry);

			/* maintain the directory tree */

                	if ( entry->parent ) {

                    	    entry->dir_next=NULL;
                    	    entry->dir_prev=NULL;

                    	    if ( entry->parent->child ) {

                        	entry->parent->child->dir_prev=entry;
                        	entry->dir_next=entry->parent->child;

                    	    }

                    	    entry->parent->child=entry;

                	}

			update_timespec(&entry->inode->laststat);
                	copy_stat(&(entry->inode->st), &st);

		    } else {

			remove_entry(entry);
			entry=NULL;

		    }

                } else {

                    entry=NULL;
                    goto out;

                }

            }


        }

        if ( fullpath==1 ) {

            break;

        } else {

            /* make slash a slash again (was turned into a \0) */
            *slash='/';
            path=slash+1;
            pentry=entry;
            pinode=entry->inode;

        }

    }

    out:

    return entry;

}

void create_notifyfs_mount_path(struct mount_entry_struct *mount_entry)
{
    struct notifyfs_entry_struct *entry;

    entry=create_notifyfs_path(mount_entry->mountpoint);

    if ( entry ) {

	mount_entry->data0=(void *) entry;
	mount_entry->typedata0=NOTIFYFS_MOUNTDATA_ENTRY;

	entry->mount_entry=mount_entry;

	mount_entry->status=MOUNT_STATUS_UP;

    } else {

	/* somehow failed to create the path */

	logoutput("create_notifyfs_mount_path: unable to create mount point %s", mount_entry->mountpoint);

	mount_entry->status=MOUNT_STATUS_NOTSET;

    }

}


/* send clients which are interested about mounts a mount message
   called by update_notifyfs when a mount is removed or added */

static void send_mount_message_to_clients(struct mount_entry_struct *mount_entry)
{
    struct client_struct *client;
    int res, nummessages=0;

    if ( strcmp(mount_entry->mountpoint, notifyfs_options.mountpoint)==0 ) {

	/* do not send when self mounted or umounted */

	return;

    }

    res=lock_clientslist();

    client=get_clientslist();

    while(client) {

	/* here some conddition to not send to all clients, only those who interested.... */

	if ( client->status_app==NOTIFYFS_CLIENTSTATUS_UP ) {

	    /* only send when client is up */

	    if ( client->messagemask & NOTIFYFS_MESSAGE_MASK_MOUNT ) {

		/* only send when client is interested in mount messages */

		logoutput("send_mount_message_to_clients: send to client pid %i", client->pid);

		res=send_mount_message(client->fd, mount_entry, 0);

		nummessages++;

	    }

	}

	client=client->next;

    }

    res=unlock_clientslist();

    logoutput("send_mount_message_to_clients: %i messages send for %s", nummessages, mount_entry->mountpoint);

}

/* function to get the value of field from options */

void get_value_from_options(char *options, const char *option, char *value, int len)
{
    char *poption;

    if ( ! options || ! option ) return;

    poption=strstr(options, option);

    if ( poption ) {
	char *endoption=strchrnul(poption, ',');
	char *issign=strchr(poption, '=');

	if ( issign ) {

	    if ( issign<endoption && endoption-issign < len ) {

		memcpy(value, issign+1, endoption-issign-1);

	    }

	}

    }

}

/* function to get remote host and information about the directory on that remote host from the mountsource 
   with sshfs this is simple
   it gets more difficult with cifs, mountsource is in netbios format
   and with ...*/

int determine_remotehost(struct mount_entry_struct *mount_entry, char *user, int len1, char *host, int len2, char *path, int len3)
{
    int nreturn=0;
    char *fstype=mount_entry->fstype;
    char *mountsource=mount_entry->mountsource;
    char *options=mount_entry->superoptions;

    memset(user, '\0', len1);
    memset(host, '\0', len2);
    memset(path, '\0', len3);


    if ( strcmp(fstype, "fuse.sshfs")==0 ) {
	int len=strlen(mountsource);

	/* remote host is in mountsource */
	/* sshfs uses the user@xxx.xxx.xxx.xxx:/path format when in ipv4
           user is optional, which defaults to the user making the creating 
           path is also optional, it defaults to the home directory of the user on the remote host*/

	if (len>0 ) {
	    char *separator1, *separator2;

	    /* look for user part */

	    separator1=strchr(mountsource, '@');

	    if ( separator1 ) {

		/* there is a user part */

		if ( separator1-mountsource < len1 ) {

		    memcpy(user, mountsource, separator1-mountsource);

		} else {

		    /* not enough room */

		    nreturn=-ENOMEM;
		    goto out;

		}

		separator1++;

	    } else {

		separator1=mountsource;

	    }

	    /* look for host part (there must be a : )*/

	    separator2=strchr(separator1, ':');

	    if ( separator2 ) {
		int len;

		if ( separator2-separator1<len2 ) {

		    memcpy(host, separator1, separator2-separator1);

		} else {

		    /* not enough room */

		    nreturn=-ENOMEM;
		    goto out;

		}

		len=strlen(separator2+1);

		if ( len>0 ) {

		    /* there is a path part */

		    if ( len<len3 ) {

			memcpy(path, separator2+1, len);

		    } else {

			/* not enough room */

			nreturn=-ENOMEM;
			goto out;

		    }

		}

	    } else {

		/* error: no ":" separator */

		nreturn=-EINVAL;
		goto out;

	    }

	}

	/* if no user part: get it from options */

	if ( strlen(user)==0 ) {

	    get_value_from_options(options, "user_id", user, len1);

	}

    } else if ( strcmp(fstype, "cifs")==0 ) {
	char *separator=strrchr(mountsource, '/');

	if ( separator ) strcpy(path, separator+1);

	/* remote "directory" is not set, but the share in SMB world. store this in stead of path */

	/* remote host is in options*/
	/* cifs stores the remote host in field addr= in options 
           and the user in username */

	get_value_from_options(options, "addr", host, len2);
	get_value_from_options(options, "username", user, len1);

    }


    out:

    return nreturn;

}

/* isn't there a better way to deal with this?? it is already known that 
   there is a program on this machine is running which uses a certain service and is a mounted network filesystem 
   for now I do it this way.....
   and maybe fuse.sshfs uses a different port....ugh*/

typedef struct { const char *fstype; const char *service; } fsservicemap;

const fsservicemap supportedfs[] = {{ "fuse.sshfs", "22"}};


char *filesystem_is_supported_networkfs(struct mount_entry_struct *mount_entry)
{
    char *service=NULL;
    int i;

    for (i=0;i<(sizeof(supportedfs)/sizeof(supportedfs[0]));i++) {

	if ( strcmp(mount_entry->fstype, supportedfs[i].fstype)==0 ) {

	    service=(char *)supportedfs[i].service;
	    break;

	}

    }

    return service;

}

/* function to determine the backend of a mountpoint, if there is any */

void determine_backend_mountpoint(struct mount_entry_struct *mount_entry)
{

    /* clientfs backend of mount entry is in field data1, check it's possible to forward to fs */

    if ( mount_entry->typedata1==NOTIFYFS_MOUNTDATA_NOTSET ) {

	/* test the fs can receive and forward */

	if ( find_and_assign_clientfs_to_mount(mount_entry)==1 ) {

	    logoutput("determine_backend_mountpoint: client fs found, set as backend");
	    return;

	}

    }

    /* other notifyfs servers are in field data2, check it's possible and required to communicate with this server */

    if ( mount_entry->typedata2==NOTIFYFS_MOUNTDATA_NOTSET && notifyfs_options.forwardovernetwork==1 ) {
	char *ipv4address=NULL;
	char path[PATH_MAX];
	char host[64]; /* guess this is enough */
	char user[32];
	int res;

	if ( strcmp(mount_entry->fstype, "fuse.sshfs")==0 ) {

	    /* extract info from the mountsource, fstype and options */

	    res=determine_remotehost(mount_entry, user, 32, host, 64, path, PATH_MAX);

	    if (res==0 ) {

		/* determine the ipv4 address of the remote host, givven the hostid in the mountsource, and the port 22 
                   note that fuse.sshfs may use another port, this is a TODO*/

		ipv4address=get_ipv4address(host, "22");

	    }

	} else if ( strcmp(mount_entry->fstype, "cifs")==0 ) {

	    /* extract info from the mountsource, fstype and options */

	    res=determine_remotehost(mount_entry, user, 32, host, 64, path, PATH_MAX);

	    if (res==0 ) {

		/* determine the ipv4 address of the remote host, givven the hostid in the mountsource, and the port 22 
                   note that fuse.sshfs may use another port, this is a TODO*/

		ipv4address=get_ipv4address(host, "445");

	    }

	}

	if ( ipv4address ) {
	    struct notifyfsserver_struct *notifyfsserver=NULL;

	    logoutput("determine_backend_mountpoint: got ipv4 %s, user %s, path %s", ipv4address, user, path);

	    /* TODO: check a notifyfsserver with this address is already connected... 
               otherwise try to connect to it (carefull: do not try to connect when once tried over and over every new mount */

	    notifyfsserver=lookup_notifyfsserver_peripv4(ipv4address, 0);

	    if ( notifyfsserver ) {

		/* server found */

		mount_entry->data2=(void *) notifyfsserver;
		mount_entry->typedata2=NOTIFYFS_MOUNTDATA_REMOTESERVER;

	    } else {

		/* here something like try to connect to remote host */

		logoutput("determine_backend_mountpoint: no remote notifyfsserver found for %s, trying to connect", ipv4address);

		res=connect_to_remote_notifyfsserver(ipv4address, notifyfs_options.networkport);

		/* if successfull..register the notifyfsserver...and send a register message */

	    }

	} else {

	    logoutput("determine_backend_mountpoint: unable to get ipv4 from %s", mount_entry->mountsource);

	}

    }

}


/* function to update notifyfs when mounts are added and/or removed 
   some observations:
   - not every mountpoint (added or removed) is used by this fs
   - in case of an fs mounted by fs, it's possible that watches are already set on this fs has been
   set to "sleep" mode after is has been umounted before
   -   
   - note::: added and removed mounts are already sorted in the same way the main sort module works 
   in mountinfo.c:
   (mountpoint, mountsource, fstype)
   */

void update_notifyfs(unsigned char firstrun)
{
    struct mount_entry_struct *mount_entry=NULL;
    struct notifyfs_entry_struct *entry;
    struct client_struct *client;
    int nreturn=0, res;

    logoutput("update_notifyfs");

    /* log based on condition */

    logoutput_list(MOUNTENTRY_ADDED, 0);
    logoutput_list(MOUNTENTRY_REMOVED, 0);
    logoutput_list(MOUNTENTRY_REMOVED_KEEP, 0);

    /* walk through removed mounts to see it affects the fs */

    res=lock_mountlist();

    /* start with last: the list is sorted, bigger/longer (also submounts) mounts are first this way */

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_REMOVED);

    while (mount_entry) {


        entry=(struct notifyfs_entry_struct *) mount_entry->data0;

        /* if normal umount then remove the tree... 
               if umounted by autofs then set tree to sleep */

        if ( entry ) {
            struct call_info_struct call_info;

	    init_call_info(&call_info, entry);

            call_info.path=mount_entry->mountpoint;
	    call_info.mount_entry=mount_entry;

            /* normal umount: remove whole tree */

            changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE );

        }

	/* if a client fs is attached: set down */

	client=(struct client_struct *) mount_entry->data1;

	if ( client ) {

	    res=lock_client(client);

	    if ( client->status_fs!=NOTIFYFS_CLIENTSTATUS_DOWN ) {

		/* if not mounted anymore set the client down */

		logoutput("set mount %s and attached client %i down", mount_entry->mountpoint, client->pid);

		client->status_fs=NOTIFYFS_CLIENTSTATUS_DOWN;

	    }

	    res=unlock_client(client);

	}

	/* send message to inform clients.. not on startup */

	if ( firstrun==0 ) send_mount_message_to_clients(mount_entry);

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_REMOVED);

    }

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_REMOVED_KEEP);

    while (mount_entry) {

	if ( mount_entry->processed==1 ) {

	    mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_REMOVED_KEEP);
	    continue;

	}

        entry=(struct notifyfs_entry_struct *) mount_entry->data0;

        /* if normal umount then remove the tree... 
               if umounted by autofs then set tree to sleep */

        if ( entry ) {
            struct call_info_struct call_info;

	    init_call_info(&call_info, entry);

            call_info.path=mount_entry->mountpoint;
	    call_info.mount_entry=mount_entry;

            changestate(&call_info, FSEVENT_ACTION_TREE_REMOVE);

        }

	mount_entry->processed=1;

	client=(struct client_struct *) mount_entry->data1;

	/* if a client fs is attached, set it to sleep */

	if ( client ) {

	    res=lock_client(client);

	    if ( client->status_fs!=NOTIFYFS_CLIENTSTATUS_SLEEP ) {

		logoutput("set mount %s and attached client %i to sleep", mount_entry->mountpoint, client->pid);

		client->status_fs=NOTIFYFS_CLIENTSTATUS_SLEEP;

	    }

	    res=unlock_client(client);

	}

	/* send message to inform clients.. not on startup... TODO */

	if ( firstrun==0 ) send_mount_message_to_clients(mount_entry);

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_REMOVED_KEEP);

    }

    /* in any case the watches set on an autofs managed fs, get out of "sleep" mode */

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_ADDED);

    while (mount_entry) {


	if ( is_rootmount(mount_entry) ) {

	    /* link rootmount and rootentry */

	    if ( ! mount_entry->data0 ) {

		/* not already assigned */

		entry=get_rootentry();

		mount_entry->data0=(void *) entry;
		entry->mount_entry=mount_entry;

	    }

	    /* root is already created */

	    goto next;

	}

	/* match a client fs if there is one, not at init(TODO) */

	determine_backend_mountpoint(mount_entry);

        entry=(struct notifyfs_entry_struct *) mount_entry->data0;

        if ( entry ) {
            struct call_info_struct call_info;

            /* entry does exist already, there is a tree already related to this mount */
            /* walk through tree and wake everything up */

	    init_call_info(&call_info, entry);

            call_info.path=mount_entry->mountpoint;
	    call_info.mount_entry=mount_entry;

            changestate(&call_info, FSEVENT_ACTION_TREE_UP);

        } else {

	    if ( strcmp(mount_entry->mountpoint, "/") == 0 ) {

		/* skip the root... not necessary to create....*/
		goto next;

	    } else if ( strcmp(notifyfs_options.mountpoint, mount_entry->mountpoint)==0 ) {

		/* skip the mountpoint of this fs, caused deadlocks in the past... 
		   this will be not the case anymore since the threads for handling the fuse
		   events are up and running independent of this thread... needs testing */

		goto next;

	    }

            create_notifyfs_mount_path(mount_entry);

        }

	/* send message to inform clients.. not on startup */

	if ( firstrun==0 ) send_mount_message_to_clients(mount_entry);

	next:

        mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_ADDED);

    }

    unlock:

    res=unlock_mountlist();

}

