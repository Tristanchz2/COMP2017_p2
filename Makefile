# TODO: make sure the rules for server client and markdown filled!
CC := gcc
CFLAGS := -Wall -Wextra

.PHONY: all clean

all: server client


server: source/server.c markdown.o
	$(CC) $(CFLAGS) -o server source/server.c markdown.o

client: source/client.c
	$(CC) $(CFLAGS) -o client source/client.c

markdown.o: source/markdown.c libs/markdown.h libs/document.h
	$(CC) $(CFLAGS) -c source/markdown.c -o markdown.o

clean:
	rm -f *.o server client