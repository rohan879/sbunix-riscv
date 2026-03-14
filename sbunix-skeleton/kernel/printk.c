#include <printk.h>
#include <uart.h>
#include <stdarg.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Kernel ring buffer for dmesg                                        */
/* ------------------------------------------------------------------ */
#define DMESG_BUFSZ 16384
static char dmesg_buf[DMESG_BUFSZ];
static int dmesg_head;   /* next write position */
static unsigned int dmesg_count;  /* total bytes written (may exceed DMESG_BUFSZ) */

unsigned int dmesg_total(void) {
  return dmesg_count;
}

int dmesg_read(char *buf, int bufsz, int offset) {
  int avail = dmesg_count < DMESG_BUFSZ ? dmesg_count : DMESG_BUFSZ;
  int start_offset = dmesg_count - avail; /* logical offset of oldest byte */

  if (offset < start_offset)
    offset = start_offset;
  if (offset >= dmesg_count)
    return 0;

  int to_copy = dmesg_count - offset;
  if (to_copy > bufsz)
    to_copy = bufsz;

  /* Compute ring buffer position for 'offset' */
  int ring_pos = (dmesg_head - avail + (offset - start_offset)) % DMESG_BUFSZ;
  if (ring_pos < 0) ring_pos += DMESG_BUFSZ;

  int copied = 0;
  while (copied < to_copy) {
    int chunk = DMESG_BUFSZ - ring_pos;
    if (chunk > to_copy - copied)
      chunk = to_copy - copied;
    memcpy(buf + copied, dmesg_buf + ring_pos, chunk);
    copied += chunk;
    ring_pos = (ring_pos + chunk) % DMESG_BUFSZ;
  }
  return copied;
}

// similar to write userspace call but does not use fd number
static void writek(char *buf, int count) {
    for (int i = 0; i < count; i++) {
        uart_write(buf[i]);
        /* Also write to ring buffer */
        dmesg_buf[dmesg_head] = buf[i];
        dmesg_head = (dmesg_head + 1) % DMESG_BUFSZ;
        dmesg_count++;
    }
}

static void emit_buf(char *buf, int *pos, int *total, char c)
{
	buf[(*pos)++] = c;
	if (*pos >= 1020) {
		writek(buf, *pos);
		*total += *pos;
		*pos = 0;
	}
}

static void emit_str(char *buf, int *pos, int *total, const char *s)
{
	while (*s)
		emit_buf(buf, pos, total, *s++);
}

static void emit_unsigned(char *buf, int *pos, int *total,
			  unsigned long val, int base, int upper)
{
	char tmp[20];
	int i = 0;
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	if (val == 0) {
		emit_buf(buf, pos, total, '0');
		return;
	}
	while (val > 0) {
		tmp[i++] = digits[val % base];
		val /= base;
	}
	while (i > 0)
		emit_buf(buf, pos, total, tmp[--i]);
}

static void emit_signed(char *buf, int *pos, int *total, long val)
{
	if (val < 0) {
		emit_buf(buf, pos, total, '-');
		/* Use unsigned cast to avoid UB when val == LONG_MIN:
		 * -(unsigned long)val is well-defined for all values. */
		emit_unsigned(buf, pos, total, -(unsigned long)val, 10, 0);
	} else {
		emit_unsigned(buf, pos, total, (unsigned long)val, 10, 0);
	}
}

// TODO support log levels
// TODO perform console locking (multicore)
// Useful references https://docs.kernel.org/core-api/printk-basics.html
// https://en.wikipedia.org/wiki/Printk
// NOTE: in the base code this function had a return type of void but normally it returns int
void printk(const char *fmt, ...)
{
	char buf[1024];
	int pos = 0;
	int total = 0;
	va_list ap;

	va_start(ap, fmt);

	for (int i = 0; fmt[i]; i++) {
		if (fmt[i] != '%') {
			emit_buf(buf, &pos, &total, fmt[i]);
			continue;
		}
		i++;
		/* Handle length modifier */
		int is_long = 0;
		if (fmt[i] == 'l') {
			is_long = 1;
			i++;
		}

		switch (fmt[i]) {
		case 'd':
		case 'i': {
			long val = is_long ? va_arg(ap, long)
					   : (long)va_arg(ap, int);
			emit_signed(buf, &pos, &total, val);
			break;
		}
		case 'u': {
			unsigned long val = is_long
				? va_arg(ap, unsigned long)
				: (unsigned long)va_arg(ap, unsigned int);
			emit_unsigned(buf, &pos, &total, val, 10, 0);
			break;
		}
		case 'x': {
			unsigned long val = is_long
				? va_arg(ap, unsigned long)
				: (unsigned long)va_arg(ap, unsigned int);
			emit_unsigned(buf, &pos, &total, val, 16, 0);
			break;
		}
		case 'X': {
			unsigned long val = is_long
				? va_arg(ap, unsigned long)
				: (unsigned long)va_arg(ap, unsigned int);
			emit_unsigned(buf, &pos, &total, val, 16, 1);
			break;
		}
		case 'p': {
			unsigned long val = (unsigned long)va_arg(ap, void *);
			emit_str(buf, &pos, &total, "0x");
			emit_unsigned(buf, &pos, &total, val, 16, 0);
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			emit_str(buf, &pos, &total, s);
			break;
		}
		case 'c': {
			char c = (char)va_arg(ap, int);
			emit_buf(buf, &pos, &total, c);
			break;
		}
		case '%':
			emit_buf(buf, &pos, &total, '%');
			break;
		default:
			emit_buf(buf, &pos, &total, '%');
			emit_buf(buf, &pos, &total, fmt[i]);
			break;
		}
	}

	va_end(ap);

	if (pos > 0) {
		writek(buf, pos);
		total += pos;
	}
	return; // total
}
