OBJS = epoll-utils.o message.o message-client.o socket.o testapp.o
EXECUTABLE = testapp

CC=gcc
CFLAGS = -Wall -std=gnu99 -O3 -D_FILE_OFFSET_BITS=64 -lpthread -lrt -ldl -lm

LDFLAGS = $(CFLAGS)
COMPILE = $(CC) $(CFLAGS) -c
LINKCC = $(CC)


all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	$(LINKCC) $(LDFLAGS) -o $(EXECUTABLE) $(OBJS)

epoll-utils.o: epoll-utils.h logging.h

testapp.o: epoll-utils.h socket.h logging.h message.h message-client.h testapp.h global-defines.h
socket.o: socket.h logging.h global-defines.h

message.o: message.h logging.h global-defines.h
message-client.o: message.h message-client.h logging.h global-defines.h

%.o: %.c 
	$(COMPILE) -o $@ $<


clean:
	rm -f $(OBJS) $(EXECUTABLE) *~
