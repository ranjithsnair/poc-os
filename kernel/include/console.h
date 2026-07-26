/*
 * Line discipline sitting between the keyboard IRQ handler and SYS_READ
 * on fd 0. There's exactly one console/tty in this kernel (no ptys, no
 * multiple terminals), so its state -- canonical vs. raw mode, echo, and
 * which pid Ctrl-C's SIGINT targets -- is a single global rather than
 * per-fd, exposed here for syscall.c's SYS_IOCTL dispatch.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);

/* Called by keyboard.c's IRQ1 handler for every decoded byte (including
 * synthesized control bytes -- Ctrl-<letter> maps to bytes 1-26, same as
 * any real tty). 0x03 (ETX, Ctrl-C) is always the interrupt character
 * regardless of canonical/raw mode: it's never buffered as input, and
 * instead raises SIGINT on the foreground pid (see
 * console_set_foreground_pid()), same as a real terminal driver.
 * Otherwise: in canonical mode (the default), handles backspace, treats
 * 0x04 (EOT, Ctrl-D) as the EOF character (see console_consume_eof()),
 * and buffers a line at a time, available to console_read_nonblock()
 * once Enter is pressed; in raw mode (POC_ICANON clear), every byte is
 * available immediately (including 0x04 -- EOF is a canonical-mode-only
 * concept, same as a real tty). Echoes what it buffers/consumes iff
 * POC_ECHO is set (also the default). */
void console_feed_char(char c);

/* The pid Ctrl-C's SIGINT is delivered to (0 = none -- Ctrl-C is
 * swallowed silently, same as a terminal with no foreground process
 * group). A shell sets this to its own pid once it becomes the
 * controlling process, and to a foreground child's pid while that child
 * runs, restoring it once the child exits/stops -- exposed to userspace
 * via SYS_IOCTL's TIOCSPGRP/TIOCGPGRP, mirroring real tcsetpgrp()/
 * tcgetpgrp(), just addressed by a single pid instead of a process group
 * (this kernel doesn't have process groups -- see the "minimal job
 * control" scope this was built to). */
uint64_t console_get_foreground_pid(void);
void console_set_foreground_pid(uint64_t pid);

/* The c_lflag bits (POC_ICANON/POC_ECHO) governing console_feed_char()'s
 * behavior, as above. */
uint32_t console_get_lflag(void);
void console_set_lflag(uint32_t lflag);

/* Copies up to len bytes of already-finalized input into buf, consuming
 * them. Returns the number of bytes copied, which is 0 if no finalized
 * line data is available yet -- this never blocks, so it's safe to call
 * from SYS_READ's syscall-context handler (see the comment in
 * syscall.c about why a blocking read would deadlock there). */
uint64_t console_read_nonblock(uint8_t *buf, uint64_t len);

/* Consumes one pending Ctrl-D-on-an-empty-line EOF marker (see
 * console_feed_char()), if any: returns 1 and clears it, or 0 if none is
 * pending. process_fd_read() calls this only once console_read_nonblock()
 * has confirmed there's no real buffered data left, so any earlier
 * finalized line is always drained first -- an EOF marker only ever
 * reports *after* everything typed before it. Each Ctrl-D-on-empty-line
 * press queues one more marker (mirroring a real tty: EOF ends whatever
 * read() call is waiting, not the fd itself, so typing can resume
 * afterward and a later read() call will just wait for it normally). */
int console_consume_eof(void);

#endif
