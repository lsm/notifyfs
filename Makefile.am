sbin_PROGRAMS = notifyfs

notifyfs_CFLAGS = $(FUSE_CFLAGS) $(GLIB_CFLAGS) -O3 -Wall -Wno-unused-but-set-variable -Wno-uninitialized -Wno-unused-variable -Wno-unused-label
notifyfs_LDADD = $(FUSE_LIBS) $(GLIB_LIBS)

notifyfs_SOURCES = access.c client.c entry-management.c epoll-utils.c \
		    handlefuseevent.c message.c mountinfo.c handlemountinfoevent.c \
		    mountinfo-monitor.c notifyfs.c options.c path-resolution.c \
		    socket.c utils.c workerthreads.c xattr.c
