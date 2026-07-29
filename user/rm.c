// rm: unlink each named file. Removes its directory entry and, once its
// link count and open-reference count both reach zero, its contents
// (see iput() in fs.c) - so a file still open elsewhere survives until
// that last reference is closed.

#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    printf(2, "Usage: rm files...\n");
    exit();
  }

  for(i = 1; i < argc; i++){
    if(unlink(argv[i]) < 0){
      printf(2, "rm: %s failed to delete\n", argv[i]);
      break;
    }
  }

  exit();
}
