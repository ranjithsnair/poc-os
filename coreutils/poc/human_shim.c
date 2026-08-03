/* human_readable()/human_options(): real gnulib (coreutils/lib/
 * human.c) formats sizes using `long double` arithmetic throughout -
 * even the ordinary, non-"-h" case (see human.c's own adjust_value(),
 * used unconditionally to round n*from_block_size/to_block_size) -
 * for overflow-safe rounding of a potentially-huge uintmax_t product.
 * long double needs the x87 FPU, which poc-os's kernel never saves/
 * restores across a context switch (-mgeneral-regs-only - see the
 * Makefile's COREUTILS_PIC_CFLAGS comment): the same reason gnulib's
 * hash.c needed a from-scratch integer reimplementation (see
 * coreutils_shims.c's own hash_initialize comment).
 *
 * This is that same kind of reimplementation, in plain 64-bit integer
 * arithmetic throughout - exact, not just "close enough", for every
 * size poc-os's own MAXFILE (a few MB at most - see include/fs.h)
 * could ever actually produce, unlike real gnulib's need to handle an
 * arbitrarily-huge uintmax_t input without overflowing. Digit
 * grouping (human_group_digits) is a deliberate no-op: it only takes
 * effect outside the C locale, which is the only locale this port's
 * setlocale() (coreutils_shims.c) ever actually has.
 */
#include "human.h"
#include "xstrtol.h"

enum strtol_error
human_options(char const *spec, int *output_block_size_opts,
              uintmax_t *output_block_size)
{
	uintmax_t val;
	uintmax_t mult;
	const char *p;

	if (spec == 0 || *spec == 0) {
		*output_block_size_opts = 0;
		*output_block_size = 1024;
		return LONGINT_OK;
	}

	val = 0;
	p = spec;
	if (*p < '0' || *p > '9')
		return LONGINT_INVALID;
	while (*p >= '0' && *p <= '9') {
		val = val * 10 + (uintmax_t)(*p - '0');
		p++;
	}

	mult = 1;
	switch (*p) {
	case 0:
		break;
	case 'k': case 'K':
		mult = 1024;
		p++;
		break;
	case 'M':
		mult = 1024ULL * 1024;
		p++;
		break;
	case 'G':
		mult = 1024ULL * 1024 * 1024;
		p++;
		break;
	default:
		return LONGINT_INVALID;
	}

	if (p[0] == 'i' && p[1] == 'B')
		p += 2;
	else if (p[0] == 'B')
		p += 1;
	if (*p != 0)
		return LONGINT_INVALID;

	*output_block_size_opts = 0;
	*output_block_size = (val == 0 ? 1 : val) * mult;
	return LONGINT_OK;
}

static int
utoa(uintmax_t v, char *buf)
{
	char rev[32];
	int rl = 0, len = 0;

	if (v == 0) {
		buf[len++] = '0';
	} else {
		while (v) {
			rev[rl++] = (char)('0' + (v % 10));
			v /= 10;
		}
		while (rl)
			buf[len++] = rev[--rl];
	}
	return len;
}

char *
human_readable(uintmax_t n, char *buf, int opts,
               uintmax_t from_block_size, uintmax_t to_block_size)
{
	unsigned long long num, den, amt;
	unsigned base;
	int shift;
	unsigned long long whole, scaled10;
	int len;

	num = (unsigned long long)n * (from_block_size ? from_block_size : 1);
	den = to_block_size ? to_block_size : 1;
	if (opts & human_round_to_nearest)
		amt = (num + den / 2) / den;
	else if (opts & human_floor)
		amt = num / den;
	else /* human_ceiling (0) is the default */
		amt = (num + den - 1) / den;

	if (!(opts & human_autoscale)) {
		len = utoa(amt, buf);
		buf[len] = 0;
		return buf;
	}

	base = (opts & human_base_1024) ? 1024 : 1000;
	static char const *const prefixes1000[] =
		{ "", "k", "M", "G", "T", "P", "E", "Z", "Y" };
	static char const *const prefixes1024[] =
		{ "", "K", "M", "G", "T", "P", "E", "Z", "Y" };

	shift = 0;
	whole = amt;
	scaled10 = amt * 10;
	while (whole >= base && shift < 8) {
		scaled10 = (scaled10 + base / 2) / base;
		whole = scaled10 / 10;
		shift++;
	}

	len = utoa(whole, buf);
	if (shift > 0 && !((opts & human_suppress_point_zero) && scaled10 % 10 == 0)) {
		buf[len++] = '.';
		buf[len++] = (char)('0' + (scaled10 % 10));
	}

	if (opts & human_space_before_unit)
		buf[len++] = ' ';
	{
		char const *unit = (base == 1024) ? prefixes1024[shift] : prefixes1000[shift];
		while (*unit)
			buf[len++] = *unit++;
	}
	if ((opts & human_B) && shift > 0) {
		if (base == 1024)
			buf[len++] = 'i';
		buf[len++] = 'B';
	}
	buf[len] = 0;
	return buf;
}
