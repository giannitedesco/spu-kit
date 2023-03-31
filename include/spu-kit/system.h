#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#if __STDC_VERSION__ > 201112L
# include <stddef.h>
# include <stdalign.h>
# define noreturn _Noreturn
#else
# define noreturn __attribute__ ((noreturn))
#endif

#if defined(__USE_ISOC11) && !defined(static_assert)
#define static_assert _Static_assert
#endif

#ifndef likely
#define likely(x) __builtin_expect((bool)(x), true)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect((bool)(x), false)
#endif

#define ARRAY_SIZE(arr) \
	({		\
		static_assert(!__builtin_types_compatible_p(typeof(arr), \
							typeof(&(arr)[0]))); \
		sizeof(arr) / sizeof((arr)[0]); \
	})

extern char *program_invocation_short_name;

#pragma GCC push_options
#pragma GCC optimize("short-enums")
enum loglevel_e {
	LOGLEVEL_TRACE,
	LOGLEVEL_DEBUG,
	LOGLEVEL_INFO,
	LOGLEVEL_WARN,
	LOGLEVEL_ERR,
	LOGLEVEL_ABRT,
};
#pragma GCC pop_options

typedef struct _loglevel {
	enum loglevel_e lvl;
} loglevel_t;

#define LOGLEVEL__DEFINE(x) ((loglevel_t){ .lvl = x })

#define TRACE LOGLEVEL__DEFINE(LOGLEVEL_TRACE)
#define DEBUG LOGLEVEL__DEFINE(LOGLEVEL_DEBUG)
#define INFO LOGLEVEL__DEFINE(LOGLEVEL_INFO)
#define WARN LOGLEVEL__DEFINE(LOGLEVEL_WARN)
#define ERR LOGLEVEL__DEFINE(LOGLEVEL_ERR)
#define ABRT LOGLEVEL__DEFINE(LOGLEVEL_ABRT)

__attribute__((format(printf,2,3)))
void say(loglevel_t code, const char *fmt, ...);

void hex_dump_addr(const size_t len;
		const uint8_t tmp[static len],
		const size_t len,
		size_t llen,
		const size_t addr);
void hex_dump(const size_t len;
		const uint8_t ptr[static len],
		const size_t len,
		size_t llen);

noreturn __attribute__ ((noinline,cold))
void x__assert_fail(const char *prefix,
			const char *msg,
			const char *file,
			unsigned int line,
			const char *func);

#ifdef NDEBUG
#define unreachable() (__builtin_unreachable())
#define xassert(expr)						\
	((void) sizeof((expr) ? 1 : 0), __extension__ ({	\
	if (!__builtin_expect((bool)(expr), true))		\
		__builtin_unreachable();			\
	}))

#else
#define unreachable() \
	x__assert_fail("Assertion failed in",			\
			"Unreachable code",			\
			__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define xassert(expr)						\
	((void) sizeof((expr) ? 1 : 0), __extension__ ({	\
	if (!__builtin_expect((bool)(expr), true))		\
		x__assert_fail("Assertion failed in",	\
			#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__);\
	}))

#endif

#define not_implemented(feature) \
	x__assert_fail("Unimplemented code in", feature,	\
			__FILE__, __LINE__, __PRETTY_FUNCTION__)
