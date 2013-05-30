sbin_PROGRAMS = notifyfs

notifyfs_CFLAGS = $(FUSE_CFLAGS) $(GLIB_CFLAGS) -O3 -Wall -I../common/ -Wno-unused-but-set-variable -Wno-uninitialized -Wno-unused-variable -Wno-unused-label
notifyfs_LDADD = $(FUSE_LIBS) $(GLIB_LIBS)

notifyfs_SOURCES = access.c backend.c changestate.c client.c handleclientmessage.c entry-management.c ../common/epoll-utils.c \
		    filesystem.c filesystem-nfs.c filesystem-smb.c filesystem-ssh.c ../common/message-receive.c \
		    ../common/handlefuseevent.c ../common/message-send.c ../common/mountinfo.c handlemountinfoevent.c ../common/notifyfs-fsevent.c \
		    ../common/notifyfs-io.c ../common/mountinfo-monitor.c networkutils.c networkservers.c \
		    notifyfs.c options.c path-resolution.c \
		    ../common/simple-list.c ../common/socket.c ../common/utils.c watches.c ../common/workerthreads.c xattr.c
