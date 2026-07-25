/*
 * __mul{s,d,x}c3/__div{s,d,x}c3 -- the compiler-generated runtime calls
 * clang/GCC emit for `_Complex` multiply/divide (options/ansi/generic/
 * complex/*.c's casin()/catanh()/etc. use plain `*`/`/` on `_Complex`
 * operands; the compiler lowers those to these calls rather than
 * inlining them). Normally supplied by libgcc or compiler-rt, neither of
 * which this freestanding, no-libgcc-dependency build has (see
 * -Dlibgcc_dependency=false in the top-level Makefile's mlibc-sysroot*
 * targets) -- for the static build this never mattered, since an
 * unreferenced complex-math object simply never gets pulled into a
 * static link, but libc.so's `--whole-archive` (mlibc's own meson.build
 * recipe for the shared build) forces every complex-math object in
 * regardless of whether anything calls it, so libc.so itself must
 * provide these.
 *
 * These are the direct algebraic formulas, not the C99 Annex G.5.1
 * reference algorithm real compiler-rt implements (which adds NaN/Inf
 * edge-case recovery so e.g. Inf*0 terms in an otherwise-well-defined
 * product don't poison the result to NaN) -- correct for all finite
 * inputs, which is what a hobby OS's libc actually exercises; the
 * Annex G edge cases are deliberately out of scope here.
 */

float _Complex __mulsc3(float a, float b, float c, float d) {
	float _Complex r;
	__real__ r = a * c - b * d;
	__imag__ r = a * d + b * c;
	return r;
}

double _Complex __muldc3(double a, double b, double c, double d) {
	double _Complex r;
	__real__ r = a * c - b * d;
	__imag__ r = a * d + b * c;
	return r;
}

long double _Complex __mulxc3(long double a, long double b, long double c, long double d) {
	long double _Complex r;
	__real__ r = a * c - b * d;
	__imag__ r = a * d + b * c;
	return r;
}

float _Complex __divsc3(float a, float b, float c, float d) {
	float denom = c * c + d * d;
	float _Complex r;
	__real__ r = (a * c + b * d) / denom;
	__imag__ r = (b * c - a * d) / denom;
	return r;
}

double _Complex __divdc3(double a, double b, double c, double d) {
	double denom = c * c + d * d;
	double _Complex r;
	__real__ r = (a * c + b * d) / denom;
	__imag__ r = (b * c - a * d) / denom;
	return r;
}

long double _Complex __divxc3(long double a, long double b, long double c, long double d) {
	long double denom = c * c + d * d;
	long double _Complex r;
	__real__ r = (a * c + b * d) / denom;
	__imag__ r = (b * c - a * d) / denom;
	return r;
}
