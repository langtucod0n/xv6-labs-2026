// Shell.
#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void); // Fork but panics on failure.
void panic(char *);
struct cmd *parsecmd(char *);
void runcmd(struct cmd *) __attribute__((noreturn));

static int expand_history(char *buf, int nbuf);
static int is_history_command(char *cmd);
// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if (cmd == 0)
    exit(1);

  switch (cmd->type) {
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd *)cmd;
    if (ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd *)cmd;
    close(rcmd->fd);
    if (open(rcmd->file, rcmd->mode) < 0) {
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd *)cmd;
    if (fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd *)cmd;
    if (pipe(p) < 0)
      panic("pipe");
    if (fork1() == 0) {
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if (fork1() == 0) {
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd *)cmd;
    if (fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

#define MAX_MATCHES 64

static int
isws(char c)
{
  return c == ' ' || c == '\t';
}

static void
complete(char *buf, int *len, int nbuf)
{
  char matches[MAX_MATCHES][DIRSIZ + 1];
  char *dir;
  char name[DIRSIZ + 1];
  char prefix[100];
  struct dirent de;
  int fd, nmatch, i, start, plen, first, namelen;
  int suffixlen, copylen;

  // Find the beginning of the current word.
  start = *len;
  while (start > 0 && !isws(buf[start - 1]))
    start--;

  plen = *len - start;
  if (plen == 0)
    return;

  // The first word is completed from "/".
  first = 1;
  for (i = 0; i < start; i++) {
    if (!isws(buf[i])) {
      first = 0;
      break;
    }
  }

  dir = first ? "/" : ".";

  // Copy the prefix so it is safely NUL-terminated.
  if (plen >= sizeof(prefix))
    return;
  memmove(prefix, buf + start, plen);
  prefix[plen] = 0;

  fd = open(dir, O_RDONLY);
  if (fd < 0)
    return;

  nmatch = 0;
  while (read(fd, &de, sizeof(de)) == sizeof(de)) {
    if (de.inum == 0)
      continue;

    memmove(name, de.name, DIRSIZ);
    name[DIRSIZ] = 0;
    // xv6 directory entry names may be space-padded, not NUL-terminated.
    for (namelen = DIRSIZ; namelen > 0 && name[namelen - 1] == ' '; namelen--)
      name[namelen - 1] = 0;

    // Ignore "." and "..".
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;

    namelen = strlen(name);
    if (namelen < plen)
      continue;

    if (memcmp(name, prefix, plen) != 0)
      continue;
    if (nmatch < MAX_MATCHES) {
      memmove(matches[nmatch], name, namelen + 1);
      nmatch++;
    }
  }
  close(fd);

  if (nmatch == 0)
    return;

  if (nmatch == 1) {
    // Complete the missing suffix and append a space.
    suffixlen = strlen(matches[0]) - plen;
    copylen = suffixlen;

    if (*len + copylen >= nbuf)
      copylen = nbuf - *len - 1;
    if (copylen < 0)
      copylen = 0;

    if (copylen > 0) {
      memmove(buf + *len, matches[0] + plen, copylen);
      write(1, matches[0] + plen, copylen);
      *len += copylen;
    }

    if (*len + 1 < nbuf) {
      buf[(*len)++] = ' ';
      buf[*len] = 0;
      write(1, " ", 1);
    }
    return;
  }

  // Multiple matches: display them on a new line.
  write(1, "\n", 1);
  for (i = 0; i < nmatch; i++) {
    write(1, matches[i], strlen(matches[i]));
    if (i + 1 < nmatch)
      write(1, " ", 1);
  }
  write(1, "\n", 1);

  // Restore the prompt and the command typed so far.
  write(2, "$ ", 2);
  write(1, buf, *len);
}

#define HIST_SIZE 10

#define KEY_UP   0x80
#define KEY_DOWN 0x81

static char history[HIST_SIZE][100];
static int history_count;

static void
save_history(char *cmd)
{
  int idx;
  int len = strlen(cmd);

  while (len > 0 && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r'))
    len--;

  if (len == 0)
    return;

  if (len >= sizeof(history[0]))
    len = sizeof(history[0]) - 1;

  idx = history_count % HIST_SIZE;
  memmove(history[idx], cmd, len);
  history[idx][len] = 0;
  history_count++;
}

static char *
get_history(int number)
{
  int oldest;

  oldest = history_count - HIST_SIZE;
  if (oldest < 0)
    oldest = 0;

  if (number < oldest + 1 || number > history_count)
    return 0;

  return history[(number - 1) % HIST_SIZE];
}

static void
show_history(void)
{
  int oldest, i;
  char numbuf[16];
  int n, value;

  oldest = history_count - HIST_SIZE;
  if (oldest < 0)
    oldest = 0;

  for (i = oldest; i < history_count; i++) {
    value = i + 1;
    n = 0;

    do {
      numbuf[n++] = '0' + value % 10;
      value /= 10;
    } while (value > 0);

    while (n > 0)
      write(1, &numbuf[--n], 1);

    write(1, " ", 1);
    write(1, history[i % HIST_SIZE], strlen(history[i % HIST_SIZE]));
    write(1, "\n", 1);
  }
}

static int
is_history_command(char *cmd)
{
  int n = strlen(cmd);

  while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r' ||
                   cmd[n - 1] == ' ' || cmd[n - 1] == '\t'))
    n--;

  return n == 7 && memcmp(cmd, "history", 7) == 0;
}

static int
expand_history(char *buf, int nbuf)
{
  int n, i, number;
  char *oldcmd;
  char expanded[100];

  n = strlen(buf);
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    n--;
  buf[n] = 0;

  if (buf[0] != '!')
    return 0;

  if (n == 2 && buf[1] == '!') {
    number = history_count;
  } else {
    if (n < 2) {
      fprintf(2, "history: event not found\n");
      return -1;
    }

    number = 0;
    for (i = 1; i < n; i++) {
      if (buf[i] < '0' || buf[i] > '9' || number > 1000000) {
        fprintf(2, "history: event not found\n");
        return -1;
      }
      number = number * 10 + buf[i] - '0';
    }
  }

  oldcmd = get_history(number);
  if (oldcmd == 0) {
    fprintf(2, "history: event not found\n");
    return -1;
  }

  n = strlen(oldcmd);
  if (n > nbuf - 2)
    n = nbuf - 2;
  memmove(expanded, oldcmd, n);
  expanded[n] = 0;

  // Show the expanded command before executing it.
  write(1, expanded, n);
  write(1, "\n", 1);

  memmove(buf, expanded, n);
  buf[n] = '\n';
  buf[n + 1] = 0;
  return 1;
}

// Read one shell command. Console input sends TAB and arrow-key markers
// as soon as they are pressed; redirected input is read normally by lines.
int
getcmd(char *buf, int nbuf)
{
  int i, cc, interactive, key;
  int histpos, oldest;
  char c;
  char *oldcmd;
  struct stat st;

  interactive = fstat(0, &st) == 0 && st.type == T_DEVICE;

  if (interactive)
    write(2, "$ ", 2);

  memset(buf, 0, nbuf);
  i = 0;
  histpos = history_count + 1;
  oldest = history_count - HIST_SIZE + 1;
  if (oldest < 1)
    oldest = 1;

  while (i + 1 < nbuf) {
    cc = read(0, &c, 1);
    if (cc < 1) {
      if (i == 0)
        return -1;
      break;
    }

    key = (unsigned char)c;

    if (key == '\n' || key == '\r')
      break;

    if (key == '\t' && interactive) {
      complete(buf, &i, nbuf);
      continue;
    }

    if (key == KEY_UP && interactive) {
      if (histpos > oldest && history_count > 0) {
        histpos--;
        oldcmd = get_history(histpos);
        if (oldcmd != 0) {
          while (i > 0) {
            write(1, "\b \b", 3);
            i--;
          }
          i = strlen(oldcmd);
          if (i >= nbuf)
            i = nbuf - 1;
          memmove(buf, oldcmd, i);
          buf[i] = 0;
          write(1, buf, i);
        }
      }
      continue;
    }

    if (key == KEY_DOWN && interactive) {
      if (histpos <= history_count) {
        histpos++;
        while (i > 0) {
          write(1, "\b \b", 3);
          i--;
        }

        if (histpos <= history_count) {
          oldcmd = get_history(histpos);
          if (oldcmd != 0) {
            i = strlen(oldcmd);
            if (i >= nbuf)
              i = nbuf - 1;
            memmove(buf, oldcmd, i);
            buf[i] = 0;
            write(1, buf, i);
          }
        } else {
          buf[0] = 0;
        }
      }
      continue;
    }

    if (key == 127 || key == '\b') {
      if (i > 0) {
        i--;
        buf[i] = 0;
        write(1, "\b \b", 3);
      }
      continue;
    }

    if ((key == '\t' || (key >= 32 && key < 127)) && i + 1 < nbuf) {
      buf[i++] = c;
    }
  }

  // The parser expects a newline-terminated command.
  if (i + 1 >= nbuf)
    i = nbuf - 2;
  buf[i++] = '\n';
  buf[i] = 0;

  return 0;
}

int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  while ((fd = open("console", O_RDWR)) >= 0) {
    if (fd >= 3) {
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while (getcmd(buf, sizeof(buf)) >= 0) {
    char *cmd = buf;
    int expanded;
    int len;

    while (*cmd == ' ' || *cmd == '\t')
      cmd++;

    if (*cmd == '\n' || *cmd == 0)
      continue;

    if (cmd != buf) {
      memmove(buf, cmd, strlen(cmd) + 1);
      cmd = buf;
    }

    expanded = expand_history(buf, sizeof(buf));
    if (expanded < 0)
      continue;

    cmd = buf;
    save_history(cmd);

    if (is_history_command(cmd)) {
      show_history();
      continue;
    }

    if (cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' ') {
      len = strlen(cmd);
      if (len > 0 && cmd[len - 1] == '\n')
        cmd[len - 1] = 0;

      if (chdir(cmd + 3) < 0)
        fprintf(2, "cannot cd %s\n", cmd + 3);
    } else {
      if (fork1() == 0)
        runcmd(parsecmd(cmd));
      wait(0);
    }
  }
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if (pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd *
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd *)cmd;
}

struct cmd *
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd *)cmd;
}

