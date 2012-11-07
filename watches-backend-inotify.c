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

#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUFF_LEN (1024 * (INOTIFY_EVENT_SIZE + 16))

extern struct notifyfs_options_struct notifyfs_options;
extern void changestate(struct call_info_struct *call_info, unsigned char typeaction);
int inotify_fd=0;
struct epoll_extended_data_struct xdata_inotify;

typedef struct INTEXTMAP {
                const char *name;
                unsigned int mask;
                } INTEXTMAP;

static const INTEXTMAP inotify_textmap[] = {
            { "IN_ACCESS", IN_ACCESS},
            { "IN_MODIFY", IN_MODIFY},
            { "IN_ATTRIB", IN_ATTRIB},
            { "IN_CLOSE_WRITE", IN_CLOSE_WRITE},
            { "IN_CLOSE_NOWRITE", IN_CLOSE_NOWRITE},
            { "IN_OPEN", IN_OPEN},
            { "IN_MOVED_FROM", IN_MOVED_FROM},
            { "IN_MOVED_TO", IN_MOVED_TO},
            { "IN_CREATE", IN_CREATE},
            { "IN_DELETE", IN_DELETE},
            { "IN_DELETE_SELF", IN_DELETE_SELF},
            { "IN_MOVE_SELF", IN_MOVE_SELF},
            { "IN_ONLYDIR", IN_ONLYDIR},
            { "IN_DONT_FOLLOW", IN_DONT_FOLLOW},
#ifdef IN_EXCL_UNLINK
            { "IN_EXCL_UNLINK", IN_EXCL_UNLINK},
#endif
            { "IN_MASK_ADD", IN_MASK_ADD},
            { "IN_ISDIR", IN_ISDIR},
            { "IN_Q_OVERFLOW", IN_Q_OVERFLOW},
            { "IN_UNMOUNT", IN_UNMOUNT}};


int print_mask(unsigned int mask, char *string, size_t size)
{
    int i, pos=0, len;

    for (i=0;i<(sizeof(inotify_textmap)/sizeof(inotify_textmap[0]));i++) {

        if ( inotify_textmap[i].mask & mask ) {

            len=strlen(inotify_textmap[i].name);

            if ( pos + len + 1  > size ) {

                pos=-1;
                goto out;

            } else {

                if ( pos>0 ) {

                    *(string+pos)='|';
                    pos++;

                }

                strcpy(string+pos, inotify_textmap[i].name);
                pos+=len;

            }

        }

    }

    out:

    return pos;

}

/* function which set a os specific watch on the backend on path with mask mask

    NOTE:
    20121017 notifyfs uses internally the inotify format to describe a watch and event
    this will change since the inotify format has some shortcomings, like describing
    the extended attributes
    so the internal format will change, so a translation from the internal notifyfs format to inotify format
    is required here in future

*/

void set_watch_backend_inotify(struct effective_watch_struct *effective_watch, char *path, int mask)
{
    int res;

    logoutput("set_watch_backend_inotify: call inotify_add_watch on fd %i, path %s and mask %i", notifyfs_options.inotify_fd, path, mask);

    res=inotify_add_watch(notifyfs_options.inotify_fd, path, mask);

    if ( res==-1 ) {

        logoutput("set_watch_backend_inotify: setting inotify watch gives error: %i", errno);

    } else {

	if ( effective_watch->inotify_id>0 ) {

	    /*	when inotify_add_watch is called on a path where a watch has already been set, 
		the watch id should be the same, it's an update... 
		only log when this is not the case */

	    if ( res != effective_watch->inotify_id ) {

		logoutput("set_watch_backend_inotify: warning: inotify watch returns a different id: %i versus %li", res, effective_watch->inotify_id);

	    }

	}

	logoutput("set_watch_backend_inotify: got inotify id %i", res);

        effective_watch->inotify_id=(unsigned long) res;

    }

}

void change_watch_backend_inotify(struct effective_watch_struct *effective_watch, char *path, int mask)
{

    /* with inotify the changing of an existing is the same call as the adding of a new watch */

    set_watch_backend_inotify(effective_watch, path, mask);

}


