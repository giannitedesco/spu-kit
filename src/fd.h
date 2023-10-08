#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

bool fd_write(const size_t len;
		const int fd,
		const uint8_t buf[static const len],
		const size_t len);

bool fd_pwrite(const size_t len;
		const int fd,
		const off_t off,
		const uint8_t buf[static const len],
		const size_t len);
