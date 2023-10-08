#pragma once

#include <assert.h>
#include <stdint.h>

#define SPC_FORMAT_ID "SNES-SPC700 Sound File Data v0.30"
#define SPC_MAGIC			0x1a1a
#define SPC_ID666_TAGGED		0x1a
#define SPC_ID666_NOT_TAGGED		0x1b
#define SPC_VERSION_MINOR		30
struct spc_hdr {
	uint8_t format_id[33];
	uint16_t magic;
	uint8_t id666_tag_status;
	uint8_t version_minor;
} __attribute__((packed));

struct spc_regs {
	uint16_t pc;
	uint8_t a;
	uint8_t x;
	uint8_t y;
	uint8_t psw;
	uint8_t sp;
	uint16_t _reserved;
} __attribute__((packed));

struct spc_id666_txt {
	uint8_t song_title[32];
	uint8_t game_title[32];
	uint8_t dumper[16];
	uint8_t comments[32];
	uint8_t dump_date[11];
	uint8_t song_secs[3];
	uint8_t fade_msecs[5];
	uint8_t artist[32];
	uint8_t default_channel_disables;
	uint8_t dump_emulator;
	uint8_t _reserved[45];
} __attribute__((packed));

struct spc_id666_bin {
	uint8_t song_title[32];
	uint8_t game_title[32];
	uint8_t dumper[16];
	uint8_t comments[32];
	uint32_t dump_date;
	uint8_t _unused[7];
	uint8_t song_secs[3];
	uint8_t fade_msecs[4];
	uint8_t artist[32];
	uint8_t default_channel_disables;
	uint8_t dump_emulator;
	uint8_t _reserved[46];
} __attribute__((packed));

struct spc_id666 {
	union {
		struct spc_id666_txt txt;
		struct spc_id666_bin bin;
	};
} __attribute__((packed));

static_assert(sizeof(struct spc_id666_bin) == sizeof(struct spc_id666_txt),
		"SPC ID666 bin/txt sizes do not match");

struct spc_file {
	struct spc_hdr hdr;
	struct spc_regs regs;
	struct spc_id666 id666;
	uint8_t ram[0x10000];
	uint8_t dsp_regs[0x80];
	uint8_t _unused[64];
	uint8_t extra_ram[64];
} __attribute__((packed));

static_assert(sizeof(struct spc_file) == 0x10200, "Wrong size SPC File");
