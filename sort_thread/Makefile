CC=gcc
CFLAGS=-std=c99 -pthread -Wall -Werror

OBJ= test.o aux.o

.PHONY=clean all

all: clean test

test: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^


clean:
	rm -f *.o