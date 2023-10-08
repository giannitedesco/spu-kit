#pragma once

struct bufwr {
	uint8_t *buf;
	uint32_t cur;
	uint32_t max;
	int fd;
	bool corrupted;
};

struct bufwr bufwr__init(const int fd, const size_t buf_size);

__attribute__((nonnull(1)))
void bufwr__fini(struct bufwr * const f);

__attribute__((nonnull(1)))
void bufwr__abort(struct bufwr * const f);

__attribute__((warn_unused_result, nonnull(1)))
bool bufwr__close(struct bufwr * const f, const bool sync);