void remove_watch_backend_inotify(struct effective_watch_struct *effective_watch)
{
    int res;

    logoutput("remove_watch_backend_inotify");

    if ( effective_watch->inotify_id>0 ) {

	res=inotify_rm_watch(notifyfs_options.inotify_fd, (int) effective_watch->inotify_id);

	if ( res==-1 ) {

    	    logoutput("remove_watch_backend_inotify: deleting inotify watch %li gives error: %i", effective_watch->backend_id, errno);

	} else {

    	    effective_watch->inotify_id=0;

	}

    }

}

/* function to translate an event reported by
   inotify
*/

struct notifyfs_fsevent_struct *evaluate_fsevent_inotify(struct inotify_event *i_event)
{
    int res;
    struct effective_watch_struct *effective_watch=NULL;
    struct notifyfs_fsevent_struct *notifyfs_fsevent=NULL;

    /* lookup watch using this wd */

    logoutput("evaluate_fsevent_inotify: received an inotify event on wd %i, mask %i", i_event->wd, i_event->mask);

    effective_watch=lookup_watch(FSEVENT_BACKEND_METHOD_INOTIFY, i_event->wd);

    if (! effective_watch) {

	logoutput("evaluate_fsevent_inotify: error: inotify watch %i not found", i_event->wd);
	goto out;

    }

    if ( ! (i_event->mask && (IN_ACCESS | IN_ATTRIB | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_CREATE | IN_DELETE |
		    IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_OPEN))) {

	logoutput("evaluate_fsevent_inotify: received an inotify event on wd %i.", i_event->wd);

	goto out;

    }

    notifyfs_fsevent=malloc(sizeof(struct notifyfs_fsevent_struct));

    if (! notifyfs_fsevent) {

	logoutput("evaluate_fsevent_inotify: unable to allocate memory");

	goto out;

    }

    notifyfs_fsevent->group=NOTIFYFS_FSEVENT_NOTSET;
    notifyfs_fsevent->type=0;

    if (i_event->name) {

        /* something happens on an entry in the directory.. check it's in use
        by this fs, the find command will return NULL if it isn't 
	    (do nothing with close, open and access) */

	entry=find_entry(effective_watch->inode->ino, i_event->name);

	if (!entry) {

	    if ( i_event->mask & ( IN_ACCESS | IN_ATTRIB | IN_CLOSE_WRITE |
		IN_CLOSE_NOWRITE | IN_CREATE | IN_MODIFY | IN_MOVED_TO | IN_OPEN)) {

		entry=create_entry_from_event(effective_watch, notifyfs_fsevent);
		entry=create_entry(effective_watch->inode->alias, notifyfs_fsevent->name, NULL);

		if (entry) {

		    assign_inode(entry);

		    if (entry->inode) {
			struct stat *st=&entry->inode->st;

			st->st_ino=entry->inode->ino;

			st->st_mode=0; /* mode zero means not synced with backend */
			st->st_dev=0;
			st->st_nlink=0;
			st->st_uid=0;
			st->st_gid=0;
			st->st_rdev=0;
			st->st_size=0;
			st->st_blksize=0;
			st->st_blocks=0;

			st->st_atime=0;
			st->st_mtime=0;
			st->st_ctime=0;

#ifdef  __USE_MISC

			/* time defined as timespec */

			st->tv_nsec=0;
			st->tv_nsec=0;
			st->tv_nsec=0;

#else

			/* n sec defined as extra field */

			st->st_atimensec=0;
			st->st_mtimensec=0;
			st->st_ctimensec=0;

#endif


			add_to_name_hash_table(entry);
			add_to_inode_hash_table(entry->inode);

			add_entry_to_dir(entry);

		    } else {

			remove_entry(entry);
			entry=NULL;

			logoutput("evaluate_fsevent_inotify: unable to create entry");

			goto out;

		    }

		} else {

		    logoutput("evaluate_fsevent_inotify: unable to create entry");

		    goto out;

		}

	    }

	}

    } else {

	/*  event on watch self
	    do nothing with close, open and access*/

	entry=effective_watch->inode->alias;

    }

    notifyfs_fsevent->entry=entry;

    if ( i_event->mask & IN_ACCESS ) {

	/* file is read */

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_FILE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_FILE_READ;

	i_event->mask-=IN_ACCESS;

	goto out;

    }

    if ( i_event->mask & IN_OPEN ) {

	/* file is open */

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_FILE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_FILE_OPEN;

	i_event->mask-=IN_OPEN;

	goto out;

    }

