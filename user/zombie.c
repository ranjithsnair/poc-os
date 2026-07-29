// Create a zombie process that
// must be reparented at exit.
//
// The child (fork() returns 0 there) exits immediately, while the
// parent sleeps first instead of calling wait() right away - so for
// those 5 ticks the child sits as a ZOMBIE, exited but not yet reaped,
// demonstrating that state (see the ZOMBIE case in proc.c's wait()).

#include "types.h"
#include "stat.h"
#include "user.h"

int
main(void)
{
  if(fork() > 0)
    sleep(5);  // Let child exit before parent.
  exit();
}
