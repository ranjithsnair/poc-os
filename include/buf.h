// A cached copy of one disk block (see the buffer-cache overview at the
// top of bio.c). prev/next thread every buf into bio.c's single LRU
// list; qnext separately threads whichever bufs are currently queued up
// waiting on the IDE driver (ide.c) - a buf can be on both lists at once.
struct buf {
  int flags;
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  struct buf *qnext; // disk queue
  uchar data[BSIZE];
};
#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk

