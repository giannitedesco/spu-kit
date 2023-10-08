#include <spu-kit/bufwr.h>

#include "system.h"
#include "fd.h"

#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_BUF_SIZE	UINT32_C(8192)
#define MAX_BUF_SIZE		UINT32_C(0x800000000)

static size_t buffer_size(const size_t buf_size)
{
	if (buf_size > UINT32_C(0xffffffff))
		return MAX_BUF_SIZE;

	return (buf_size) ? buf_size : DEFAULT_BUF_SIZE;
}

#include "bufwr.h"

bufwr_t bufwr__init(const int fd, const size_t buf_size)
{
	const size_t bufsz = buffer_size(buf_size);

	return (struct bufwr){
		.fd = fd,
		.buf = malloc(bufsz),
		.max = bufsz,
	};
}

bufwr_t *bufwr_new(const int fd, const size_t buf_size)
{
	struct bufwr *f;

	f = malloc(sizeof(*f));
	if (unlikely(f == NULL)) {
		goto out;
	}

	*f = bufwr__init(fd, buf_size);
	if (unlikely(f->buf == NULL)) {
		goto out_free;
	}

	return f;

out_free:
	free(f);
	f = NULL;
out:
	return f;
}

__attribute__((nonnull(1),warn_unused_result))
static bool flush(bufwr_t *f)
{
	/* -1 is basically /dev/null */
	if (unlikely(f->fd < 0)) {
		goto ok;
	}

	if (unlikely(!fd_write(f->fd, f->buf, f->cur))) {
		f->corrupted = true;
		return false;
	}

ok:
	f->cur = 0;
	return true;
}

/* if len is so big that we're going to be wasting a memcpy for a
 * second flush, or doing multiple buffer-fulles and therefore multiple
 * syscalls, then just flush what's already in the buffer and do a
 * single syscall to write out the argument.
 */
__attribute__((noinline))
static bool large_write(bufwr_t *f,
			const void * const buf,
			const size_t len)
{
	if (unlikely(!flush(f))) {
		return false;
	}
	if (unlikely(!fd_write(f->fd, buf, len))) {
		return false;
	}
	return true;
}

__attribute__((warn_unused_result))
bool bufwr_write(bufwr_t *f,
		const void * const buf,
		const size_t len)
{
	const size_t space = f->max - f->cur;
	const uint8_t *ptr = buf;
	size_t remaining = len;

	if (len >= space + f->max) {
		return large_write(f, buf, len);
	}

	/* If the arg is bigger than buffer space remaining, fill up the buffer
	 * and write it out first
	 */
	if (len > space) {
		memcpy(f->buf + f->cur, ptr, space);
		f->cur += space;
		if (unlikely(!flush(f))) {
			return false;
		}

		ptr += space;
		remaining -= space;
	}

	/* Finally, memcpy the remainder into the buffer */
	memcpy(f->buf + f->cur, ptr, remaining);
	f->cur += remaining;

	xassert(f->cur <= f->max);
	return true;
}

__attribute__((warn_unused_result))
bool bufwr_flush(bufwr_t *f)
{
	/* nothing to flush */
	if (!f->cur) {
		return true;
	}

	return flush(f);
}

__attribute__((nonnull(1)))
void bufwr__fini(bufwr_t *f)
{
	free(f->buf);
}

__attribute__((nonnull(1)))
void bufwr__abort(bufwr_t *f)
{
	if (f->fd >= 0) {
		close(f->fd);
	}
	bufwr__fini(f);
}

__attribute__((warn_unused_result, nonnull(1)))
bool bufwr__close(bufwr_t *f, const bool sync)
{
	bool ret = true;

	if (unlikely(!bufwr_flush(f)))
		ret = false;

	if (f->fd >= 0) {
		if (sync && unlikely(fsync(f->fd))) {
			switch (errno) {
			case EROFS:
			case EINVAL:
				/* fs doesn't implement fsync */
				break;
			default:
				ret = false;
			}
		}

		if (unlikely(close(f->fd))) {
			ret = false;
		}
	}

	bufwr__fini(f);
	return ret;
}

__attribute__((warn_unused_result))
bool bufwr_close(bufwr_t *f, const bool sync)
{
	bool ret;

	if (f == NULL) {
		return true;
	}

	ret = bufwr__close(f, sync);

	free(f);

	return ret;
}

void bufwr_abort(bufwr_t *f)
{
	if (f) {
		bufwr__abort(f);
		free(f);
	}
}

/* set fd to -1 - useful if you don't want close/abort to close it
 */
void bufwr__leak_fd(bufwr_t *f)
{
	f->fd = -1;
}
