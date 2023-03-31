#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <endian.h>

#include <spu-kit/system.h>
#include <spu-kit/fobuf.h>
#include <spu-kit/spc-file.h>
#include <spu-kit/apu.h>
#include <spu-kit/spc700.h>
#include <spu-kit/dsp.h>

static bool fill_buf(size_t len;
			int fd,
			uint8_t buf[static len],
			size_t len)
{
	uint8_t *ptr = buf;
	ssize_t ret;

	while (len) {
		ret = read(fd, ptr, len);
		if (ret < 0) {
			return false;
		} else if (ret == 0) {
			errno = 0;
			return false;
		}

		xassert((size_t)ret <= len);

		ptr += ret;
		len -= ret;
	}

	return true;
}

struct spc_file spc;

__attribute__((cold))
static bool load(const char *fn)
{
	bool ret = false;
	int fd;

	say(INFO, "load: %s", fn);

	fd = open(fn, O_RDONLY);
	if (fd < 0) {
		say(ERR, "%s: open: %s", fn, strerror(errno));
		goto out;
	}

	if (!fill_buf(fd, (uint8_t *)&spc, sizeof(spc))) {
		say(ERR, "%s: open: %s", fn, strerror(errno));
		goto out_close;
	}

	ret = true;

out_close:
	close(fd);
out:
	return ret;
}

__attribute__((cold))
static void print_id666(void)
{
#if 1
	say(INFO, "song title: %.*s", 32, spc.id666.txt.song_title);
	say(INFO, "game title: %.*s", 32, spc.id666.txt.game_title);
	say(INFO, "dumper: %.*s", 16, spc.id666.txt.dumper);
	if (spc.id666.txt.comments[0] != '\0')
		say(INFO, "comments: %.*s", 32, spc.id666.txt.comments);
	say(INFO, "dump date: %.*s", 11, spc.id666.txt.dump_date);
	say(INFO, "song length: %.*s secs", 3, spc.id666.txt.song_secs);
	say(INFO, "fade length: %.*s msec", 5, spc.id666.txt.fade_msecs);
	say(INFO, "artist: %.*s", 32, spc.id666.txt.artist);
	say(INFO, "channel disables: 0x%.2x", spc.id666.txt.default_channel_disables);
	say(INFO, "emulator: 0x%.2x", spc.id666.txt.dump_emulator);
#endif
#if 0
	say(INFO, "song title: %.*s", 32, spc.id666.bin.song_title);
	say(INFO, "game title: %.*s", 32, spc.id666.bin.game_title);
	say(INFO, "dumper: %.*s", 16, spc.id666.bin.dumper);
	if (spc.id666.bin.comments[0] != '\0')
		say(INFO, "comments: %.*s", 32, spc.id666.bin.comments);
	say(INFO, "dump date: 0x%.8x", spc.id666.bin.dump_date);
	say(INFO, "song length: %.*s secs", 3, spc.id666.bin.song_secs);
	say(INFO, "fade length: %.*s msec", 4, spc.id666.bin.fade_msecs);
	say(INFO, "artist: %.*s", 32, spc.id666.bin.artist);
	say(INFO, "channel disables: 0x%.2x", spc.id666.bin.default_channel_disables);
	say(INFO, "emulator: 0x%.2x", spc.id666.bin.dump_emulator);
#endif
}

static struct spc700_regs convert_regs(const struct spc_regs r)
{
	return (struct spc700_regs){
		.pc = le32toh(r.pc),
		.a = r.a,
		.x = r.x,
		.y = r.y,
		.psw = r.psw,
		.sp = r.sp,
	};
}

static bool dump_ram(const uint8_t ram[static 0x10000], const char *fn)
{
	bool ret = false;
	int fd;

	say(INFO, "Dumping 64KiB of RAM to: %s", fn);

	fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		say(ERR, "%s: open: %s", fn, strerror(errno));
		goto out;
	}

	if (!fd_write(fd, ram, 0x10000)) {
		say(ERR, "%s: write: %s", fn, strerror(errno));
		goto out_close;
	}

	ret = true;

out_close:
	if (close(fd)) {
		say(ERR, "%s: close: %s", fn, strerror(errno));
		ret = false;
	}
out:
	return ret;
}

__attribute__((cold))
static void setup_spc700(void)
{
	dump_ram(spc.ram, "aram.bin");

	spc700_restore(convert_regs(spc.regs),
			spc.ram,
			spc.extra_ram);

	apu_restore(apu_state_from_aram(spc.ram));

	dsp_restore(spc.dsp_regs);
}

static bool handle_file(const char *fn)
{
	if (!load(fn))
		return false;

	print_id666();

	setup_spc700();

	spc700_run_forever();

	return true;
}

__attribute__((cold))
int main(int argc, char **argv)
{
	int ret = EXIT_SUCCESS;

	for (int i = 1; i < argc; i++) {
		if (!handle_file(argv[i]))
			ret = EXIT_FAILURE;
	}

	return ret;
}
