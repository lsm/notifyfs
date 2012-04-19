OBJS = utils.o options.o xattr.o socket.o epoll-utils.o handlefuseevent.o entry-management.o message.o message-client.o path-resolution.o testfs.o watches.o changestate.o
EXECUTABLE = testfs

CC=gcc
# CFLAGS = -Wall -Wno-unused-but-set-variable -Wno-uninitialized -Wno-unused-variable -Wno-unused-label -std=gnu99 -O3 -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lpthread -lfuse -lrt -ldl -lm
CFLAGS = -Wall -std=gnu99 -O3 -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lpthread -lfuse -lrt -ldl -lm

LDFLAGS = $(CFLAGS)
COMPILE = $(CC) $(CFLAGS) -c
LINKCC = $(CC)


all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	$(LINKCC) $(LDFLAGS) -o $(EXECUTABLE) $(OBJS)

epoll-utils.o: epoll-utils.h logging.h

utils.o: utils.h logging.h

testfs.o: utils.h options.h xattr.h epoll-utils.h handlefuseevent.h entry-management.h logging.h testfs.h message.h message-client.h global-defines.h socket.h path-resolution.h
options.o: options.h logging.h global-defines.h
xattr.o: xattr.h logging.h testfs.h global-defines.h entry-management.h
client.o: client.h logging.h entry-management.h

handlefuseevent.o: handlefuseevent.h epoll-utils.h logging.h global-defines.h
socket.o: socket.h epoll-utils.h logging.h global-defines.h
watches.o: watches.h entry-management.h logging.h testfs.h global-defines.h
changestate.o: changestate.h entry-management.h logging.h testfs.h global-defines.h

entry-management.o: entry-management.h logging.h global-defines.h

message.o: message.h logging.h global-defines.h
message-client.o: message.h message-client.h logging.h global-defines.h

%.o: %.c 
	$(COMPILE) -o $@ $<


clean:
	rm -f $(OBJS) $(EXECUTABLE) *~
