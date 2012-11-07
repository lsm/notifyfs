sbin_PROGRAMS = testclient

testclient_CFLAGS = $(FUSE_CFLAGS) -O3 -Wall -Wno-unused-but-set-variable -Wno-uninitialized -Wno-unused-variable -Wno-unused-label
testclient_LDADD = $(FUSE_LIBS)

testclient_SOURCES = epoll-utils.c message.c processdata.c socket.c testclient.c usermenu.c utils.c
