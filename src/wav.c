#include <spu-kit/wav.h>
#include <spu-kit/bufwr.h>

#include "bufwr.h"
#include "fd.h"
#include "system.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>


typedef struct fourcc_s {
	union {
		char bytes[4];
		uint32_t be32;
	};
} fourcc_t;

#define FOURCC(a, b, c, d) ((fourcc_t){ .bytes = {a, b, c, d}})

#define RIFF FOURCC('R', 'I', 'F', 'F')
#define WAVE FOURCC('W', 'A', 'V', 'E')
#define FMT FOURCC('f', 'm', 't', ' ')
#define DATA FOURCC('d', 'a', 't', 'a')

struct chunk_hdr {
	fourcc_t fourcc;
	uint32_t size;
};
static_assert(sizeof(struct chunk_hdr) == 8, "RIFF chunk hdr size");

struct riff_hdr {
	struct chunk_hdr hdr;
	fourcc_t form;
};
static_assert(sizeof(struct riff_hdr) == 12, "RIFF hdr size");

struct wave_fmt {
	struct chunk_hdr hdr;
	uint16_t audio_fmt;
	uint16_t num_channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t sample_bits;
};
static_assert(sizeof(struct wave_fmt) == 8 + 16, "wave fmt block size");

struct wave_hdr {
	struct riff_hdr riff;
	struct wave_fmt fmt;
	struct chunk_hdr data;
} __attribute__((packed));

struct wav_s {
	struct bufwr buf;
	size_t nr_samples;
};

#define HZ 32000
//#define HZ 33252
static const struct wave_hdr hdr = {
	.riff = {
		.hdr.fourcc = RIFF,
		.form = WAVE,
	},
	.fmt = {
		.hdr.fourcc = FMT,
		.hdr.size = 16,
		.audio_fmt = 1,
		.num_channels = 2,
		.sample_rate = HZ ,
		.byte_rate = HZ * 2 * 2,
		.sample_bits = 16,
		.block_align = 2,
	},
	.data = {
		.fourcc = DATA,
	},
};

wav_t *wav_create(const char *fn)
{
	wav_t *wav;
	int fd;

	wav = malloc(sizeof(*wav));
	if (wav == NULL) {
		goto out;
	}

	fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		say(ERR, "%s: open: %s", fn, strerror(errno));
		goto out_free;
	}

	*wav = (struct wav_s) {
		.buf = bufwr__init(fd, 0),
	};

	if (unlikely(wav->buf.buf == NULL))
		goto out_close;

	if (unlikely(!bufwr_write(&wav->buf, &hdr, sizeof(hdr))))
		goto out_abort;

	return wav;

out_abort:
	bufwr__abort(&wav->buf);
	goto out_free;
out_close:
	close(fd);
out_free:
	free(wav);
out:
	return wav;
}

__attribute__((nonnull(1)))
bool wav_write_samples16(size_t num;
		wav_t *wav,
		const int16_t sample[static num],
		size_t num)

{
	const bool ret = bufwr_write(&wav->buf, (uint8_t *)sample, num * sizeof(sample[0]));

	if (unlikely(!ret)) {
		say(ERR, "wav: write: %s", strerror(errno));
		return false;
	}

	wav->nr_samples += num;

	return true;
}

__attribute__((nonnull(1),warn_unused_result))
static bool rewrite_hdr(wav_t *wav)
{
	const size_t file_size = (wav->nr_samples * 2) + sizeof(hdr);
	struct wave_hdr fixed = hdr;

	fixed.riff.hdr.size = file_size - sizeof(struct chunk_hdr);
	fixed.data.size = file_size - sizeof(fixed);

	if (!fd_pwrite(wav->buf.fd, 0, (uint8_t *)&fixed, sizeof(fixed)))
		return false;

	return true;
}

__attribute__((nonnull(1)))
bool _wav_close(wav_t *wav)
{
	const bool ret = likely(
		bufwr_flush(&wav->buf)
		&& rewrite_hdr(wav)
		&& bufwr__close(&wav->buf, false)
	);

	free(wav);
	return ret;
}
