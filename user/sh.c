// Shell.
//
// Three stages, bottom of the file to top: gettoken()/peek() tokenize
// the input line without copying it (tokens are just pointers back into
// the original buffer); the parse* functions build a tree of struct cmd
// nodes describing what to run (nulterminate() then patches NUL bytes
// into the original buffer in place of the delimiters the tokenizer
// skipped over, since only at the end do we know a token won't be
// extended further, e.g. by a redirection reusing part of the buffer);
// and runcmd() walks that tree, forking and connecting pipes/redirects
// as needed, calling itself recursively for compound commands (pipes,
// ';'-separated lists, and the parenthesized blocks in parseblock()).
//
// Every struct cmd subtype below starts with the same `int type` field
// at offset 0, so runcmd() and nulterminate() can look at cmd->type
// through the generic struct cmd* before casting to the specific
// subtype that type indicates.

#include "types.h"
#include "user.h"
#include "fcntl.h"
#include "elf.h"
#include "stat.h"

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

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);

// A deliberately tiny stand-in for a real $PATH search: a bare name (no
// '/') that doesn't exist relative to the current directory is looked
// up under /usr/bin instead, where poc-os installs every stock binary
// (see the Makefile's MKFS_INSTALL) - so "true"/"cat"/"sh" keep working
// as typed even though none of them actually live at the process's cwd.
// A name that already contains '/', or that does exist as typed, is
// left alone - cwd always wins over /usr/bin, same as a "./foo" would
// in a real shell.
static char*
resolvepath(char *name, char *buf)
{
  struct stat st;

  if(strchr(name, '/') || stat(name, &st) == 0)
    return name;
  strcpy(buf, "/usr/bin/");
  strcpy(buf + strlen(buf), name);
  return buf;
}

#ifdef X64
// Peeks at path's ELF header to tell a poc-os-native static binary
// (ET_EXEC, entered via plain SYS_exec's argc/argv convention) apart
// from a musl-crt1/PIE binary (ET_DYN - true/false/cat and any future
// GNU coreutils port), which instead needs SYS_execve's Linux-style
// argc/argv/envp/auxv stack and PT_INTERP handling (see include/elf.h's
// own comment on ELF_ET_DYN and kernel/exec.c's execve()) - the same
// distinction musl/test/runmusl.c makes via raw syscalls, but done here
// so plain "true"/"cat" work by name instead of needing a "runmusl"
// prefix.
static int
isdyn(char *path)
{
  int fd, n;
  struct elfhdr eh;

  if((fd = open(path, O_RDONLY)) < 0)
    return 0;
  n = read(fd, &eh, sizeof(eh));
  close(fd);
  return n == sizeof(eh) && eh.magic == ELF_MAGIC && eh.type == ELF_ET_DYN;
}
#endif

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
  char pathbuf[128], *path;

  if(cmd == 0)
    exit();

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit();
    path = resolvepath(ecmd->argv[0], pathbuf);
#ifdef X64
    if(isdyn(path)){
      // Minimal fixed envp, same as musl/test/runmusl.c - poc-os's
      // shell has no environment variables of its own to forward.
      static char *envp[] = { "HOME=/", "PATH=/", 0 };
      execve(path, ecmd->argv, envp);
    } else
#endif
      exec(path, ecmd->argv);
    printf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    // Closing rcmd->fd (0 for "<", 1 for ">"/">>") first frees up
    // exactly that descriptor number, so the open() that follows is
    // guaranteed to reuse it - redirecting that standard fd to the
    // file - rather than landing on some arbitrary next-free fd.
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      printf(2, "open %s failed\n", rcmd->file);
      exit();
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait();
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait();
    wait();
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit();
}

// cwd: this shell's own idea of its current directory, purely so the
// prompt can show it - poc-os has no real getcwd() (no way to turn an
// inode back into a path without directory-reading support, and even
// with that, no reason for every process to redo the walk when this
// one already knows every cd it did itself). Kept as a plain string,
// updated locally after each successful chdir() rather than queried
// from the kernel - accurate for exactly the reason a shell's own cwd
// tracking always is: nothing but this process's own cd command ever
// changes what its chdir() calls resolved against.
#define MAXPATH 512
static char cwd[MAXPATH] = "/";

// Rewrites abspath in place into cwd: splits on '/', drops "." and
// empty components, pops one component per ".." (the actual reason
// this can't just be string concatenation - resolving ".." requires
// already knowing what the prior component was), then rejoins.
static void
setcwd(char *abspath)
{
  char *comp[64];
  int len[64];
  int ncomp = 0;
  char *s = abspath;

  while(*s){
    while(*s == '/')
      s++;
    if(*s == 0)
      break;
    char *start = s;
    while(*s && *s != '/')
      s++;
    int n = s - start;
    if(n == 1 && start[0] == '.'){
      // skip
    } else if(n == 2 && start[0] == '.' && start[1] == '.'){
      if(ncomp > 0)
        ncomp--;
    } else if(ncomp < 64){
      comp[ncomp] = start;
      len[ncomp] = n;
      ncomp++;
    }
  }

  int pos = 0;
  cwd[pos++] = '/';
  for(int i = 0; i < ncomp; i++){
    if(i > 0 && pos < MAXPATH - 1)
      cwd[pos++] = '/';
    int n = len[i];
    if(pos + n >= MAXPATH - 1)
      n = MAXPATH - 1 - pos;
    if(n > 0){
      memmove(cwd + pos, comp[i], n);
      pos += n;
    }
  }
  cwd[pos] = 0;
}

// Combines cwd with a cd argument (absolute or relative) into one
// path and hands it to setcwd() to resolve - called only after a
// chdir() to the same argument already succeeded.
static void
update_cwd(char *arg)
{
  char joined[MAXPATH];

  if(arg[0] == '/'){
    strcpy(joined, arg);
  } else if(cwd[1] == 0){  // cwd is "/"
    joined[0] = '/';
    strcpy(joined + 1, arg);
  } else {
    strcpy(joined, cwd);
    int l = strlen(joined);
    joined[l] = '/';
    strcpy(joined + l + 1, arg);
  }
  setcwd(joined);
}

int
getcmd(char *buf, int nbuf)
{
  printf(2, "%s $ ", cwd);
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;
  return 0;
}

int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  // sh may be exec'd with some (or all) of fds 0/1/2 already closed -
  // e.g. init.c's very first invocation has none open at all - and the
  // rest of the shell assumes stdin/stdout/stderr just work. Opening
  // "console" repeatedly always returns the lowest free fd, so this
  // stops as soon as that reaches 3, having filled in whichever of
  // 0/1/2 were missing (and closing the redundant extra open above it).
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    if(buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' '){
      // Chdir must be called by the parent, not the child.
      buf[strlen(buf)-1] = 0;  // chop \n
      if(chdir(buf+3) < 0)
        printf(2, "cannot cd %s\n", buf+3);
      else
        update_cwd(buf+3);
      continue;
    }
    if(fork1() == 0)
      runcmd(parsecmd(buf));
    wait();
  }
  exit();
}

void
panic(char *s)
{
  printf(2, "%s\n", s);
  exit();
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
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
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
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
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
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
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    printf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
    case '+':  // >> - poc has no O_APPEND, so this opens the same as '>'
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
