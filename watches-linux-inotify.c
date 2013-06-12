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

#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUFF_LEN (1024 * (INOTIFY_EVENT_SIZE + 16))

#define INOTIFY_WATCH_LOOKUP_WD			1
#define INOTIFY_WATCH_LOOKUP_WATCH		2

#define NOTIFYFS_WATCHMODE_INOTIFY		4

extern struct notifyfs_options_struct notifyfs_options;

static int inotify_fd=0;
static struct epoll_extended_data_struct xdata_inotify;

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

/* lookup watch using wd */

struct watch_struct *lookup_watch_inotify_wd(int wd)
{
    struct watch_struct *watch=NULL;

    /* lookup using the wd */

    pthread_mutex_lock(&watchctr_mutex);

    watch=watch_list;

    while(watch) {

	if (watch->mode &(NOTIFYFS_WATCHMODE_INOTIFY | NOTIFYFS_WATCHMODE_OSSPECIFIC)) {

	    if (watch->backend.wd==wd) break;

	}

	watch=watch->next;

    }

    pthread_mutex_unlock(&watchctr_mutex);

    return watch;

}

/* translate a fsevent mask to inotify mask \
    since fsevent describes an event different and in more detail
    this will lose some info 

    when an event occurs, and the event has to be translated back
    it's up to that procedure to get the right info
*/

static int translate_fseventmask_to_inotify(struct fseventmask_struct *fseventmask)
{
    int inotify_mask=0;

    logoutput("translate_mask_fsevent_to_inotify");


    if (fseventmask->attrib_event & NOTIFYFS_FSEVENT_ATTRIB_CA) {

	/* normal attributes */

	inotify_mask|=IN_ATTRIB;

    }

    if (fseventmask->xattr_event & NOTIFYFS_FSEVENT_XATTR_CA) {

	/* under linux now: the only way to watch xattr with inotify is to watch the normal attributes */

	inotify_mask|=IN_ATTRIB;

    }

    if (fseventmask->file_event & NOTIFYFS_FSEVENT_FILE_SIZE) {

	inotify_mask|=IN_ATTRIB;

    }

    if (fseventmask->file_event & NOTIFYFS_FSEVENT_FILE_MODIFIED) {

	inotify_mask|=IN_MODIFY;

    }

    if (fseventmask->file_event & NOTIFYFS_FSEVENT_FILE_OPEN) {

	inotify_mask|=IN_OPEN;

    }

    if (fseventmask->file_event & NOTIFYFS_FSEVENT_FILE_READ) {

	inotify_mask|=IN_ACCESS;

    }

    if (fseventmask->file_event & NOTIFYFS_FSEVENT_FILE_CLOSE_WRITE) {

	inotify_mask|=IN_CLOSE_WRITE;

    }

    if (fseventmask->file_event & NOTIFYFS_FSEVENT_FILE_CLOSE_NOWRITE) {

	inotify_mask|=IN_CLOSE_NOWRITE;

    }

    if (fseventmask->move_event & NOTIFYFS_FSEVENT_MOVE_CREATED) {

	inotify_mask|=IN_CREATE;

    }

    if (fseventmask->move_event & NOTIFYFS_FSEVENT_MOVE_DELETED) {

	inotify_mask|=(IN_DELETE | IN_DELETE_SELF);

    }

    if (fseventmask->move_event & NOTIFYFS_FSEVENT_MOVE_MOVED) {

	inotify_mask|=IN_MOVE_SELF;

    }

    if (fseventmask->move_event & NOTIFYFS_FSEVENT_MOVE_MOVED_FROM) {

	inotify_mask|=IN_MOVED_FROM;

    }

    if (fseventmask->move_event & NOTIFYFS_FSEVENT_MOVE_MOVED_TO) {

	inotify_mask|=IN_MOVED_TO;

    }

    if (fseventmask->move_event & NOTIFYFS_FSEVENT_MOVE_NLINKS) {

	inotify_mask|=IN_ATTRIB;

    }

    logoutput("translate_mask_fsevent_to_inotify: inotify mask %i", inotify_mask);

    return inotify_mask;

}

