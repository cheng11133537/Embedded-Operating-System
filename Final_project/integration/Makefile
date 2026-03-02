CC      = gcc
CFLAGS  = -Wall -Wextra -g
LDFLAGS = 

TARGETS = server client


all: $(TARGETS)


server: server.o
	$(CC) $(LDFLAGS) -o $@ $^

client: client.o
	$(CC) $(LDFLAGS) -o $@ $^


%.o: %.c log_common.h
	$(CC) $(CFLAGS) -c $< -o $@


clean:
	rm -f *.o $(TARGETS)

.PHONY: all clean