    if ( i_event->mask & IN_CLOSE_WRITE ) {

	/* file is closed after write */

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_FILE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_FILE_CLOSE_WRITE;

	i_event->mask-=IN_CLOSE_WRITE;

	goto out;

    }

    if ( i_event->mask & IN_CLOSE_NOWRITE ) {

	/* file is closed with no write */

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_FILE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_FILE_CLOSE_NOWRITE;

	i_event->mask-=IN_CLOSE_NOWRITE;

	goto out;

    }

    if ( i_event->mask & IN_MODIFY ) {

	/* file is modified */

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_FILE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_FILE_FILE;

	i_event->mask-=IN_MODIFY;

	goto out;

    }

    if ( i_event->mask & IN_ATTRIB ) {
	struct call_info_struct call_info;
	struct stat st;

	/* test out what it's 
	    three possibities: 
	    mode, owner, group: META_ATTRIB
	    size: FILE
	    xattr: META_XATTR
	*/

	/* stat not defined: get it here */

    	init_call_info(&call_info, entry);

    	res=determine_path(&call_info, NOTIFYFS_PATH_FORCE);
    	if (res<0) goto out;

	res=lstat(call_info.path, &st);

	if ( res==-1 ) {

	    /* strange case: message that entry does exist, but stat gives error... */

	    mask-=IN_ATTRIB;

	    if ( i_event->name ) {

		mask|=IN_DELETE;

	    } else {

		mask|=IN_DELETE_SELF;

	    }

	} else {
	    struct stat *cached_st=&(entry->inode->st);

	    if (st->st_mode==0) {

		/* stat not yet set in notifyfs, sp nothing to compare... what now ?? 
		    leave it for to be a meta 
		    it can also be a file change....*/

		notifyfs_fsevent->group=NOTIFYFS_FSEVENT_META;
		notifyfs_fsevent->type=NOTIFYFS_FSEVENT_META_NOTSET;

		copy_stat(cached_st, &st);

	    } else {

		/* mode, owner and group belong to group META */

		if (cached_st->st_mode!=st->st_mode || cached_st->st_uid!=st->st_uid || cached_st->st_gid!=st->st_gid) {

		    notifyfs_fsevent->group=NOTIFYFS_FSEVENT_META;

		    if (cached_st->st_mode!=st->st_mode) {

			notifyfs_fsevent->type|=NOTIFYFS_FSEVENT_META_ATTRIB_MODE;
			cached_st->st_mode=st->st_mode;

		    }

		    if (cached_st->st_uid!=st->st_uid) {

			notifyfs_fsevent->type|=NOTIFYFS_FSEVENT_META_ATTRIB_OWNER;
			cached_st->st_uid=st->st_uid;

		    }

		    if (cached_st->st_gid!=st->st_gid) {

			notifyfs_fsevent->type|=NOTIFYFS_FSEVENT_META_ATTRIB_GROUP;
			cached_st->st_gid!=st->st_gid;

		    }

		    i_event->mask-=IN_ATTRIB;

		    goto out

		}

		/* nlinks belongs to group FS */

		if (cached_st->st_nlink!=st->st_nlink) {

		    notifyfs_fsevent->group=NOTIFYFS_FSEVENT_FS;
		    notifyfs_fsevent->type|=NOTIFYFS_FSEVENT_FS_NLINKS;

		    cached_st->st_nlink=st->st_nlink;

		    i_event->mask-=IN_ATTRIB;

		    goto out;

		}

		/* size belongs to group FILE */

		if (cached_st->st_size!=st->st_size) {

		    notifyfs_fsevent->group=NOTIFYFS_FSEVENT_FILE;
		    notifyfs_fsevent->type|=NOTIFYFS_FSEVENT_FILE_SIZE;

		    cached_st->st_size=st->st_size;

		    i_event->mask-=IN_ATTRIB;

		    goto out;

		}

		/* when here the change must be in xattr */

		logoutput("evaluate_fsevent_inotify: IN_ATTRIB: probably xattr, but no test here yet...");

		notifyfs_fsevent->group=NOTIFYFS_FSEVENT_META;
		notifyfs_fsevent->type|=NOTIFYFS_FSEVENT_META_XATTR_NOTSET;

		i_event->mask-=IN_ATTRIB;

		goto out;

	    }

	}

    }

    if ( i_event->mask & IN_DELETE_SELF ) {

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_MOVE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_MOVE_DELETED;

	i_event->mask-=IN_DELETE_SELF;

	goto out;

    }

