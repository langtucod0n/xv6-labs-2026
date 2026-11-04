#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

void
run_exec(char *cmd_args[], int cmd_argc, char *file_path)
{
  char *argv[MAXARG];
  int i;
  int pid;

  if (cmd_argc + 2 > MAXARG) {
    fprintf(2, "find: too many arguments for -exec\n");
    return;
  }

  for (i = 0; i < cmd_argc; i++) {
    argv[i] = cmd_args[i];
  }
  argv[cmd_argc] = file_path;
  argv[cmd_argc + 1] = 0;

  pid = fork();
  if (pid < 0) {
    fprintf(2, "find: fork failed\n");
    return;
  }
  if (pid == 0) {
    exec(argv[0], argv);
    fprintf(2, "find: exec %s failed\n", argv[0]);
    exit(1);
  }
  wait(0);
}

void
find(char *path, char *name, char **exec_args, int exec_argc)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if ((fd = open(path, O_RDONLY)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  if (st.type != T_DIR) {
    close(fd);
    return;
  }

  if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
    fprintf(2, "find: path too long\n");
    close(fd);
    return;
  }

  strcpy(buf, path);
  p = buf + strlen(buf);
  if (*(p - 1) != '/') {
    *p++ = '/';
  }

  while (read(fd, &de, sizeof(de)) == sizeof(de)) {
    if (de.inum == 0)
      continue;

    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = 0;

    // Do not recurse into or match "." and ".."
    if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0)
      continue;

    if (stat(buf, &st) < 0) {
      fprintf(2, "find: cannot stat %s\n", buf);
      continue;
    }

    // Match both regular files and directories (and devices)
    if (strcmp(p, name) == 0) {
      if (exec_argc > 0) {
        run_exec(exec_args, exec_argc, buf);
      } else {
        printf("%s\n", buf);
      }
    }

    // Recursively search subdirectories
    if (st.type == T_DIR) {
      find(buf, name, exec_args, exec_argc);
    }
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  char **exec_args = 0;
  int exec_argc = 0;

  if (argc < 3) {
    fprintf(2, "Usage: find <path> <name> [-exec <cmd> [args...]]\n");
    exit(1);
  }

  if (argc > 3) {
    if (strcmp(argv[3], "-exec") != 0 || argc == 4) {
      fprintf(2, "Usage: find <path> <name> [-exec <cmd> [args...]]\n");
      exit(1);
    }
    exec_args = &argv[4];
    exec_argc = argc - 4;
  }

  find(argv[1], argv[2], exec_args, exec_argc);
  exit(0);
}
