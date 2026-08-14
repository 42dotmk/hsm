/* log.h - rotated per-service log, see log.c */
#include <limits.h>

typedef struct {
  char dir[PATH_MAX]; /* the service directory */
  int fd;             /* current log file, -1 when closed */
  long size;          /* bytes written so far, drives rotation */
} Log;

int logopen(Log *l, const char *dir);
void logwrite(Log *l, const char *buf, size_t n);
void logclose(Log *l);