    if ( i_event->mask & IN_DELETE ) {

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_MOVE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_MOVE_DELETED;

	i_event->mask-=IN_DELETE;

	goto out;

    }

    if ( i_event->mask & IN_MOVE_SELF ) {

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_MOVE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_MOVE_MOVED;

	i_event->mask-=IN_MOVE_SELF;

	goto out;

    }

    /* do something with the cookie provided by inotify? */

    if ( i_event->mask & IN_MOVED_FROM ) {

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_MOVE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_MOVE_MOVED_FROM;

	i_event->mask-=IN_MOVED_FROM;

	goto out;

    }

    if ( i_event->mask & IN_MOVED_TO ) {

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_MOVE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_MOVE_DELETED;

	i_event->mask-=IN_DELETE_SELF;

	goto out;

    }

    if ( i_event->mask & IN_CREATE ) {

	notifyfs_fsevent->group=NOTIFYFS_FSEVENT_MOVE;
	notifyfs_fsevent->type=NOTIFYFS_FSEVENT_MOVE_CREATED;

	i_event->mask-=IN_CREATE;

	goto out;

    }

    out:

    return notifyfs_fsevent;

}

/* INOTIFY BACKEND SPECIFIC CALLS */

void handle_data_on_inotify_fd(int fd, uint32_t events, int signo)
{
    int nreturn=0;
    char outputstring[256];

    logoutput("handle_data_on_inotify_fd");

    if ( events & EPOLLIN ) {
        int lenread=0;
        char buff[INOTIFY_BUFF_LEN];

        lenread=read(fd, buff, INOTIFY_BUFF_LEN);

        if ( lenread<0 ) {

            logoutput("handle_data_on_inotify_fd: error (%i) reading inotify events (fd: %i)", errno, fd);

        } else {
            int i=0, res;
            struct inotify_event *i_event=NULL;
	    struct notifyfs_fsevent_struct *notifyfs_event=NULL;

            while(i<lenread) {

                i_event = (struct inotify_event *) &buff[i];

                /* handle overflow here */

                if ( (i_event->mask & IN_Q_OVERFLOW) && i_event->wd==-1 ) {

                    /* what to do here: read again?? go back ??*/

                    logoutput("handle_data_on_inotify_fd: error reading inotify events: buffer overflow.");
                    goto next;

                }

		while (i_event->mask>0) {
		    unint32_t oldmask=i_event->mask;

		    /* translate the inotify event in a general notifyfs fs event */

		    notifyfs_fsevent=evaluate_fsevent_inotify(i_event);

		    if (! notifyfs_event) {

			break;

		    } else {

			/* process the event here futher: put it on a queue to be processed by a special thread */

			logoutput("handle_data_on_inotify_fd: received notifyfs_fsevent");

			process_notifyfs_fsevent(notifyfs_fsevent);

		    }

		    /* prevent an endless loop */

		    if (i_event->mask==oldmask) break;

		}

		next:

                i += INOTIFY_EVENT_SIZE + i_event->len;

            }

        }

    }

}

void initialize_inotify()
{
    struct epoll_extended_data_struct *epoll_xdata;

    /*
    *    add a inotify instance to epoll : default backend 
    *
    */

    /* create the inotify instance */

    inotify_fd=inotify_init();

    if ( inotify_fd<=0 ) {

        logoutput("Error creating inotify fd: %i.", errno);
        goto out;

    }

    /* add inotify to the main eventloop */

    epoll_xdata=add_to_epoll(inotify_fd, EPOLLIN | EPOLLPRI, TYPE_FD_INOTIFY, &handle_data_on_inotify_fd, NULL, &xdata_inotify, NULL);

    if ( ! epoll_xdata ) {

        logoutput("error adding inotify fd to mainloop.");
        goto out;

    } else {

        logoutput("inotify fd %i added to epoll", inotify_fd);

	add_xdata_to_list(epoll_xdata);

    }

    notifyfs_options.inotify_fd=inotify_fd;

}

void close_inotify()

    if ( xdata_inotify.fd>0 ) {

	res=remove_xdata_from_epoll(&xdata_inotify, 0);
	close(xdata_inotify.fd);
	xdata_inotify.fd=0;
	notifyfs_options.inotify_fd=0;
	remove_xdata_from_list(&xdata_inotify, 0, NULL);

    }

}