/* function which set a os specific watch on the backend on path with mask mask
*/

int set_watch_backend_inotify(struct watch_struct *watch)
{
    int wd;
    int inotify_mask;
    char maskstring[64];
    int nreturn=0;

    logoutput("set_watch_backend_inotify");

    watch->mode=NOTIFYFS_WATCHMODE_OSSPECIFIC | NOTIFYFS_WATCHMODE_INOTIFY;

    /* first translate the fsevent mask into a inotify mask */

    inotify_mask=translate_fseventmask_to_inotify(&watch->fseventmask);

    if (inotify_mask==0) {

	/* fseventmask is not resulting in a os specific watch */

	nreturn=-EINVAL;
	goto out;

    }

    print_mask(inotify_mask, maskstring, 64);

    logoutput("set_watch_backend_inotify: call inotify_add_watch on path %s and mask %i/%s", watch->pathinfo.path, inotify_mask, maskstring);


    /* add some sane flags and all events:
    */

    inotify_mask|=IN_DONT_FOLLOW | IN_ALL_EVENTS;

#ifdef IN_EXCL_UNLINK

    inotify_mask|=IN_EXCL_UNLINK;

#endif

    wd=inotify_add_watch(inotify_fd, watch->pathinfo.path, inotify_mask);

    if ( wd==-1 ) {

	nreturn=-errno;

        logoutput("set_watch_backend_inotify: setting inotify watch gives error: %i", errno);

	watch->backend.wd=nreturn

    } else {

	watch->backend.wd=wd;

    }

    out:

    return nreturn;

}

int change_watch_backend_inotify(struct watch_struct *watch)
{

    /* with inotify the changing of an existing is the same call as the adding of a new watch */

    return set_watch_backend_inotify(watch);

}


void remove_watch_backend_inotify(struct watch_struct *watch)
{
    int res;

    logoutput("remove_watch_backend_inotify");

    /* lookup the inotify watch, if it's ok then it does not exist already */

    if ( watch->backend.wd>=0 ) {

	res=inotify_rm_watch(inotify_fd, watch->backend.wd);

	if ( res==-1 ) {

    	    logoutput("remove_watch_backend_inotify: deleting inotify watch %i gives error: %i", watch->backend.wd, errno);

	} else {

	    watch->backend.wd=0;

	}

    }

}

/* function to translate an event reported by
   inotify
*/

struct notifyfs_fsevent_struct *evaluate_fsevent_inotify(struct inotify_event *i_event)
{
    int res;
    struct watch_struct *watch=NULL;
    struct notifyfs_entry_struct *entry=NULL;
    struct notifyfs_fsevent_struct *notifyfs_fsevent=NULL;
    unsigned char entrycreated=0;
    struct fseventmask_struct *fseventmask;

    /* lookup watch using this wd */

    logoutput("evaluate_fsevent_inotify: received an inotify event on wd %i, mask %i", i_event->wd, i_event->mask);

    if ( ! (i_event->mask && IN_ALL_EVENTS)) {

	logoutput("evaluate_fsevent_inotify: inotify event not interesting");
	goto out;

    }

    /* first lookup the watch set by inotify */

    watch=lookup_watch_inotify_wd(i_event->wd);

    if (! watch) {

	logoutput("evaluate_fsevent_inotify: error: no watch found for inotify watch descriptor %i", i_event->wd);
	goto out;

    }

    notifyfs_fsevent=malloc(sizeof(struct notifyfs_fsevent_struct));

    if (! notifyfs_fsevent) {

	logoutput("evaluate_fsevent_inotify: unable to allocate memory");
	goto out;

    }

    logoutput("evaluate_fsevent_inotify: inotify watch %i on %s (len=%i) found", i_event->wd, watch->pathinfo.path, watch->pathinfo.len);

    init_notifyfs_fsevent(notifyfs_fsevent);

    fseventmask=&notifyfs_fsevent->fseventmask;

    /* get the entry on which the event occurs */

