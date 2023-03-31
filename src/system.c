#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <spu-kit/system.h>

static const loglevel_t _log_level = TRACE;

static const char color[] = {
	[LOGLEVEL_TRACE]	= '8',	/* grey */
	[LOGLEVEL_DEBUG]	= '4',	/* blue */
	[LOGLEVEL_INFO]		= '2',	/* green */
	[LOGLEVEL_WARN]		= '3',	/* orange */
	[LOGLEVEL_ERR]		= '1',	/* red */
	[LOGLEVEL_ABRT]		= '6',	/* teal */
};

static const char * const code_str[] = {
	"---", // trace
	"dbg", // debug
	"inf", // info
	"wrn", // warning
	"err", // error
	"xxx", // critical
};

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

	if (code.lvl == LOGLEVEL_TRACE) {
		fputs_unlocked("     ", stdout);
	}else if (stdout_is_a_tty) {
		fputs_unlocked("\033[1;3", stdout);
		fputc_unlocked(color[code.lvl], stdout);
		fputc_unlocked('m', stdout);
		fputs_unlocked(code_str[code.lvl], stdout);
		fputs_unlocked("\033[0m: ", stdout);
	} else {
		fputs_unlocked(code_str[code.lvl], stdout);
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
	fprintf(stderr,
		"%s: %s:%u: %s %s(): %s\n",
		program_invocation_short_name,
		file, line, prefix, func, msg);
	abort();
}
