/* 8253/8254 Programmable Interval Timer, used as the tick source. */
#ifndef PIT_H
#define PIT_H

#include <stdint.h>
#include "isr.h"

/* Programs channel 0 to fire IRQ0 at approximately `frequency_hz` Hz. */
void pit_init(uint32_t frequency_hz);

uint64_t pit_get_ticks(void);

/* Registers a function to run on every tick, after the counter is
 * incremented, with the same `struct registers *` the IRQ0 handler
 * itself received (pointing at the interrupted context, still sitting
 * on the kernel stack that irq_common_stub will POP_ALL+iretq when this
 * call chain returns). process.c uses this to drive preemption without
 * pit.c needing to know processes exist. */
void pit_set_tick_callback(irq_handler_t callback);

#endif
