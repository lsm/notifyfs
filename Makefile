OBJS = utils.o options.o xattr.o client.o socket.o fuse-loop-epoll-mt.o entry-management.o access.o notifyfs.o mountinfo.o
EXECUTABLE = notifyfs

CC=gcc
CFLAGS = -Wall -std=gnu99 -O3 -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lpthread -lfuse -lrt -ldl -I/usr/include/glib-2.0 -I/usr/lib/glib-2.0/include -lm -lglib-2.0

LDFLAGS = $(CFLAGS)
COMPILE = $(CC) $(CFLAGS) -c
LINKCC = $(CC)


all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	$(LINKCC) $(LDFLAGS) -o $(EXECUTABLE) $(OBJS)

fuse-loop-epoll-mt.o: fuse-loop-epoll-mt.h logging.h

utils.o: utils.h logging.h
notifyfs.o: utils.h options.h xattr.h fuse-loop-epoll-mt.h entry-management.h logging.h notifyfs.h global-defines.h mountinfo.h
options.o: options.h logging.h global-defines.h
xattr.o: xattr.h logging.h notifyfs.h global-defines.h entry-management.h
client.o: client.h logging.h entry-management.h
access.o: access.h logging.h entry-management.h
mountinfo.o: mountinfo.h logging.h global-defines.h

entry-management.o: entry-management.h logging.h global-defines.h

%.o: %.c 
	$(COMPILE) -o $@ $<


clean:
	rm -f $(OBJS) $(EXECUTABLE) *~
