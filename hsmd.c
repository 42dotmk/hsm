/* hsmd - hackable service manager, the supervising daemon.
 *
 * A service is a subdirectory of the service directory (svdir in config.h)
 * containing an executable ./run script that runs the daemon in the
 * foreground.  hsmd starts every service, reaps it when it dies and restarts
 * it, throttled so a crashing service cannot spin.  A service directory
 * containing a file named "down" is not started at boot.
 *
 * Unlike runit there is no process per service: one poll loop supervises
 * everything.  Signals arrive over a signalfd, commands arrive as text lines
 * over a unix socket ("up foo\n", "status\n", ...) written by the hsm client.
 * Each service runs in its own session/process group so stopping a service
 * signals its whole tree: SIGTERM first, SIGKILL killwait seconds later.
 *
 * hsmd runs in the foreground and logs to stderr; start it from your session
 * (.xinitrc, a terminal, another supervisor).  Service stdout/stderr flow
 * through a pipe into hsmd and land in a rotated "log" file inside the
 * service directory (see log.c).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

enum { SDOWN, SUP, STERM }; /* run state: stopped, running, SIGTERM sent */
enum { WANTDOWN, WANTUP };  /* what the user asked for */

typedef struct {
  char name[NAMEMAX];
  pid_t pid; /* 0 when not running */
  int state;
  int want;
  int seen;      /* directory still exists (scratch flag for scan) */
  long started;  /* monotonic seconds at last exec */
  long deadline; /* when to act next: restart a throttled service (SDOWN)
                  * or escalate SIGTERM to SIGKILL (STERM); 0 = now/none */
  int outfd;     /* read end of the service's stdout/stderr pipe, -1 = none */
  int infd;      /* write end; we keep it too so the pipe survives restarts
                  * (no EOF handling, grandchild output still lands here) */
  Log log;       /* rotated <servicedir>/log fed from outfd, see log.c */
} Service;

static Service sv[MAXSV];
static int nsv;
static char svroot[PATH_MAX];
static char sockfile[PATH_MAX];
static int sigfd = -1, lsock = -1;
static int shuttingdown;

static long now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static void die(const char *msg) {
  fprintf(stderr, "hsmd: %s: %s\n", msg, strerror(errno));
  exit(1);
}

/* expand a config.h path: absolute stays, anything else is $HOME-relative */
static void homepath(char *dst, size_t len, const char *p) {
  const char *home = getenv("HOME");
  if (p[0] == '/')
    snprintf(dst, len, "%s", p);
  else
    snprintf(dst, len, "%s/%s", home ? home : ".", p);
}

/* path inside a service directory; file == NULL gives the directory itself.
 * Returns a static buffer: use the result before the next call. */
static const char *svpath(const char *name, const char *file) {
  static char buf[PATH_MAX];
  int n;
  if (file)
    n = snprintf(buf, sizeof(buf), "%s/%s/%s", svroot, name, file);
  else
    n = snprintf(buf, sizeof(buf), "%s/%s", svroot, name);
  if (n < 0 || (size_t)n >= sizeof(buf)) {
    fprintf(stderr, "hsmd: path too long for %s\n", name);
    buf[0] = '\0';
  }
  return buf;
}

static void mkpath(const char *path) {
  char tmp[PATH_MAX], *p;
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (p = tmp + 1; *p; p++)
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  mkdir(tmp, 0755);
}

static Service *findsv(const char *name) {
  int i;
  for (i = 0; i < nsv; i++)
    if (!strcmp(sv[i].name, name))
      return &sv[i];
  return NULL;
}

