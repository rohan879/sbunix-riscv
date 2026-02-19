#include <printk.h>
#include <uart.h>
#include <stdarg.h>

// similar to write userspace call but does not use fd number
static void writek(char *buf, int count) {
    for (int i = 0; i < count; i++) {
        uart_write(buf[i]);
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
		emit_unsigned(buf, pos, total, (unsigned long)(-val), 10, 0);
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
