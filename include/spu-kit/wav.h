#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct wav_s;
typedef struct wav_s wav_t;

wav_t *wav_create(const char *fn);

__attribute__((nonnull(1)))
bool wav_write_samples16(size_t num;
		wav_t *wav,
		const int16_t sample[static num],
		size_t num);

__attribute__((nonnull(1),warn_unused_result))
bool _wav_close(wav_t *wav);

__attribute__((warn_unused_result))
static inline bool wav_close(wav_t *wav)
{
	if (wav == NULL) {
		return true;
	}
	return _wav_close(wav);
}