    if (i_event->len>0) {
	char eventstring[32];

	notifyfs_fsevent->pathinfo.path=malloc(watch->pathinfo.len + 2 + i_event->len);

	if ( ! notifyfs_fsevent->pathinfo.path) {

	    free(notifyfs_fsevent);
	    notifyfs_fsevent=NULL;
	    goto out;

	}

	notifyfs_fsevent->pathinfo.len=watch->pathinfo.len;

	memcpy(notifyfs_fsevent->pathinfo.path, watch->pathinfo.path, notifyfs_fsevent->pathinfo.len);
	*(notifyfs_fsevent->pathinfo.path+notifyfs_fsevent->pathinfo.len)='/';
	notifyfs_fsevent->pathinfo.len++;
	memcpy(notifyfs_fsevent->pathinfo.path+notifyfs_fsevent->pathinfo.len, i_event->name, i_event->len);
	notifyfs_fsevent->pathinfo.len+=i_event->len;
	*(notifyfs_fsevent->pathinfo.path+notifyfs_fsevent->pathinfo.len)='\0';

	notifyfs_fsevent->pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

	print_mask(i_event->mask, eventstring, 32);

	logoutput("evaluate_fsevent_inotify: event on %s, path %s, mask %i/%s", i_event->name, notifyfs_fsevent->pathinfo.path, i_event->mask, eventstring);

        /* something happens on an entry in the directory.. check it's in use
        by this fs, the find command will return NULL if it isn't 
	    (do nothing with close, open and access) */

	entry=find_entry_by_ino(watch->inode->ino, i_event->name);

	if (!entry) {
	    struct notifyfs_entry_struct *parent=get_entry(watch->inode->alias);

	    entry=create_entry(parent, i_event->name);

	    if (entry) {

		assign_inode(entry);

		if (entry->inode>=0) {
		    struct stat st;

		    if (lstat(notifyfs_fsevent->pathinfo.path, &st)==0) {
			struct notifyfs_inode_struct *inode=get_inode(entry->inode);
			struct notifyfs_attr_struct *attr=get_attr(inode->attr);

			if (! attr) attr=assign_attr(&st, inode);

			if (attr) {

			    copy_stat(&attr->cached_st, &st);
			    copy_stat_times(&attr->cached_st, &st);

			    attr->ctim.tv_sec=attr->cached_st.st_ctim.tv_sec;
			    attr->ctim.tv_nsec=attr->cached_st.st_ctim.tv_nsec;

			    if ( S_ISDIR(st.st_mode)) {

				/* directory no access yet */

				attr->mtim.tv_sec=0;
				attr->mtim.tv_nsec=0;

			    } else {

				attr->mtim.tv_sec=attr->cached_st.st_mtim.tv_sec;
				attr->mtim.tv_nsec=attr->cached_st.st_mtim.tv_nsec;

			    }

			}

			/* add to different lookup tables */

			add_to_name_hash_table(entry);

			entrycreated=1;

			fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_CREATED;


		    } else {

			/* do nothing here...entry did not exist in notifyfs 
			    so it's safe to assume that no client is interested... */

			logoutput("evaluate_fsevent_inotify: strange case.. event on %s but stat givies error %i", i_event->name, errno);

			/* remove the entry again?? */

			remove_entry(entry);
			entry=NULL;

			goto out;

		    }

		} else {

		    remove_entry(entry);
		    entry=NULL;

		    logoutput("evaluate_fsevent_inotify: unable to create entry");

		    goto out;

		}

	    }

	}

    } else {
	char eventstring[32];
	char *name;

	notifyfs_fsevent->pathinfo.path=malloc(watch->pathinfo.len + 1);

	if ( ! notifyfs_fsevent->pathinfo.path) {

	    free(notifyfs_fsevent);
	    notifyfs_fsevent=NULL;
	    goto out;

	}

	notifyfs_fsevent->pathinfo.len=watch->pathinfo.len;

	memcpy(notifyfs_fsevent->pathinfo.path, watch->pathinfo.path, notifyfs_fsevent->pathinfo.len);
	*(notifyfs_fsevent->pathinfo.path+notifyfs_fsevent->pathinfo.len)='\0';

	notifyfs_fsevent->pathinfo.flags=NOTIFYFS_PATHINFOFLAGS_ALLOCATED;

	print_mask(i_event->mask, eventstring, 32);

	/*  event on watch self
	    do nothing with close, open and access*/

	entry=get_entry(watch->inode->alias);

	name=get_data(entry->name);

	logoutput("evaluate_fsevent_inotify: event on %s, mask %i/%s", name, i_event->mask, eventstring);

    }

