#include <stdarg.h>
#include "types.h"
#include "stat.h"
#include "user.h"

static void
putc(int fd, char c)
{
  write(fd, &c, 1);
}

// xx is always the full pointer-width value here - see the comment on
// printf's %p handling below.
static void
printint(int fd, uintp xx, int base, int sgn)
{
  static char digits[] = "0123456789ABCDEF";
  char buf[24];
  int i, neg;
  uintp x;

  neg = 0;
  if(sgn && (long)xx < 0){
    neg = 1;
    x = -(long)xx;
  } else {
    x = xx;
  }

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);
  if(neg)
    buf[i++] = '-';

  while(--i >= 0)
    putc(fd, buf[i]);
}

// Print to the given fd. Only understands %d, %x, %p, %s, %c.
//
// %d and %x read a plain int/uint (4 bytes); %p reads a full uintp
// (pointer-width) - the two must be kept distinct on the 64-bit build,
// where they're different sizes, so every %p call site needs to pass a
// pointer-width value, not a plain int.
void
printf(int fd, const char *fmt, ...)
{
  va_list ap;
  char *s;
  int c, i, state;

  va_start(ap, fmt);
  state = 0;
  for(i = 0; fmt[i]; i++){
    c = fmt[i] & 0xff;
    if(state == 0){
      if(c == '%'){
        state = '%';
      } else {
        putc(fd, c);
      }
    } else if(state == '%'){
      if(c == 'd'){
        printint(fd, va_arg(ap, int), 10, 1);
      } else if(c == 'x'){
        printint(fd, va_arg(ap, uint), 16, 0);
      } else if(c == 'p'){
        printint(fd, va_arg(ap, uintp), 16, 0);
      } else if(c == 's'){
        s = va_arg(ap, char*);
        if(s == 0)
          s = "(null)";
        while(*s != 0){
          putc(fd, *s);
          s++;
        }
      } else if(c == 'c'){
        putc(fd, va_arg(ap, int));
      } else if(c == '%'){
        putc(fd, c);
      } else {
        // Unknown % sequence.  Print it to draw attention.
        putc(fd, '%');
        putc(fd, c);
      }
      state = 0;
    }
  }
  va_end(ap);
}
