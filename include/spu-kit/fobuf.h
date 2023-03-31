#pragma once

#include <string.h>

#include <spu-kit/system.h>

/** fobuf - File output buffer object */
struct fobuf {
	/** Underlying file descriptor */
	int fd;

	/** @internal Pointer to base of buffer */
	uint8_t *buf;
	/** @internal Pointer to current position in buffer */
	uint8_t *ptr;
	/** @internal Amount of data remaining in the buffer */
	size_t buf_len;
	/** @internal Total size of the buffer */
	size_t buf_sz;
};

__attribute__((warn_unused_result,nonnull(1)))
bool fobuf_init(struct fobuf *b, int fd, size_t bufsz);

__attribute__((warn_unused_result,nonnull(1)))
bool _fobuf_write_slow(struct fobuf *b, const void *buf, size_t len);

__attribute__((warn_unused_result,nonnull(1)))
bool fobuf_flush(struct fobuf *b);

__attribute__((warn_unused_result,nonnull(1)))
bool fobuf_close(struct fobuf *b, const bool sync);

void fobuf_abort(struct fobuf *b);

__attribute__((pure,nonnull(1)))
int fobuf_fd(struct fobuf *b);

__attribute__((warn_unused_result,nonnull(1)))
static inline
bool fobuf_write(struct fobuf *b, const void *buf, size_t len)
{
	if (likely(len < b->buf_len)) {
		memcpy(b->ptr, buf, len);
		b->ptr += len;
		b->buf_len -= len;
		return true;
	}

	return _fobuf_write_slow(b, buf, len);
}

__attribute__((warn_unused_result))
bool fd_write(int fd, const void *buf, size_t len);

__attribute__((warn_unused_result))
bool fd_pwrite(int fd, off_t off, const void *buf, size_t len);
