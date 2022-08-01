CC = gcc

CFLAGS = -std=gnu99 -g -O3

default: all

all: http_client

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

%: %.o
	$(CC) -o $@ $^

.PRECIOUS: %.o

.PHONY: default all

