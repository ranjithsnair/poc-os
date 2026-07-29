// kill: send the kill system call to each pid given on the command
// line (see kill() in proc.c - it just sets a flag; the target process
// only actually dies next time it enters or leaves the kernel).

#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char **argv)
{
  int i;

  if(argc < 2){
    printf(2, "usage: kill pid...\n");
    exit();
  }
  for(i=1; i<argc; i++)
    kill(atoi(argv[i]));
  exit();
}
