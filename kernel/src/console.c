#include <stdint.h>
#include "console.h"
#include "serial.h"
#include "process.h"
#include "syscall.h"

#define CONSOLE_LINE_MAX 256
#define CONSOLE_RING_SIZE 1024

static uint32_t lflag = POC_ICANON | POC_ECHO;
static uint64_t foreground_pid = 0;

uint64_t console_get_foreground_pid(void) { return foreground_pid; }
void console_set_foreground_pid(uint64_t pid) { foreground_pid = pid; }
uint32_t console_get_lflag(void) { return lflag; }
void console_set_lflag(uint32_t new_lflag) { lflag = new_lflag; }

/* The line currently being typed (not yet visible to console_read_nonblock()). */
static uint8_t line_buf[CONSOLE_LINE_MAX];
static uint64_t line_len = 0;

/* Finalized lines, end to end (including their trailing '\n'), waiting
 * to be read. A plain wraparound ring buffer: ring_head is the oldest
 * unread byte, ring_count is how many unread bytes are buffered. */
static uint8_t ring[CONSOLE_RING_SIZE];
static uint64_t ring_head = 0;
static uint64_t ring_count = 0;

static void ring_push(uint8_t byte) {
    if (ring_count == CONSOLE_RING_SIZE) {
        return; /* full: drop rather than overwrite unread data */
    }
    uint64_t tail = (ring_head + ring_count) % CONSOLE_RING_SIZE;
    ring[tail] = byte;
    ring_count++;
}

void console_init(void) {
    line_len = 0;
    ring_head = 0;
    ring_count = 0;
}

void console_feed_char(char c) {
    if (c == 0x03) { /* ETX / Ctrl-C: the interrupt character, in any mode */
        line_len = 0; /* a real tty discards whatever was mid-line too */
        if (lflag & POC_ECHO) {
            serial_print("^C\n");
        }
        if (foreground_pid != 0) {
            process_send_signal(foreground_pid, SIGINT);
        }
        return;
    }

    if (!(lflag & POC_ICANON)) {
        /* Raw mode: every byte is available immediately, never buffered
         * until a newline. */
        if (lflag & POC_ECHO) {
            serial_putc(c);
        }
        ring_push((uint8_t)c);
        return;
    }

    if (c == '\b') {
        if (line_len > 0) {
            line_len--;
            if (lflag & POC_ECHO) {
                serial_putc('\b');
                serial_putc(' ');
                serial_putc('\b');
            }
        }
        return;
    }

    if (line_len < CONSOLE_LINE_MAX) {
        line_buf[line_len++] = (uint8_t)c;
    }
    if (lflag & POC_ECHO) {
        serial_putc(c);
    }

    if (c == '\n') {
        for (uint64_t i = 0; i < line_len; i++) {
            ring_push(line_buf[i]);
        }
        line_len = 0;
    }
}

uint64_t console_read_nonblock(uint8_t *buf, uint64_t len) {
    uint64_t n = 0;
    while (n < len && ring_count > 0) {
        buf[n++] = ring[ring_head];
        ring_head = (ring_head + 1) % CONSOLE_RING_SIZE;
        ring_count--;
    }
    return n;
}
