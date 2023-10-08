#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct bufwr bufwr_t;

bufwr_t *bufwr_new(const int fd, const size_t buf_size);

__attribute__((warn_unused_result))
bool bufwr_write(bufwr_t *f,
		const void * const buf,
		const size_t len);

__attribute__((warn_unused_result))
bool bufwr_flush(bufwr_t *f);

/* flush, close, and free */
__attribute__((warn_unused_result))
bool bufwr_close(bufwr_t *f, const bool sync);

/* close and free, not flushing, and ignoring errors */
void bufwr_abort(bufwr_t *f);

/* set fd to -1 - useful if you don't want close/abort to close it
 */
void bufwr__leak_fd(bufwr_t *f);
