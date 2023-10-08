/*
* This file is part of Firestorm NIDS
* Copyright (c) 2003 Gianni Tedesco
* Released under the terms of the GNU GPL version 2
*/

#include "system.h"
#include "fd.h"

#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/poll.h>

/**
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
static bool fd_wait_single(const int fd, const int flags)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = flags | POLLHUP | POLLERR,
	};
	int ret;

again:
	ret = poll(&pfd, 1, -1);
	if (unlikely(ret < 0)) {
		if (errno == EINTR)
			goto again;
		return false;
	}


	/* Some systems don't indicate POLLIN on EOF, rather they use POLLHUP.
	 * In the case of POLLERR/POLLHUP: we return true here and force the
	 * caller to attempt an I/O operation in order to make sure that the
	 * correct value ends up in errno
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
bool fd_write(const size_t len;
		const int fd,
		const uint8_t buf[static const len],
		const size_t len)
{
	const uint8_t *ptr = buf;
	size_t remains = len;
	size_t wr_size;
	ssize_t ret;

again:
	wr_size = remains;
	if (unlikely(wr_size > SSIZE_MAX)) {
		wr_size = SSIZE_MAX;
	}

	ret = write(fd, ptr, wr_size);
	if (unlikely(ret < 0)) {
		if (errno == EINTR)
			goto again;
		if (errno == EAGAIN && fd_wait_single(fd, POLLOUT))
			goto again;
		return false;
	}

	/* This can happen if fd is socket, and connection was hung up */
	if (unlikely(ret == 0)) {
		errno = ECONNRESET;
		return false;
	}

	remains -= (size_t)ret;

	/* This can happen on a regular file if a long I/O is interrupted for
	 * example on NFS in soft/interruptable mode in the Linux kernel.  It
	 * can also happen on sockets and character devices.
	 */
	if (remains) {
		ptr += (size_t)ret;
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
bool fd_pwrite(const size_t len;
		const int fd,
		const off_t off,
		const uint8_t buf[static const len],
		const size_t len)
{
	const uint8_t *ptr = buf;
	off_t cur_off = off;
	size_t remains = len;
	size_t wr_size;
	ssize_t ret;

again:
	wr_size = remains;
	if (unlikely(wr_size > SSIZE_MAX)) {
		wr_size = SSIZE_MAX;
	}

	ret = pwrite(fd, buf, wr_size, cur_off);
	if (ret < 0) {
		if (errno == EINTR)
			goto again;
		if (errno == EAGAIN && fd_wait_single(fd, POLLOUT))
			goto again;
		return false;
	}

	/* This can happen if fd is socket, and connection was hung up */
	if (unlikely(ret == 0)) {
		errno = ECONNRESET;
		return false;
	}

	remains -= (size_t)ret;

	/* This can happen on a regular file if a long I/O is interrupted for
	 * example on NFS in soft/interruptable mode in the Linux kernel.  It
	 * can also happen on sockets and character devices.
	 */
	if (remains) {
		cur_off += (size_t)ret;
		ptr += (size_t)ret;
		goto again;
	}

	return true;
}
