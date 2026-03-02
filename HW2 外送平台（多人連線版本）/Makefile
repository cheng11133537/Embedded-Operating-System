CC = gcc
CFLAGS = -Wall -O2

TARGET = hw2
OBJS = hw2.o 

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

hw2.o: hw2.c 
	$(CC) $(CFLAGS) -c hw2.c

clean:
	rm -f $(TARGET) $(OBJS)
