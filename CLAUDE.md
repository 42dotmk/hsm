# hsm

Runit-style service supervisor, pure C99 + Linux (signalfd, prctl). Two
binaries from two files: `hsmd.c` (daemon, single poll loop supervising all
services) and `hsm.c` (client, text lines over a unix socket). Compile-time
config in `config.h`, suckless style. `make` builds, `make install` symlinks
into `~/.local/bin`.

Conventions:
- Services are directories under svdir with an executable `./run` (runit
  contract); the service list is runtime data, never config.h.
- No threads, no async signal handlers — signals come through the signalfd
  and everything runs sequentially in main's poll loop.
- Keep it small and readable; prefer deleting features to adding flags.
- Formatting: 2-space indent, /* */ comments, lowercase names, C99.

Testing by hand: create a throwaway service dir with a `run` script, start
`./hsmd` in a terminal (it logs to stderr), drive it with `./hsm`. The
sibling projects (hed, hws, hwm, htray) share the makefile/install shape.