    notifyfs_fsevent->data=(void *) entry;

    if ( i_event->mask & IN_ACCESS ) {

	/* file is read */

	fseventmask->file_event|=NOTIFYFS_FSEVENT_FILE_READ;
	i_event->mask-=IN_ACCESS;

    }

    if ( i_event->mask & IN_OPEN ) {

	/* file is opened */

	fseventmask->file_event|=NOTIFYFS_FSEVENT_FILE_OPEN;
	i_event->mask-=IN_OPEN;


    }

    if ( i_event->mask & IN_CLOSE_WRITE ) {

	/* file is closed after write */

	fseventmask->file_event|=NOTIFYFS_FSEVENT_FILE_CLOSE_WRITE;
	i_event->mask-=IN_CLOSE_WRITE;

    }

    if ( i_event->mask & IN_CLOSE_NOWRITE ) {

	/* file is closed with no write */

	fseventmask->file_event|=NOTIFYFS_FSEVENT_FILE_CLOSE_NOWRITE;
	i_event->mask-=IN_CLOSE_NOWRITE;

    }

    if ( i_event->mask & IN_MODIFY ) {

	/* file is modified */

	fseventmask->file_event=NOTIFYFS_FSEVENT_FILE_MODIFIED;
	i_event->mask-=IN_MODIFY;

    }