/* pick up new/removed service directories; cheap, callable any time */
static void scan(void) {
  DIR *d;
  struct dirent *e;
  struct stat st;
  Service *s;
  int i;

  if (!(d = opendir(svroot))) {
    fprintf(stderr, "hsmd: cannot open %s: %s\n", svroot, strerror(errno));
    return;
  }
  for (i = 0; i < nsv; i++)
    sv[i].seen = 0;
  while ((e = readdir(d))) {
    size_t len = strlen(e->d_name);
    if (e->d_name[0] == '.' || len >= NAMEMAX)
      continue;
    if (stat(svpath(e->d_name, "run"), &st) < 0 || !(st.st_mode & S_IXUSR))
      continue;
    if ((s = findsv(e->d_name))) {
      s->seen = 1;
      continue;
    }
    if (nsv >= MAXSV) {
      fprintf(stderr, "hsmd: too many services, ignoring %s\n", e->d_name);
      continue;
    }
    s = &sv[nsv++];
    memset(s, 0, sizeof(*s));
    memcpy(s->name, e->d_name, len + 1);
    s->outfd = s->infd = s->log.fd = -1;
    s->state = SDOWN;
    s->want = stat(svpath(s->name, "down"), &st) == 0 ? WANTDOWN : WANTUP;
    s->seen = 1;
    fprintf(stderr, "hsmd: new service %s (want %s)\n", s->name,
            s->want == WANTUP ? "up" : "down");
  }
  closedir(d);

  /* a vanished directory means: stop the service, forget it once dead */
  for (i = 0; i < nsv;) {
    if (!sv[i].seen) {
      sv[i].want = WANTDOWN;
      if (!sv[i].pid) {
        fprintf(stderr, "hsmd: dropping removed service %s\n", sv[i].name);
        if (sv[i].outfd >= 0) {
          close(sv[i].outfd);
          close(sv[i].infd);
        }
        logclose(&sv[i].log);
        sv[i] = sv[--nsv];
        continue;
      }
    }
    i++;
  }
}

/* one pipe + log per service, created on first start and kept forever */
static void openlog(Service *s) {
  int p[2];

  if (s->outfd >= 0)
    return;
  if (pipe2(p, O_CLOEXEC) < 0) {
    fprintf(stderr, "hsmd: pipe %s: %s\n", s->name, strerror(errno));
    return; /* service inherits hsmd's stderr instead */
  }
  fcntl(p[0], F_SETFL, O_NONBLOCK); /* read end only: pipe ends are
                                     * separate open file descriptions */
  s->outfd = p[0];
  s->infd = p[1];
  logopen(&s->log, svpath(s->name, NULL));
}

static void drainlog(Service *s) {
  char buf[4096];
  ssize_t n;

  while (s->outfd >= 0 && (n = read(s->outfd, buf, sizeof(buf))) > 0)
    logwrite(&s->log, buf, (size_t)n);
}

static void start(Service *s) {
  sigset_t empty;
  pid_t pid;

  openlog(s);
  pid = fork();
  if (pid < 0) {
    fprintf(stderr, "hsmd: fork %s: %s\n", s->name, strerror(errno));
    s->deadline = now() + throttle; /* retry later */
    return;
  }
  if (pid == 0) {
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    setsid(); /* own session + process group: kill(-pid) hits the tree */
    if (chdir(svpath(s->name, NULL)) < 0)
      _exit(127);
    if (s->infd >= 0) { /* dup2 clears O_CLOEXEC on the copies */
      dup2(s->infd, 1);
      dup2(s->infd, 2);
    }
    execl("./run", "./run", (char *)NULL);
    _exit(127);
  }
  s->pid = pid;
  s->state = SUP;
  s->started = now();
  s->deadline = 0;
  fprintf(stderr, "hsmd: %s started (pid %d)\n", s->name, (int)pid);
}

/* ask a running service to stop; tick() escalates to SIGKILL later */
static void stop(Service *s) {
  if (!s->pid || s->state == STERM)
    return;
  kill(-s->pid, SIGTERM);
  s->state = STERM;
  s->deadline = now() + killwait;
}

