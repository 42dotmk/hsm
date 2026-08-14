# hsm — hackable service manager

A runit-style service supervisor for Linux in ~600 lines of C. One daemon
(`hsmd`) supervises everything from a single poll loop; one client (`hsm`)
controls it over a unix socket. Configuration is compile-time in `config.h`,
services are directories on disk.

## Service contract (same as runit)

A service is a subdirectory of the service directory (default
`~/.config/hsm/sv`) with an executable `run` script that runs the daemon in
the **foreground**:

    ~/.config/hsm/sv/syncthing/run:
      #!/bin/sh
      exec syncthing serve --no-browser

- `exec` so signals reach the real process, not a leftover shell.
- A file named `down` in the service directory means "don't start at boot".
- Service stdout/stderr land in `log` in the service directory, rotated to
  `log.0`, `log.1`, ... past `logsize` bytes (`logkeep` old files are kept).
- Existing runit run scripts work as-is.

## Usage

    make && make install     # symlinks into ~/.local/bin
    hsmd                     # run in the foreground, e.g. from .xinitrc:  hsmd &
    hsm                      # status of all services
    hsm up NAME              # start (and keep restarting) a service
    hsm down NAME            # stop a service: SIGTERM, then SIGKILL after killwait
    hsm restart NAME
    hsm rescan               # pick up added/removed service directories (or: pkill -HUP hsmd)

Status states: `run` (running), `down` (stopped on purpose), `wait` (died,
restart pending — a crash-looping service sits here between throttled
restarts), `term` (SIGTERM sent, waiting for it to die).

## How it works

- Each service runs in its own session (`setsid`), so `down` signals the
  whole process group — children included.
- Signals arrive over a `signalfd`, so there are no async signal handlers:
  everything is sequential in one `poll` loop over the signalfd and the
  control socket.
- A service that exits is restarted, but never more than once per `throttle`
  seconds, so a crashing service can't spin the CPU.
- `hsmd` is a child subreaper (`prctl`), so orphans of double-forking
  services are reaped instead of becoming init's problem.
- Service output flows through a pipe held by hsmd rather than straight
  into a file: that is what makes rotation possible while the service
  runs (a child writing to its own fd would follow the renamed file
  forever). The pipe outlives restarts, so grandchild output keeps
  landing in the same log. Rotation itself lives in `log.c`.
- Stopping hsmd (SIGTERM/SIGINT) stops every service and waits for them
  before exiting.

## Hacking

Everything is in `hsmd.c`; the interesting parts are `start()` (fork/exec
into a fresh session), `reap()` (the `waitpid` loop and restart throttle),
`tick()` (the state machine: enforce want-vs-state, compute the next poll
timeout), and `handleclient()` (the whole wire protocol — it's just text
lines). Knobs live in `config.h`.

Deliberately not here (yet): log rotation, service dependencies, readiness
notification, running as pid 1.