    if ( i_event->mask & IN_ATTRIB ) {

	if (entrycreated==1) {

	    /* something happened on the attributes, but since there is no cache available (it's created)
		it's not possible to determine what exactly */

	    fseventmask->attrib_event=NOTIFYFS_FSEVENT_ATTRIB_NOTSET;
	    i_event->mask-=IN_ATTRIB;

	} else {
	    struct stat st;

	    if (lstat(notifyfs_fsevent->pathinfo.path, &st)==-1) {

		/* strange case: i_event about entry, but stat gives error... */

		logoutput("evaluate_fsevent_inotify: strange case.. event on %s but stat gives error %i", notifyfs_fsevent->pathinfo.path, errno);

		i_event->mask-=IN_ATTRIB;

		if ( i_event->name ) {

		    i_event->mask|=IN_DELETE;

		} else {

		    i_event->mask|=IN_DELETE_SELF;

		}

	    } else {
		struct notifyfs_inode_struct *inode=get_inode(entry->inode);
		struct notifyfs_attr_struct *attr=get_attr(inode->attr);

		if (attr) {

		    logoutput("evaluate_fsevent_inotify: testing and comparing the attributes");

		} else {

		    logoutput("evaluate_fsevent_inotify: attributes empty .... ");

		    goto out;

		}

		if (attr->cached_st.st_mode!=st.st_mode) {

		    fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_MODE;
		    attr->cached_st.st_mode=st.st_mode;

		    if (i_event->mask & IN_ATTRIB) i_event->mask-=IN_ATTRIB;

		}

		if (attr->cached_st.st_uid!=st.st_uid) {

		    fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_OWNER;
		    attr->cached_st.st_uid=st.st_uid;

		    if (i_event->mask & IN_ATTRIB) i_event->mask-=IN_ATTRIB;

		}

		if (attr->cached_st.st_gid!=st.st_gid) {

		    fseventmask->attrib_event|=NOTIFYFS_FSEVENT_ATTRIB_GROUP;
		    attr->cached_st.st_gid=st.st_gid;

		    if (i_event->mask & IN_ATTRIB) i_event->mask-=IN_ATTRIB;

		}

		/* nlinks belongs to group MOVE */

		if (attr->cached_st.st_nlink!=st.st_nlink) {

		    fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_NLINKS;
		    attr->cached_st.st_nlink=st.st_nlink;

		    if (i_event->mask & IN_ATTRIB) i_event->mask-=IN_ATTRIB;

		}

		/* size belongs to group FILE */

		if (attr->cached_st.st_size!=st.st_size) {

		    fseventmask->file_event|=NOTIFYFS_FSEVENT_FILE_SIZE;
		    attr->cached_st.st_size=st.st_size;

		    if (i_event->mask & IN_ATTRIB) i_event->mask-=IN_ATTRIB;

		}

		/* not yet obvious what happened: xattr or contents changed in directory??

		    with linux when adding or removing an entry, the timestamp st_mtime is changed of the parent directory

		    when changing the xattr, the timestamp st_ctime is changed

		    when using an utility like touch, the timestamps st_atime and possibly st_mtime is changed 
		*/

		/* check the mtime */

		if (attr->cached_st.st_mtim.tv_sec<st.st_mtim.tv_sec || (attr->cached_st.st_mtim.tv_sec==st.st_mtim.tv_sec && attr->cached_st.st_mtim.tv_nsec<st.st_mtim.tv_nsec)) {

		    /* first for check the change in a directory 
			other changes */

		    if ( S_ISDIR(st.st_mode) ) {

			if (i_event->mask & IN_ATTRIB) {

			    /* probably an entry created or deleted 
				test futher .... when inotify watch has been set on the parent, and in this entry - which is a directory -
				an entry is created or removed */

			    logoutput("evaluate_fsevent_inotify: IN_ATTRIB: modify timestamp changed, entry created or removed...");

			    i_event->mask-=IN_ATTRIB;

			    /* howto process futher ?? */

			}

		    } else {

			/* not a directory, and mtime is changed.... the event should be picked up before 
			    maybe make this configurable...
			    default: ignore this
			*/

			logoutput("evaluate_fsevent_inotify: modify timestamp changed");

			if (i_event->mask & IN_ATTRIB) i_event->mask-=IN_ATTRIB;

		    }

		    attr->cached_st.st_mtim.tv_sec=st.st_mtim.tv_sec;
		    attr->cached_st.st_mtim.tv_nsec=st.st_mtim.tv_nsec;

		}

		/* check the ctime */

		if (attr->cached_st.st_ctim.tv_sec<st.st_ctim.tv_sec || (attr->cached_st.st_ctim.tv_sec==st.st_ctim.tv_sec && attr->cached_st.st_ctim.tv_nsec<st.st_ctim.tv_nsec)) {

		    /* check for the xattr */

		    if (i_event->mask & IN_ATTRIB) {

			/* probably something with xattr 
			    what changed exactly is todo .... */

			logoutput("evaluate_fsevent_inotify: IN_ATTRIB: change timestamp changed, probably xattr, but no test here yet...");

			fseventmask->xattr_event|=NOTIFYFS_FSEVENT_XATTR_NOTSET;

			i_event->mask-=IN_ATTRIB;

		    }

		    attr->cached_st.st_ctim.tv_sec=st.st_ctim.tv_sec;
		    attr->cached_st.st_ctim.tv_nsec=st.st_ctim.tv_nsec;


		}

		/* check for utilities like touch, which change only the timestamps */

		/* check the atime */

		if (attr->cached_st.st_atim.tv_sec<st.st_atim.tv_sec || (attr->cached_st.st_atim.tv_sec==st.st_atim.tv_sec && attr->cached_st.st_atim.tv_nsec<st.st_atim.tv_nsec)) {

		    if (i_event->mask & IN_ATTRIB) {

			logoutput("evaluate_fsevent_inotify: IN_ATTRIB: access timestamp changed, no event detected");

			i_event->mask-=IN_ATTRIB;

		    }

		    attr->cached_st.st_atim.tv_sec=st.st_atim.tv_sec;
		    attr->cached_st.st_atim.tv_nsec=st.st_atim.tv_nsec;

		}

	    }

	}

    }