static void reap(void) {
  int status, i;
  pid_t pid;
  long ran;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (i = 0; i < nsv; i++)
      if (sv[i].pid == pid)
        break;
    if (i == nsv)
      continue; /* reparented grandchild (we are a subreaper): just reap */
    ran = now() - sv[i].started;
    fprintf(stderr, "hsmd: %s exited (status %d) after %lds\n", sv[i].name,
            WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
            ran);
    sv[i].pid = 0;
    sv[i].state = SDOWN;
    /* crashed quickly? wait out the rest of the throttle window */
    sv[i].deadline = ran < throttle ? now() + (throttle - ran) : 0;
  }
}

/* enforce want vs state on every service; returns poll timeout in ms
 * until the next timed action (-1 = nothing pending) */
static int tick(void) {
  long t = now(), next = LONG_MAX;
  Service *s;
  int i;

  for (i = 0; i < nsv; i++) {
    s = &sv[i];
    if (s->state == STERM && s->pid && t >= s->deadline) {
      fprintf(stderr, "hsmd: %s ignored SIGTERM, sending SIGKILL\n", s->name);
      kill(-s->pid, SIGKILL);
      s->deadline = t + killwait;
    } else if (s->state == SUP && s->want == WANTDOWN) {
      stop(s);
    } else if (s->state == SDOWN && s->want == WANTUP && !shuttingdown &&
               t >= s->deadline) {
      start(s);
    }
    if (s->state == STERM || (s->state == SDOWN && s->want == WANTUP &&
                              !shuttingdown && s->deadline > t))
      if (s->deadline < next)
        next = s->deadline;
  }
  return next == LONG_MAX ? -1 : (int)(next - t) * 1000;
}

static int alldead(void) {
  int i;
  for (i = 0; i < nsv; i++)
    if (sv[i].pid)
      return 0;
  return 1;
}

static const char *statename(const Service *s) {
  if (s->state == SUP)
    return "run";
  if (s->state == STERM)
    return "term";
  return s->want == WANTUP ? "wait" : "down";
}

static void status(int fd) {
  int i;
  for (i = 0; i < nsv; i++) {
    dprintf(fd, "%-*s %-5s", NAMEMAX / 2, sv[i].name, statename(&sv[i]));
    if (sv[i].pid)
      dprintf(fd, " pid %-6d up %lds", (int)sv[i].pid, now() - sv[i].started);
    dprintf(fd, "\n");
  }
  if (!nsv)
    dprintf(fd, "no services in %s\n", svroot);
}

/* one client = one line in, reply out, close.  Blocking with short socket
 * timeouts: a control socket does not need an async client state machine. */
static void handleclient(void) {
  struct timeval tv = {2, 0};
  char buf[512], *cmd, *arg;
  Service *s;
  ssize_t n;
  int fd;

  if ((fd = accept(lsock, NULL, NULL)) < 0)
    return;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  fcntl(fd, F_SETFD, FD_CLOEXEC);

  if ((n = read(fd, buf, sizeof(buf) - 1)) <= 0) {
    close(fd);
    return;
  }
  buf[n] = '\0';
  cmd = strtok(buf, " \t\n");
  arg = strtok(NULL, " \t\n");

  if (!cmd || !strcmp(cmd, "status")) {
    status(fd);
  } else if (!strcmp(cmd, "rescan")) {
    scan();
    dprintf(fd, "ok\n");
  } else if (!strcmp(cmd, "up") || !strcmp(cmd, "down") ||
             !strcmp(cmd, "restart")) {
    if (!arg) {
      dprintf(fd, "err: %s needs a service name\n", cmd);
    } else if (!(s = findsv(arg))) {
      dprintf(fd, "err: no such service: %s\n", arg);
    } else {
      if (!strcmp(cmd, "down")) {
        s->want = WANTDOWN;
      } else { /* up and restart both end wanting up */
        s->want = WANTUP;
        s->deadline = 0; /* explicit request skips the throttle wait */
        if (!strcmp(cmd, "restart"))
          stop(s); /* dies, then tick() restarts it because want is up */
      }
      dprintf(fd, "ok\n");
    }
  } else {
    dprintf(fd, "err: unknown command: %s\n", cmd);
  }
  close(fd);
}

