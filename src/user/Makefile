SHELL=/bin/sh
CC=gcc
CFLAGS=-O2 -Wall -W -Wextra -Wshadow -std=c11 -pedantic
ACTIVATE_OBJ=activation.o
RESTORE_OBJ=restore.o
ACTIVATE_OUT=blkdev-activation
RESTORE_OUT=blkdev-restore

all: $(ACTIVATE_OBJ) $(RESTORE_OBJ)
	$(CC) $(ACTIVATE_OBJ) -o $(ACTIVATE_OUT)
	$(CC) $(RESTORE_OBJ) -o $(RESTORE_OUT)

clean:
	rm $(ACTIVATE_OUT)
	rm $(RESTORE_OUT)
