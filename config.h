/* hsm configuration.
 * Paths starting with '/' are absolute; anything else is relative to $HOME. */

static const char svdir[]    = ".config/hsm/sv";       /* one subdir per service */
static const char sockpath[] = ".config/hsm/hsm.sock"; /* control socket */

static const int throttle = 1; /* min seconds between restarts of a crashing service */
static const int killwait = 7; /* seconds after SIGTERM before SIGKILL */

#define MAXSV   64 /* max supervised services */
#define NAMEMAX 64 /* max service name length */
