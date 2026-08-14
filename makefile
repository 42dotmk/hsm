.POSIX:

CC     = cc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Os -D_GNU_SOURCE
BINDIR = $(HOME)/.local/bin

all: hsmd hsm

hsmd: hsmd.c config.h
	$(CC) $(CFLAGS) -o $@ hsmd.c

hsm: hsm.c config.h
	$(CC) $(CFLAGS) -o $@ hsm.c

install: all
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hsmd" $(BINDIR)/hsmd
	ln -sf "$$(pwd)/hsm" $(BINDIR)/hsm

uninstall:
	rm -f $(BINDIR)/hsmd $(BINDIR)/hsm

clean:
	rm -f hsmd hsm

.PHONY: all install uninstall clean
