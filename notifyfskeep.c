void handle_mountinfo_request(struct client_struct *client,  struct notifyfs_mount_message *mount_message, void *data1, void *data2)
{
    int res=0;
    struct mount_entry_struct *mount_entry;
    char *path=(char *) data1;

    if ( strlen(mount_message->fstype)>0 ) {

	logoutput("handle_mountinfo_request, for fstype %s", mount_message->fstype);

    } else {

	logoutput("handle_mountinfo_request, no fs filter");

    }

    if ( path && strlen(path)>0 ) {

	logoutput("handle_mountinfo_request, a subdirectory of %s", path);

    } else {

	logoutput("handle_mountinfo_request, no path filter");

    }


    res=lock_mountlist();

    mount_entry=get_next_mount_entry(NULL, 1, MOUNTENTRY_CURRENT);

    while (mount_entry) {


	if ( strlen(mount_message->fstype)>0 ) {

	    logoutput("handle_mountinfo_request, compare %s with %s", mount_entry->fstype, mount_message->fstype);

	    /* filter on filesystem */

	    if ( strcmp(mount_message->fstype, mount_entry->fstype)!=0 ) goto next;

	}

	if ( path && strlen(path)>0 ) {

	    logoutput("handle_mountinfo_request, compare %s with %s", mount_entry->mountpoint, path);

	    /* filter on directory */

	    if ( issubdirectory(mount_entry->mountpoint, path, 1)==0 ) goto next;

	}

	res=send_mount_message(client->fd, mount_entry, mount_message->unique);

	next:

	mount_entry=get_next_mount_entry(mount_entry, 1, MOUNTENTRY_CURRENT);

    }

    res=unlock_mountlist();

    /* a reply message to terminate */

    res=reply_message(client->fd, mount_message->unique, 0);

}

void handle_fsevent_message(struct client_struct *client, struct notifyfs_fsevent_message *fsevent_message, void *data1, int len1)
{
    unsigned char type=fsevent_message->type;

    if ( type==NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYPATH ) {

	logoutput("handle_fsevent_message: setwatch_bypath");

	/* here read the data, it must be complete:
	   - path and mask 
	   then set the watch at backend
	*/


    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_SETWATCH_BYINO ) {

	logoutput("handle_fsevent_message: setwatch_byino");

	/* here read the data, it must be complete:
	   - ino and mask 
	   then set the watch at backend
	*/

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_NOTIFY ) {
	struct effective_watch_struct *effective_watch;

	logoutput("handle_fsevent_message: notify");

	/* read the data from the client fs 
	   and pass it through to client apps 
	   but first filter it out as it maybe an event caused by this fs
	   and because it comes through a message it's an event on the backend
	   howto determine....
	   it's a fact that inotify events have been realised on the VFS,
	   with events on the backend this is not so
	   but first filter out the events caused by this host....*/

	/* lookup the watch the event is about using the backend_id */

	/* there must be a client interested */

	effective_watch=lookup_watch(FSEVENT_BACKEND_METHOD_FORWARD, fsevent_message->id);

	if ( effective_watch ) {

	    logoutput("handle_fsevent_message: watch found.");

	    if ( fsevent_message->statset==1 ) {

		evaluate_and_process_fsevent(effective_watch, (char *) data1, len1, fsevent_message->mask, &(fsevent_message->st), FSEVENT_BACKEND_METHOD_FORWARD);

	    } else {

		evaluate_and_process_fsevent(effective_watch, (char *) data1, len1, fsevent_message->mask, NULL, FSEVENT_BACKEND_METHOD_FORWARD);

	    }

	} else {

	    logoutput("handle_fsevent_message: watch not found for id %li.", fsevent_message->id);

	}


    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_DELWATCH ) {

	logoutput("handle_fsevent_message: delwatch");

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_SLEEPWATCH ) {

	logoutput("handle_fsevent_message: sleepwatch");

    } else if ( type==NOTIFYFS_MESSAGE_FSEVENT_WAKEWATCH ) {

	logoutput("handle_fsevent_message: wakewatch");

    } else {

	logoutput("handle_fsevent_message: unknown message");

    }

}

void handle_reply_message(struct client_struct *client,  struct notifyfs_reply_message *reply_message, char *path)
{
    unsigned char type=reply_message->type;

    if ( type==NOTIFYFS_MESSAGE_REPLY_OK ) {

	logoutput("handle_reply_message: reply_ok");

    } else if ( type==NOTIFYFS_MESSAGE_REPLY_ERROR ) {

	logoutput("handle_reply_message: reply_error");

    } else if ( type==NOTIFYFS_MESSAGE_REPLY_REPLACE ) {

	logoutput("handle_reply_message: reply_error");

    } else {

	logoutput("handle_reply_message: unknown message");

    }

}
