# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

hsm is a runit-style service supervisor for Linux in ~600 lines of C11. Two binaries from two files: `hsmd.c` (daemon: one poll loop supervises every service — no process-per-service like runit) and `hsm.c` (client: joins its args into one text line, sends it over a unix socket, prints the reply). `log.c` holds log rotation. Compile-time config in `config.h` (paths, `throttle`, `killwait`, log rotation knobs, `MAXSV`/`NAMEMAX`), suckless style.

## Build and test

    make            # builds ./hsmd and ./hsm
    make install    # symlinks into ~/.local/bin
    make clean

No test suite or linter; `-std=c11 -pedantic -Wall -Wextra` must stay clean. Test by hand: create a throwaway service dir (a subdirectory of `svdir`, default `~/.config/hsm/sv`, containing an executable `run` script that execs a foreground process), run `./hsmd` in a terminal (it runs in the foreground and logs to stderr), drive it with `./hsm` (`status` / `up NAME` / `down NAME` / `restart NAME` / `rescan`). A second hsmd refuses to start if the socket is connectable.

## Architecture

Everything sequential in `main()`'s poll loop over three fd groups: the signalfd, the listening control socket, and one log pipe per service. There are no threads and no async signal handlers — signals (SIGCHLD → `reap()`, SIGHUP → `scan()`, SIGTERM/SIGINT → shutdown) arrive as data through the signalfd.

Per-service state machine: `state` (SDOWN/SUP/STERM) vs `want` (WANTUP/WANTDOWN), reconciled by `tick()` each loop iteration. `tick()` is also the only timer: it computes the poll timeout from each service's `deadline` (throttled restart when SDOWN, SIGTERM→SIGKILL escalation when STERM). The key functions map cleanly: `start()` (fork/exec `./run` in a fresh session with cwd = the service dir), `stop()` (SIGTERM to the process *group*, `kill(-pid)`), `reap()` (waitpid loop + restart-throttle deadline), `handleclient()` (the whole wire protocol — plain text lines, one line in / reply out / close), `scan()` (directory rescan; a vanished dir means stop-then-forget).

Invariants to preserve:

- **The service list is runtime data** (directories under svdir, runit contract: executable `./run`, optional `down` file suppresses autostart) — never move it into `config.h`.
- **Each service runs in its own session** (`setsid`) so stopping signals the whole tree; hsmd is a child subreaper (`prctl`) so orphans of double-forking services get reaped in `reap()` (unknown pids are reaped and ignored).
- **Log pipes outlive restarts on purpose.** hsmd keeps *both* pipe ends (`outfd`/`infd`): keeping the write end means no EOF handling and grandchild output keeps landing in the same log; output flows through hsmd (not straight to a file) precisely so `log.c` can rotate while the service runs. Don't "fix" the retained write end.
- **In the main loop, service pipes are drained before signals/clients** — `handlesignals()`/`handleclient()` can rescan and reorder `sv[]`, which would invalidate the pfd→service mapping built that round.
- Client-visible states are derived, not stored: `run`, `term`, and SDOWN splits into `wait` (want up, restart pending) vs `down` — see `statename()`.
- An explicit `up`/`restart` clears the throttle deadline; only crash-loop restarts wait it out.

Deliberately absent (don't add without being asked): service dependencies, readiness notification, restart backoff, running as pid 1.

## Style

2-space indent, `/* */` comments, lowercase names, C11, fixed-size arrays. Keep it small and readable; prefer deleting features to adding flags. The sibling projects (hed, hws, hwm, htray) share the makefile/install shape.
