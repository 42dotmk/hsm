/* hsm configuration.
 * Paths starting with '/' are absolute; anything else is relative to $HOME. */

#ifndef HSM_VERSION
#define HSM_VERSION "dev" /* the makefile injects `git describe` here */
#endif

static const char svdir[] = ".config/hsm/sv"; /* one subdir per service */
static const char sockpath[] = ".config/hsm/hsm.sock"; /* control socket */

static const int throttle =
    1; /* min seconds between restarts of a crashing service */
static const int killwait = 7; /* seconds after SIGTERM before SIGKILL */

static const long logsize =
    1 << 20;                  /* rotate a service log past this many bytes */
static const int logkeep = 3; /* rotated logs to keep (log.0 is the newest) */

#define MAXSV 64   /* max supervised services */
#define NAMEMAX 64 /* max service name length */