struct cmd *
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd *)cmd;
}

struct cmd *
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd *)cmd;
}

struct cmd *
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd *)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while (s < es && strchr(whitespace, *s))
    s++;
  if (q)
    *q = s;
  ret = *s;
  switch (*s) {
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if (*s == '>') {
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if (eq)
    *eq = s;

  while (s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while (s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);
struct cmd *parsepipe(char **, char *);
struct cmd *parseexec(char **, char *);
struct cmd *nulterminate(struct cmd *);

struct cmd *
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if (s != es) {
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd *
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while (peek(ps, es, "&")) {
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if (peek(ps, es, ";")) {
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd *
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if (peek(ps, es, "|")) {
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd *
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while (peek(ps, es, "<>")) {
    tok = gettoken(ps, es, 0, 0);
    if (gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch (tok) {
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE | O_TRUNC, 1);
      break;
    case '+': // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd *
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if (!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if (!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd *
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if (peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd *)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while (!peek(ps, es, "|)&;")) {
    if ((tok = gettoken(ps, es, &q, &eq)) == 0)
      break;
    if (tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if (argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd *
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if (cmd == 0)
    return 0;

  switch (cmd->type) {
  case EXEC:
    ecmd = (struct execcmd *)cmd;
    for (i = 0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd *)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd *)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd *)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd *)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
