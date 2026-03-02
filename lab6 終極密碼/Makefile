CC = gcc
CFLAGS = -Wall -O2

all: game guess

game: game.c
	$(CC) $(CFLAGS) -o game game.c

guess: guess.c
	$(CC) $(CFLAGS) -o guess guess.c

clean:
	rm -f game guess
