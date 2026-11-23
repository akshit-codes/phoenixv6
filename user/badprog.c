// badprog: a program that damages files, for trying out undo.
//
//   badprog setup   create a.txt, b.txt and c.txt with known contents
//   badprog         overwrite part of a.txt, empty b.txt and put
//                   something else in it, delete c.txt, create d.txt
//                   and the directory newdir, then fail (exit 1)
//   badprog ok      the same, but exit 0

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static void
put(char *path, int flags, char *s)
{
  int fd = open(path, flags);

  if (fd < 0 || write(fd, s, strlen(s)) != strlen(s)) {
    fprintf(2, "badprog: cannot write %s\n", path);
    exit(2);
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if (argc == 2 && strcmp(argv[1], "setup") == 0) {
    put("a.txt", O_CREATE | O_WRONLY | O_TRUNC, "alpha one\nalpha two\nalpha three\n");
    put("b.txt", O_CREATE | O_WRONLY | O_TRUNC, "bravo one\nbravo two\n");
    put("c.txt", O_CREATE | O_WRONLY | O_TRUNC, "charlie\n");
    exit(0);
  }

  put("a.txt", O_WRONLY, "XXXXXXXXX");               // overwrite the start
  put("b.txt", O_WRONLY | O_TRUNC, "all gone\n");    // truncate, then write
  if (unlink("c.txt") < 0)                           // delete
    fprintf(2, "badprog: cannot delete c.txt\n");
  put("d.txt", O_CREATE | O_WRONLY, "new file\n");   // create
  mkdir("newdir");
  put("newdir/e.txt", O_CREATE | O_WRONLY, "inside\n");

  if (argc == 2 && strcmp(argv[1], "ok") == 0) {
    printf("badprog: finished\n");
    exit(0);
  }
  printf("badprog: something went wrong\n");
  exit(1);
}
