/* hsm - control client for hsmd.
 *
 * Joins its arguments into one command line, sends it over the unix socket
 * and prints the reply.  No arguments means "status".  Exits nonzero when
 * the daemon replies with an error or cannot be reached.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.h"

static void homepath(char *dst, size_t len, const char *p) {
  const char *home = getenv("HOME");
  if (p[0] == '/')
    snprintf(dst, len, "%s", p);
  else
    snprintf(dst, len, "%s/%s", home ? home : ".", p);
}

int main(int argc, char *argv[]) {
  struct sockaddr_un sa;
  char cmd[512] = "status", buf[4096];
  int fd, i, err = 0, first = 1;
  size_t off;
  ssize_t n;

  if (argc > 1 && !strcmp(argv[1], "-v")) {
    printf("hsm %s\n", HSM_VERSION);
    return 0;
  }
  if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
    fprintf(stderr, "usage: hsm [-v] [status | up NAME | down NAME | "
                    "restart NAME | rescan]\n");
    return 1;
  }
  if (argc > 1) {
    cmd[0] = '\0';
    for (i = 1, off = 0; i < argc && off < sizeof(cmd); i++)
      off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, "%s%s",
                              i > 1 ? " " : "", argv[i]);
  }

  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  homepath(sa.sun_path, sizeof(sa.sun_path), sockpath);

  if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
      connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    fprintf(stderr, "hsm: cannot reach hsmd at %s: %s\n", sa.sun_path,
            strerror(errno));
    return 1;
  }
  dprintf(fd, "%s\n", cmd);
  shutdown(fd, SHUT_WR);

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    if (first && n >= 4 && !memcmp(buf, "err:", 4))
      err = 1;
    first = 0;
    fwrite(buf, 1, (size_t)n, stdout);
  }
  close(fd);
  return err;
}
