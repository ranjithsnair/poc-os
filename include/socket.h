// AF_UNIX stream sockets with SCM_RIGHTS fd-passing (GUI roadmap phase
// 3 - see /Users/ranjith/.claude/plans/structured-stargazing-pixel.md
// for the full design rationale). Deliberately message-boundary
// preserving rather than a true byte stream: every sendmsg()/write()
// queues one whole message that one recvmsg()/read() consumes whole.
// This is exactly how a real Wayland client/compositor already uses
// the socket (each read is sized to one message), so it doesn't block
// a future real port, but it means these sockets aren't POSIX-general-
// purpose - fine, since nothing in this codebase needs them to be.

// sockconn (below) is kalloc()'d as a single 4096-byte page, the same
// way kernel/pipe.c's struct pipe is - so SOCKMSGSIZE*SOCKQLEN*2
// (both directions) plus its own bookkeeping has to fit in one page.
// 512 matches PIPESIZE's own existing precedent and comfortably covers
// real Wayland wire-protocol traffic (individual requests/events are
// typically well under this; a genuinely large payload like a keymap
// already goes over a separate shm-backed fd in real Wayland, not
// through the socket itself - see include/shm.h).
#define SOCKMSGSIZE 512
#define SOCKQLEN    3      // queued messages per direction
#define MAXFDMSG    4      // fds passable in one sendmsg
#define SUNPATHMAX  108    // matches musl's struct sockaddr_un.sun_path
#define SOCKBACKLOG 8      // max pending (connect()ed, not yet accept()ed)
#define SOCKBINDMAX 16     // max simultaneously bound listening sockets
#define NSOCKET     32     // system-wide struct socket slots - mirrors
                             // kernel/file.c's ftable[NFILE] pattern

struct sockmsg {
  char data[SOCKMSGSIZE];
  int len;
  int nfds;
  struct file *fds[MAXFDMSG];   // already filedup()'d references
};

// The shared "pipe" for one connected STREAM pair - kalloc()'d once
// (mirrors struct pipe in kernel/pipe.c exactly), pointed at by both
// endpoints' struct socket, freed only once both sides have closed.
struct sockconn {
  struct spinlock lock;
  int open[2];                    // is each end still open
  struct sockmsg q[2][SOCKQLEN];  // q[0]: end0->end1, q[1]: end1->end0
  int head[2], tail[2], count[2];
};

enum sockstate { SOCK_UNBOUND, SOCK_LISTENING, SOCK_CONNECTED };

// The per-file-descriptor object. Either a listening socket (path +
// backlog of pending sockconns, filled by connect()/drained by
// accept()) or a connected endpoint (conn + which end it is).
struct socket {
  int inuse;
  enum sockstate state;
  char path[SUNPATHMAX];

  // SOCK_LISTENING only:
  struct spinlock listenlock;
  struct sockconn *backlog[SOCKBACKLOG];
  int bhead, btail, bcount;

  // SOCK_CONNECTED only:
  struct sockconn *conn;
  int end;                        // 0 or 1 - which side of conn
};
