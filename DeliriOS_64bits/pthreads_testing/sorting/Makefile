CFLAGS= -Wall -g -ggdb -lpthread
TARGETS = testsort.c sort_code.c

.PHONY: all clean
all: clean sort

run: all
	@valgrind --leak-check=full ./testsort

sort: $(TARGETS)
	@gcc $(CFLAGS) $(TARGETS) -lm -lpthread -o testsort

clean:
	reset
	@rm -f testsort
