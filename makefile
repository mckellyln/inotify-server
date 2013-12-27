
CC = gcc
CFLAGS = -I. -g -O2 -Wall -std=c99
LIBS =

all: server example

server: server.c uthash.h
	$(CC) $(CFLAGS) $< -o $@ $(LIBS) -lpthread 

example: example.c
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)

clean:
	$(RM) -f server example

