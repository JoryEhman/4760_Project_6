CC      = gcc
CFLAGS  = -Wall -Wextra -g
TARGETS = oss user_proc

.PHONY: all clean

all: $(TARGETS)

oss: oss.c shared.h
	$(CC) $(CFLAGS) -o oss oss.c

user_proc: user_proc.c shared.h
	$(CC) $(CFLAGS) -o user_proc user_proc.c

clean:
	rm -f $(TARGETS) *.o oss.log