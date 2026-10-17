/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal environment for the SPI-NAND DMA error tails and ready-wait function
 * cut from the prepared U-Boot or Linux tree (see tests/test_platform_bmt.py). */
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
typedef unsigned char u8;
static jmp_buf stopped;
static int panics, mode_off, status_value, read_error;
static void panic(const char *fmt, ...) __attribute__((noreturn));
static void panic(const char *fmt, ...) { (void)fmt; panics++; longjmp(stopped,1); }
#define SPI_MODE_MANUAL 0
#define STATUS_BUSY 1
#define SPINAND_WAITRDY_TIMEOUT_MS 3
struct spinand_device { void *spimem; u8 *scratchbuf; };
static int spinand_read_status(struct spinand_device *s, u8 *out)
{ (void)s; *out=status_value; return read_error; }
static int airoha_snand_set_mode(void *ctx, int mode)
{ (void)ctx; (void)mode; mode_off++; return 0; }
static unsigned long ticks;
static void udelay(unsigned long n) { (void)n; ticks++; }
#ifdef TEST_UBOOT
static void schedule(void) { }
static unsigned long get_timer(unsigned long start) { return ++ticks-start; }
#else
/* Linux patch 0012: spinand_wait() busy-polls the status register. */
#define ETIMEDOUT 110
#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, before, args...) \
({ unsigned long _left = (timeout_us) / ((delay_us) ? (delay_us) : 1); int _rc; \
   (void)(before); \
   for (;;) { \
       (val) = op(args); \
       if (cond) { _rc = 0; break; } \
       if (!_left--) { (val) = op(args); _rc = (cond) ? 0 : -ETIMEDOUT; break; } \
       udelay(delay_us); \
   } \
   _rc; })
#endif
