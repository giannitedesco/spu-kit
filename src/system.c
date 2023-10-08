#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "system.h"

static const loglevel_t _log_level = TRACE;

__attribute__((pure))
static inline char color(const loglevel_t lvl)
{
	switch (lvl.lvl) {
	case LOGLEVEL_TRACE:	return '8'; /* grey */
	case LOGLEVEL_DEBUG:	return '4'; /* blue */
	case LOGLEVEL_INFO:	return '2'; /* green */
	case LOGLEVEL_WARN:	return '3'; /* orange */
	case LOGLEVEL_ERR:	return '1'; /* red */
	case LOGLEVEL_ABRT:	return '6'; /* teal */
	default:
		xunreachable();
	}
}

__attribute__((pure))
static inline const char *code_str(const loglevel_t lvl)
{
	switch (lvl.lvl) {
	case LOGLEVEL_TRACE:	return "---";
	case LOGLEVEL_DEBUG:	return "dbg";
	case LOGLEVEL_INFO:	return "inf";
	case LOGLEVEL_WARN:	return "wrn";
	case LOGLEVEL_ERR:	return "err";
	case LOGLEVEL_ABRT:	return "xxx";
	default:
		xunreachable();
	}
}

static bool stdout_is_a_tty;

__attribute__((constructor))
static void ctor(void)
{
	if (isatty(fileno(stdout)))
		stdout_is_a_tty = true;
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
}

/* Print or log message. Debug can be enabled per module */
void say(loglevel_t code, const char *fmt, ...)
{
	va_list va;

	/* Hard-coded for now */
	if (code.lvl < _log_level.lvl)
		return;

	/* We're doing output in multiple calls using the underlying stdout
	 * object so we need to lock it to make sure concurrent printfs dont
	 * mangle our output
	 */
	flockfile(stdout);

	if (stdout_is_a_tty) {
		fputs_unlocked("\033[1;3", stdout);
		fputc_unlocked(color(code), stdout);
		fputc_unlocked('m', stdout);
		fputs_unlocked(code_str(code), stdout);
		fputs_unlocked("\033[0m: ", stdout);
	} else {
		fputs_unlocked(code_str(code), stdout);
		fputs_unlocked(": ", stdout);
	}

	va_start(va, fmt);
	vprintf(fmt, va);
	va_end(va);

	putc_unlocked('\n', stdout);
	funlockfile(stdout);
}

noreturn __attribute__ ((noinline,cold))
void x__assert_fail(const char *prefix,
			const char *msg,
			const char *file,
			unsigned int line,
			const char *func)
{
	say(ABRT,
		"%s: %s:%u: %s %s(): %s",
		program_invocation_short_name,
		file, line, prefix, func, msg);
	abort();
}