static void handlesignals(void) {
  struct signalfd_siginfo si;
  int i;

  while (read(sigfd, &si, sizeof(si)) == sizeof(si)) {
    switch ((int)si.ssi_signo) {
    case SIGCHLD:
      reap();
      break;
    case SIGHUP:
      scan();
      break;
    case SIGTERM:
    case SIGINT:
      fprintf(stderr, "hsmd: shutting down\n");
      shuttingdown = 1;
      for (i = 0; i < nsv; i++)
        sv[i].want = WANTDOWN;
      break;
    }
  }
}

static void opensignals(void) {
  sigset_t mask;

  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigaddset(&mask, SIGHUP);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigprocmask(SIG_BLOCK, &mask, NULL);
  signal(SIGPIPE, SIG_IGN);
  if ((sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC)) < 0)
    die("signalfd");
}

static void opensocket(void) {
  struct sockaddr_un sa;
  char dir[PATH_MAX], *slash;
  int fd;

  if (strlen(sockfile) >= sizeof(sa.sun_path)) {
    fprintf(stderr, "hsmd: socket path too long: %s\n", sockfile);
    exit(1);
  }
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  memcpy(sa.sun_path, sockfile, strlen(sockfile) + 1);

  /* a connectable socket means another hsmd owns this path */
  if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
    die("socket");
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
    fprintf(stderr, "hsmd: already running on %s\n", sockfile);
    exit(1);
  }
  close(fd);

  snprintf(dir, sizeof(dir), "%s", sockfile);
  if ((slash = strrchr(dir, '/')))
    *slash = '\0';
  mkpath(dir);
  unlink(sockfile); /* stale leftover from a crash */

  if ((lsock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
    die("socket");
  fcntl(lsock, F_SETFD, FD_CLOEXEC);
  if (bind(lsock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    die("bind");
  if (listen(lsock, 8) < 0)
    die("listen");
}

int main(void) {
  struct pollfd pfd[2 + MAXSV];
  int svof[2 + MAXSV]; /* pfd slot -> sv[] index */
  int i, nfds;

  homepath(svroot, sizeof(svroot), svdir);
  homepath(sockfile, sizeof(sockfile), sockpath);
  mkpath(svroot);
  opensignals();
  opensocket();

  /* orphans of double-forking services reparent to us, not to init,
   * so reap() can pick them up instead of leaving zombies around */
  prctl(PR_SET_CHILD_SUBREAPER, 1);

  fprintf(stderr, "hsmd: supervising %s\n", svroot);
  scan();

  pfd[0].fd = sigfd;
  pfd[0].events = POLLIN;
  pfd[1].fd = lsock;
  pfd[1].events = POLLIN;

  for (;;) {
    int timeout = tick();
    if (shuttingdown && alldead())
      break;
    /* the service pipes come and go, so rebuild their slots every round */
    for (i = 0, nfds = 2; i < nsv; i++)
      if (sv[i].outfd >= 0) {
        pfd[nfds].fd = sv[i].outfd;
        pfd[nfds].events = POLLIN;
        svof[nfds++] = i;
      }
    if (poll(pfd, (nfds_t)nfds, timeout) < 0) {
      if (errno == EINTR)
        continue;
      die("poll");
    }
    /* drain pipes first: handlesignals/handleclient may rescan and
     * reorder sv[], which would invalidate the svof mapping */
    for (i = 2; i < nfds; i++)
      if (pfd[i].revents & POLLIN)
        drainlog(&sv[svof[i]]);
    if (pfd[0].revents & POLLIN)
      handlesignals();
    if (pfd[1].revents & POLLIN)
      handleclient();
  }

  for (i = 0; i < nsv; i++)
    drainlog(&sv[i]); /* catch the services' last words */
  unlink(sockfile);
  fprintf(stderr, "hsmd: bye\n");
  return 0;
}
