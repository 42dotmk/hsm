/* log.c - rotated per-service logs for hsmd.
 *
 * A service's stdout/stderr flows through a pipe into hsmd, which hands each
 * chunk to logwrite().  Chunks append to <servicedir>/log; when that grows
 * past logsize (config.h) the files shuffle down one slot:
 *
 *   log -> log.0 -> log.1 -> ... -> log.<logkeep-1> -> deleted
 *
 * Rotation happens between chunks, so a line can be split across two files
 * when a service is mid-write at the boundary - fine for a service log.
 * The size is remembered across hsmd restarts by stat()ing the file on open.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

/* "<dir>/log" or "<dir>/log.<i>" for i >= 0, into caller storage */
static const char *logname(const Log *l, int i, char *buf, size_t len) {
  if (i < 0)
    snprintf(buf, len, "%s/log", l->dir);
  else
    snprintf(buf, len, "%s/log.%d", l->dir, i);
  return buf;
}

static int openfile(Log *l) {
  char path[PATH_MAX + 32];
  struct stat st;

  logname(l, -1, path, sizeof(path));
  l->fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  l->size = l->fd >= 0 && fstat(l->fd, &st) == 0 ? (long)st.st_size : 0;
  return l->fd;
}

int logopen(Log *l, const char *dir) {
  snprintf(l->dir, sizeof(l->dir), "%s", dir);
  return openfile(l);
}

static void rotate(Log *l) {
  char from[PATH_MAX + 32], to[PATH_MAX + 32];
  int i;

  close(l->fd);
  for (i = logkeep - 1; i > 0; i--)
    rename(logname(l, i - 1, from, sizeof(from)),
           logname(l, i, to, sizeof(to)));
  if (logkeep > 0)
    rename(logname(l, -1, from, sizeof(from)), logname(l, 0, to, sizeof(to)));
  else
    unlink(logname(l, -1, from, sizeof(from)));
  openfile(l);
}

void logwrite(Log *l, const char *buf, size_t n) {
  ssize_t w;

  if (l->fd < 0)
    return;
  if (l->size + (long)n > logsize)
    rotate(l);
  w = write(l->fd, buf, n);
  if (w > 0)
    l->size += w;
}

void logclose(Log *l) {
  if (l->fd >= 0)
    close(l->fd);
  l->fd = -1;
}
