.POSIX:

CC     = cc

# Version derived from `git describe` at build time so the binaries report
# the exact tag/commit they were built from; "dev" without git metadata.
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS = -std=c11 -pedantic -Wall -Wextra -Os -D_GNU_SOURCE \
         -DHSM_VERSION='"$(VERSION)"'
BINDIR = $(HOME)/.local/bin

all: hsmd hsm

hsmd: hsmd.c log.c log.h config.h
	$(CC) $(CFLAGS) -o $@ hsmd.c log.c

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
