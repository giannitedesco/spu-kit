/*
* This file is part of Firestorm NIDS
* Copyright (c) 2003 Gianni Tedesco
* Released under the terms of the GNU GPL version 2
*/

#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/poll.h>

#include <spu-kit/system.h>
#include <spu-kit/fobuf.h>

/*
 * @param fd: File descriptor to wait for events on
 * @param flags: Poll events to wait for
 *
 * Waits for events on a non-blocking file descriptor. This is an internal API
 * used by fibuf/fobuf etc. if the fd they are trying to use returns EAGAIN,
 * this returns the blocking behaivour.
 *
 * Return value: 1 on success, 0 on error.
 */
__attribute__((noinline))
static bool fd_wait_single(int fd, int flags)
{
	struct pollfd pfd;
	int ret;

	/* Some systems don't indicate POLLIN on EOF, rather they use POLLHUP,
	 * we always check for POLLHUP then use read/ write to see what the
	 * real problem is.
	 */
	flags |= POLLHUP;

	pfd.fd = fd;
	pfd.events = flags | POLLERR;
	pfd.revents = 0;

again:
	ret = poll(&pfd, 1, -1);
	if (unlikely(ret < 0)) {
		if (errno == EINTR)
			goto again;
		return false;
	}

	if (pfd.revents & flags)
		return true;

	/* In the case of PILLERR: we return 1 here and force the caller to
	 * attempt an I/O operation in order to make sure that the correct
	 * value ends up in errno
	 */
	return true;
}

/** Write to a file descriptor handling all errors.
 * @param fd file descriptor
 * @param buf data to write
 * @param len length of data
 *
 * Call write(2) with given parameters but handle all possible errors. We
 * handle short writes, interrupted calls, fd going O_NONBLOCK under us, and
 * only bail on really unrecoverable errors.
 *
 * @return 0 on unrecoverable error, 1 on success.
 */
bool fd_write(int fd, const void *buf, size_t len)
{
	ssize_t ret;

again:
	ret = write(fd, buf, likely(len < SSIZE_MAX) ? len : SSIZE_MAX);
	if (unlikely(ret < 0)) {
		if (errno == EINTR)
			goto again;
		if (errno == EAGAIN && fd_wait_single(fd, POLLOUT))
			goto again;
		return false;
	}

	/* This can happen on a regular file if a long I/O is interrupted for
	 * example on NFS in soft/interruptable mode in the Linux kernel.  It
	 * can also happen on sockets and character devices.
	 */
	if ((size_t)ret < len) {
		buf += (size_t)ret;
		len -= (size_t)ret;
		goto again;
	}

	return true;
}

/** Write to a file descriptor handling all errors.
 * @param fd file descriptor
 * @param buf data to write
 * @param len length of data
 *
 * Call pwrite(2) with given parameters but handle all possible errors. We
 * handle short writes, interrupted calls, fd going O_NONBLOCK under us, and
 * only bail on really unrecoverable errors.
 *
 * @return 0 on unrecoverable error, 1 on success.
 */
bool fd_pwrite(int fd, off_t off, const void *buf, size_t len)
{
	ssize_t ret;

again:
	ret = pwrite(fd, buf, likely(len < SSIZE_MAX) ? len : SSIZE_MAX, off);
	if (ret < 0) {
		if (errno == EINTR)
			goto again;
		if (errno == EAGAIN && fd_wait_single(fd, POLLOUT))
			goto again;
		return false;
	}

	/* This can happen on a regular file if a long I/O is interrupted
	 * for example on NFS in soft/interruptable mode in the Linux kernel.
	 * It can also happen on sockets and character devices.
	 */
	if ((size_t)ret < len) {
		off += (size_t)ret;
		buf += (size_t)ret;
		len -= (size_t)ret;
		goto again;
	}

	return true;
}

/** fobuf_flush
 * @param b: the fobuf structure to flush
 *
 * Flush the userspace buffer to disk. Note this does not call fsync() so do
 * not rely on it in order to verify that data is written to disk.
 */
bool fobuf_flush(struct fobuf *b)
{
	size_t len = b->buf_sz - b->buf_len;
	const void *buf = b->buf;

	/* buffer empty */
	if (len == 0)
		return true;

	if (unlikely(!fd_write(b->fd, buf, len))) {
		//ERR("fd_write: %s", os_err());
		return false;
	}

	b->ptr = b->buf;
	b->buf_len = b->buf_sz;

	return true;
}

/** fobuf_init
 * @param b: a fobuf structure to use
 * @param fd: file descriptor to write to
 * @param bufsz: size of output buffer, 0 is default
 *
 * Attach a file descriptor to a buffer and ready it for use.
 */
bool fobuf_init(struct fobuf *b, const int fd, size_t bufsz)
{
	uint8_t *buf;

	if (fd < 0)
		return false;

	if (!bufsz)
		bufsz = 4096;

	buf = malloc(bufsz);
	if (unlikely(buf == NULL))
		return false;

	*b = (struct fobuf) {
		.fd = fd,
		.buf = buf,
		.ptr = buf,
		.buf_len = bufsz,
		.buf_sz = bufsz,
	};

	return true;
}

/** fobuf_abort()
 * @param b: the fobuf structure to finish up with.
 *
 * Free up any allocated memory and anything in the buffer. Note that this does
 * not close the file descriptor, you must do that yourself.
 */
void fobuf_abort(struct fobuf *b)
{
	if (b->buf) {
		free(b->buf);
	}
}

/** fobuf_close
 * @param b: the fobuf structure to finish up with.
 *
 * Flush the buffers, ensure data is on disk and close the file descriptor.
 *
 * Return value: zero on error, non-zero on success.
 */
bool fobuf_close(struct fobuf *b, const bool sync)
{
	bool ret = true;

	//DEBUG("%p", b);

	if (b->fd < 0)
		goto noclose;

	if (!fobuf_flush(b))
		ret = false;

	if (sync) {
		/* don't error if the output file is a special file which does
		 * not support fsync (eg: a pipe)
		 */
		if (fsync(b->fd) && errno != EROFS && errno != EINVAL) {
			//ERR("fsync: %s", os_err());
			ret = false;
		}
	}

	if (close(b->fd)) {
		//ERR("close: %s", os_err());
		ret = false;
	}

noclose:
	fobuf_abort(b);

	return ret;
}

/**
 * @param b: a fobuf structure to use
 * @param buf: pointer to the data you want to write
 * @param len: size of data pointed to by buf
 *
 * Slow path for writing to the buffer, do not call directly instead use
 * fobuf_write().
 *
 * Return value: zero on error, non-zero on success.
 */
bool _fobuf_write_slow(struct fobuf *b, const void *buf, size_t len)
{
	/* fill up the buffer before flushing, we already know that len >=
	 * b->buf_len so a full buffer flush is inevitable.
	 */
	memcpy(b->ptr, buf, b->buf_len);
	buf += b->buf_len;
	len -= b->buf_len;
	b->ptr += b->buf_len;
	b->buf_len = 0;
	if (unlikely(!fobuf_flush(b)))
		return false;

	/* If the remaining data is the same size as the buffer then buffering
	 * is doing an un-necessary copy of the data.
	 *
	 * If the remaining data is bigger than the buffer then we must write
	 * it out right away anyway.
	 */
	if (len >= b->buf_sz)
		return fd_write(b->fd, buf, len);

	/* normal write - len may be zero */
	memcpy(b->ptr, buf, len);
	b->ptr += len;
	b->buf_len -= len;

	return true;
}

int fobuf_fd(struct fobuf *b)
{
	return b->fd;
}
