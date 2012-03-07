OBJS = utils.o options.o xattr.o socket.o fuse-loop-epoll-mt.o entry-management.o testfs.o
EXECUTABLE = testfs

CC=gcc
CFLAGS = -Wall -std=gnu99 -O3 -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lpthread -lfuse -lrt -ldl -lm

LDFLAGS = $(CFLAGS)
COMPILE = $(CC) $(CFLAGS) -c
LINKCC = $(CC)


all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	$(LINKCC) $(LDFLAGS) -o $(EXECUTABLE) $(OBJS)

fuse-loop-epoll-mt.o: fuse-loop-epoll-mt.h logging.h

utils.o: utils.h logging.h
testfs.o: utils.h options.h xattr.h fuse-loop-epoll-mt.h entry-management.h logging.h testfs.h global-defines.h
options.o: options.h logging.h global-defines.h
xattr.o: xattr.h logging.h testfs.h global-defines.h entry-management.h
client.o: client.h logging.h entry-management.h

entry-management.o: entry-management.h logging.h global-defines.h

%.o: %.c 
	$(COMPILE) -o $@ $<


clean:
	rm -f $(OBJS) $(EXECUTABLE) *~
