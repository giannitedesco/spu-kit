#pragma once

#include <stddef.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

/* ========[ c stuff ]===================================================== */
#if __STDC_VERSION__ > 201112L
# define noreturn _Noreturn
#else
# define noreturn __attribute__ ((noreturn))
#endif

#if defined(__USE_ISOC11) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/* ========[ macros ]====================================================== */
#ifndef likely
#define likely(x) __builtin_expect((bool)(x), true)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect((bool)(x), false)
#endif

/* ========[ ccan ]======================================================== */
#include <ccan/array_size.h>

/* ========[ glibc ]======================================================= */
extern char *program_invocation_short_name;

static inline const char *err_str(void)
{
	return strerror(errno);
}

/* ========[ say ]======================================================== */
typedef enum loglvl_e
#if __GNUC__ >= 13
: uint8_t
#endif
{
	LOGLEVEL_TRACE,
	LOGLEVEL_DEBUG,
	LOGLEVEL_INFO,
	LOGLEVEL_WARN,
	LOGLEVEL_ERR,
	LOGLEVEL_ABRT,
} loglvl_t;

typedef struct loglevel_s {
	loglvl_t lvl;
} loglevel_t;

#define LOGLEVEL__DEFINE(x) ((loglevel_t){ .lvl = x })

#define TRACE	LOGLEVEL__DEFINE(LOGLEVEL_TRACE)
#define DEBUG	LOGLEVEL__DEFINE(LOGLEVEL_DEBUG)
#define INFO	LOGLEVEL__DEFINE(LOGLEVEL_INFO)
#define WARN	LOGLEVEL__DEFINE(LOGLEVEL_WARN)
#define ERR	LOGLEVEL__DEFINE(LOGLEVEL_ERR)
#define ABRT	LOGLEVEL__DEFINE(LOGLEVEL_ABRT)

__attribute__((format(printf,2,3)))
void say(const loglevel_t code, const char * const fmt, ...);

/* Use this to prepend function name to error, so we can track it down */
#define TRACE_ERR(fmt, x...) say(ERR, "%s: " fmt, \
				__PRETTY_FUNCTION__, ##x)

/* Use this to report errors in system/library calls */
#define SYS_ERR(syscall) say(ERR, "%s: %s: %s", \
				__PRETTY_FUNCTION__, \
				syscall, \
				err_str())

/* Use this to report errors in system/lib calls, but when relating to a
 * specific file: eg. failed to open a file
 */
#define SYS_FILE_ERR(fn, syscall) say(ERR, "%s: %s: %s: %s", \
				__PRETTY_FUNCTION__, \
				fn, \
				syscall, \
				err_str())

#if DEBUG_TRACE
#define dbg_trace(...) say(TRACE, __VA_ARGS__)
#else
#define dbg_trace(...) do { } while (0);
#endif

/* ========[ assert ]===================================================== */
noreturn __attribute__ ((noinline,cold))
void x__assert_fail(const char *prefix,
			const char *msg,
			const char *file,
			unsigned int line,
			const char *func);

#ifndef unreachable
#define unreachable() (__builtin_unreachable())
#endif

#ifdef NDEBUG

# define xunreachable() (unreachable())
# define xassert(expr)						\
	((void) sizeof((expr) ? 1 : 0), __extension__ ({	\
	if (unlikely(!(bool)(expr)))				\
		unreachable();					\
	}))

#else

# define xunreachable() \
	x__assert_fail("Assertion failed in",			\
			"Unreachable code",			\
			__FILE__, __LINE__, __PRETTY_FUNCTION__)

# define xassert(expr)						\
	((void) sizeof((expr) ? 1 : 0), __extension__ ({	\
	if (unlikely(!(bool)(expr)))				\
		x__assert_fail("Assertion failed in",		\
			#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__);\
	}))

#endif

#define not_implemented(feature) \
	x__assert_fail("Unimplemented code in", feature,	\
			__FILE__, __LINE__, __PRETTY_FUNCTION__)
