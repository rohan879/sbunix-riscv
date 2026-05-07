#include <procfs_internal.h>

/* ENH: Procfs diagnostic counters stay deliberately simple for SBUnix's
 * single-hart execution model.  Interrupt handlers increment these directly. */
uint64_t procfs_diag_timer_interrupts;
uint64_t procfs_diag_external_interrupts;
uint64_t procfs_diag_uart_interrupts;