    if ( i_event->mask & IN_DELETE_SELF ) {

	fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_DELETED;
	i_event->mask-=IN_DELETE_SELF;

    }

    if ( i_event->mask & IN_DELETE ) {

	fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_DELETED;
	i_event->mask-=IN_DELETE;

    }

    if ( i_event->mask & IN_MOVE_SELF ) {

	fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_MOVED;
	i_event->mask-=IN_MOVE_SELF;

    }

    /* do something with the cookie provided by inotify? */

    if ( i_event->mask & IN_MOVED_FROM ) {

	fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_MOVED_FROM;
	i_event->mask-=IN_MOVED_FROM;

    }

    if ( i_event->mask & IN_MOVED_TO ) {

	fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_MOVED_TO;
	i_event->mask-=IN_DELETE_SELF;

    }

    if ( i_event->mask & IN_CREATE ) {

	fseventmask->move_event|=NOTIFYFS_FSEVENT_MOVE_CREATED;
	i_event->mask-=IN_CREATE;

    }

    out:

    return notifyfs_fsevent;

}

/* INOTIFY BACKEND SPECIFIC CALLS */

void handle_data_on_inotify_fd(int fd, uint32_t events, int signo)
{
    int nreturn=0;
    char outputstring[256];
    int lenread=0;
    char buff[INOTIFY_BUFF_LEN];

    logoutput("handle_data_on_inotify_fd");

    lenread=read(fd, buff, INOTIFY_BUFF_LEN);

    if ( lenread<0 ) {

        logoutput("handle_data_on_inotify_fd: error (%i) reading inotify events (fd: %i)", errno, fd);

    } else {
        int i=0, res;
        struct inotify_event *i_event=NULL;

        while(i<lenread) {

            i_event = (struct inotify_event *) &buff[i];

            /* handle overflow here */

            if ( (i_event->mask & IN_Q_OVERFLOW) || i_event->wd==-1 ) {

                /* what to do here: read again?? go back ??*/

                logoutput("handle_data_on_inotify_fd: error reading inotify events: buffer overflow.");
                goto next;

            }

	    if ( (i_event->mask & IN_ISDIR) && (i_event->mask & (IN_OPEN | IN_CLOSE_NOWRITE))) {

		/* explicit ignore the reading of directories */

		goto next;

	    }

	    while (i_event->mask>0) {
		uint32_t oldmask=i_event->mask;
		struct notifyfs_fsevent_struct *notifyfs_fsevent=NULL;

		/* translate the inotify event in a general notifyfs fs event */

		notifyfs_fsevent=evaluate_fsevent_inotify(i_event);

		if (! notifyfs_fsevent) {

		    logoutput("handle_data_on_inotify_fd: no notifyfs_fsevent... break");
		    break;

		} else {

		    /* process the event here futher: put it on a queue to be processed by a special thread */

		    logoutput("handle_data_on_inotify_fd: received notifyfs_fsevent");

		    get_current_time(&notifyfs_fsevent->detect_time);
		    queue_fsevent(notifyfs_fsevent);

		}

		/* prevent an endless loop */

		if (i_event->mask==oldmask) break;

	    }

	    next:

            i += INOTIFY_EVENT_SIZE + i_event->len;

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
        return;

    }

    /* add inotify to the main eventloop */

    epoll_xdata=add_to_epoll(inotify_fd, EPOLLIN | EPOLLPRI, &handle_data_on_inotify_fd, NULL, &xdata_inotify, NULL);

    if ( ! epoll_xdata ) {

        logoutput("error adding inotify fd to mainloop.");
        return;

    } else {

        logoutput("inotify fd %i added to epoll", inotify_fd);

	add_xdata_to_list(epoll_xdata);

    }

}

void close_inotify()
{

    if ( xdata_inotify.fd>0 ) {

	remove_xdata_from_epoll(&xdata_inotify);
	close(xdata_inotify.fd);
	xdata_inotify.fd=0;
	remove_xdata_from_list(&xdata_inotify, 0);

	inotify_fd=0;

    }

}

