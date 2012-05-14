OBJS = utils.o options.o xattr.o client.o socket.o epoll-utils.o handlefuseevent.o entry-management.o access.o notifyfs.o mountinfo.o mountinfo-monitor.o message.o message-server.o path-resolution.o watches.o changestate.o determinechanges.o
EXECUTABLE = notifyfs

CC=gcc
CFLAGS = -Wall -Wno-unused-but-set-variable -Wno-uninitialized -Wno-unused-variable -Wno-unused-label -std=gnu99 -O3 -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lpthread -lfuse -lrt -ldl -I/usr/include/glib-2.0 -I/usr/lib/glib-2.0/include -lm -lglib-2.0

LDFLAGS = $(CFLAGS)
COMPILE = $(CC) $(CFLAGS) -c
LINKCC = $(CC)


all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	$(LINKCC) $(LDFLAGS) -o $(EXECUTABLE) $(OBJS)

epoll-utils.o: epoll-utils.h logging.h
entry-management.o: entry-management.h logging.h global-defines.h mountinfo.h watches.h
handlefuseevent.o: handlefuseevent.h epoll-utils.h logging.h global-defines.h

utils.o: utils.h logging.h
notifyfs.o: utils.h options.h xattr.h epoll-utils.h entry-management.h logging.h notifyfs.h global-defines.h mountinfo.h mountinfo-monitor.h socket.h message.h message-server.h path-resolution.h watches.h handlefuseevent.h changestate.h

determinechanges.o: determinechanges.h notifyfs.h logging.h
changestate.o: changestate.h logging.h global-defines.h message.h client.h entry-management.h path-resolution.h 
options.o: options.h logging.h global-defines.h notifyfs.h
xattr.o: xattr.h logging.h notifyfs.h global-defines.h entry-management.h path-resolution.h 
client.o: client.h logging.h entry-management.h notifyfs.h
access.o: access.h logging.h entry-management.h notifyfs.h path-resolution.h 
mountinfo.o: mountinfo.h logging.h global-defines.h
mountinfo-monitor.o: mountinfo.h mountinfo-monitor.h logging.h global-defines.h
socket.o: socket.h message.h logging.h global-defines.h notifyfs.h

message.o: message.h logging.h global-defines.h
message-server.o: message.h message-server.h logging.h global-defines.h




%.o: %.c 
	$(COMPILE) -o $@ $<

clean:
	rm -f $(OBJS) $(EXECUTABLE) *~
