CC = gcc
CFLAGS = -Wall -pthread
LIBS = 

SERVER = server
CLIENT = client

.PHONY all clean

all $(SERVER) $(CLIENT)

$(SERVER) server.c
	$(CC) $(CFLAGS) -o $(SERVER) server.c $(LIBS)

$(CLIENT) client.c
	$(CC) $(CFLAGS) -o $(CLIENT) client.c $(LIBS)

clean
	rm -f $(SERVER) $(CLIENT) .o
