sbin_PROGRAMS = notifyfs

notifyfs_CFLAGS = $(FUSE_CFLAGS) $(GLIB_CFLAGS) -O3 -Wall -I../common/ -Wno-unused-but-set-variable -Wno-uninitialized -Wno-unused-variable -Wno-unused-label -lmagic 
notifyfs_LDADD = $(FUSE_LIBS) $(GLIB_LIBS)

notifyfs_SOURCES = access.c changestate.c client.c handleclientmessage.c entry-management.c ../common/epoll-utils.c ../common/handlemessage.c \
		    ../common/handlefuseevent.c ../common/message.c message-server.c ../common/mountinfo.c handlemountinfoevent.c handlelockinfoevent.c \
		    ../common/notifyfs-io.c mimetype.c lock-monitor.c ../common/mountinfo-monitor.c networkutils.c notifyfs.c options.c path-resolution.c \
		    ../common/socket.c ../common/utils.c watches.c ../common/workerthreads.c xattr.c
