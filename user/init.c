// init: The initial user-level program

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

char *argv[] = { "sh", 0 };

int
main(void)
{
  int pid, wpid;

  // initcode.asm execs "/usr/bin/init" with no open file descriptors at
  // all, so this open() is guaranteed to become fd 0; the two dup()s
  // below then give fds 1 and 2 (stdout/stderr) the same underlying
  // console file.
  if(open("console", O_RDWR) < 0){
    mknod("console", 1, 1);
    open("console", O_RDWR);
  }
  dup(0);  // stdout
  dup(0);  // stderr

  for(;;){
    printf(1, "init: starting sh\n");
    pid = fork();
    if(pid < 0){
      printf(1, "init: fork failed\n");
      exit();
    }
    if(pid == 0){
      exec("/usr/bin/sh", argv);
      printf(1, "init: exec sh failed\n");
      exit();
    }
    // Reap every exited child, not just the shell: init inherits any
    // orphaned process whose original parent has already exited (see
    // exit() in proc.c), and as the root of the process tree it must
    // wait() for them so they don't linger as zombies forever.
    while((wpid=wait()) >= 0 && wpid != pid)
      printf(1, "zombie!\n");
  }
}
