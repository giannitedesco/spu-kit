#include <string.h>
#include <stdlib.h>
#include <endian.h>

#include <spu-kit/system.h>
#include <spu-kit/dsp.h>
#include <spu-kit/wav.h>

#include "dsp-regs.h"
#include "dsp.h"
#include "aram.h"

//#define BRR_DECODE_TRACE
//#define MMIO_TRACE

#ifdef BRR_DECODE_TRACE
#define brr_decode_trace(...) say(TRACE, __VA_ARGS__)
#else
#define brr_decode_trace(...) do { } while (0)
#endif

#ifdef MMIO_TRACE
#define mmio_trace(...) say(TRACE, __VA_ARGS__)
#else
#define mmio_trace(...) do { } while (0)
#endif

static uint8_t regs[0x80];
#pragma GCC push_options
#pragma GCC optimize("short-enums")
typedef enum {
	ENV_RELEASE,
	ENV_ATTACK,
	ENV_DECAY,
	ENV_SUSTAIN,
} env_state_t;
#pragma GCC pop_options

struct sample {
	union {
		struct {
			int16_t left;
			int16_t right;
		};
		int16_t arr[2];
	};
};

#define BRR_BUF_SZ 12
struct vstate {
	int interp_pos;
	int env;
	int16_t output_sample;
	uint16_t srcn_ptr;
	uint16_t next_brr_addr;
	uint16_t brr_addr;
	uint16_t pitch;
	env_state_t env_mode;
	uint8_t brr_hdr;
	uint8_t brr_off;
	uint8_t buf_pos;
	uint8_t attack_delay;
	int16_t buf[BRR_BUF_SZ];
};

static const uint8_t ctr_number[32] = {
	0xff,
	   0, 1,
	2, 0, 1,
	2, 0, 1,
	2, 0, 1,
	2, 0, 1,
	2, 0, 1,
	2, 0, 1,
	2, 0, 1,
	2, 0, 1,
	2, 0, 1,
	   0,
	   0,
};
static const uint16_t ctr_mask[32] = {
	0,
	       0x7ff, 0x1ff,
	0x0ff, 0x3ff, 0x0ff,
	0x07f, 0x1ff, 0x07f,
	0x03f, 0x0ff, 0x03f,
	0x01f, 0x07f, 0x01f,
	0x00f, 0x03f, 0x00f,
	0x007, 0x01f, 0x007,
	0x003, 0x00f, 0x003,
	0x001, 0x007, 0x001,
	0x000, 0x003, 0x000,
	       0x001,
	       0x000,
};

static uint8_t ctr_internal[3];
static const uint8_t ctr_rate[3] = {1, 3, 5};
static const uint8_t ctr_internal_init[3] = {1, 2, 3};
static unsigned int ctr_out[3];
static const unsigned int ctr_initial[3] = {0, -347, -107};

static inline void ctr_init(void)
{
	for (int i = 0; i < 3; i++) {
		ctr_internal[i] = ctr_internal_init[i];
		ctr_out[i] = ctr_initial[i];
	}
}

static inline void ctr_run(void)
{
	ctr_out[0]++;

	if (!--ctr_internal[1]) {
		ctr_internal[1] = 3;
		ctr_out[1]++;
	}
	if (!--ctr_internal[2]) {
		ctr_internal[2] = 5;
		ctr_out[2]++;
	}
}

static inline bool ctr_read(unsigned int rate)
{
	const uint8_t ctr_nr = ctr_number[rate];

	if (rate == 0)
		return false;

	if (ctr_out[ctr_nr] & ctr_mask[rate])
		return false;
#if 0
	printf("ctr_out[%d] = %d & 0x%03x (internal %d)\n",
		ctr_nr,
		ctr_out[ctr_nr],
		ctr_mask[rate],
		ctr_internal[ctr_nr]);
#endif
	return ctr_internal[ctr_nr] == ctr_rate[ctr_nr];
}

static struct vstate vstate[DSP_CHANNELS];

/* KON/KOF when last checked */
static uint8_t kon;
static uint8_t koff;

/* toggles every sample */
static bool toggle;

__attribute__((always_inline))
static inline int clamp16(int val)
{
	return ((int16_t)val != val) ? ((val >> 31) ^ 0x7fff) : val;
}

__attribute__((pure))
static struct vregs *voice(uint8_t channel)
{
	xassert(channel < DSP_CHANNELS);

	return (struct vregs *)(regs + (channel << 4));
}

__attribute__((pure))
static uint16_t voice_pitch(const struct vregs *v)
{
	return le16toh(v->pitch) & 0x3fff;
}

#define FLAG_REG(x, y) ((regs[x] & (1U << y)) ? "YES" : "---")
static inline void dump_regs(void)
{
	say(INFO, "%8s:  0   1   2   3   4   5   6   7", "VOICE");
	say(INFO, "%8s: %s %s %s %s %s %s %s %s",
		"KON",
		FLAG_REG(REG_KON, 0),
		FLAG_REG(REG_KON, 1),
		FLAG_REG(REG_KON, 2),
		FLAG_REG(REG_KON, 3),
		FLAG_REG(REG_KON, 4),
		FLAG_REG(REG_KON, 5),
		FLAG_REG(REG_KON, 6),
		FLAG_REG(REG_KON, 7));
	say(INFO, "%8s: %s %s %s %s %s %s %s %s",
		"KOFF",
		FLAG_REG(REG_KOFF, 0),
		FLAG_REG(REG_KOFF, 1),
		FLAG_REG(REG_KOFF, 2),
		FLAG_REG(REG_KOFF, 3),
		FLAG_REG(REG_KOFF, 4),
		FLAG_REG(REG_KOFF, 5),
		FLAG_REG(REG_KOFF, 6),
		FLAG_REG(REG_KOFF, 7));
	say(INFO, "%8s: %s %s %s %s %s %s %s %s",
		"NON",
		FLAG_REG(REG_NON, 0),
		FLAG_REG(REG_NON, 1),
		FLAG_REG(REG_NON, 2),
		FLAG_REG(REG_NON, 3),
		FLAG_REG(REG_NON, 4),
		FLAG_REG(REG_NON, 5),
		FLAG_REG(REG_NON, 6),
		FLAG_REG(REG_NON, 7));
	say(INFO, "%8s: %s %s %s %s %s %s %s %s",
		"EON",
		FLAG_REG(REG_EON, 0),
		FLAG_REG(REG_EON, 1),
		FLAG_REG(REG_EON, 2),
		FLAG_REG(REG_EON, 3),
		FLAG_REG(REG_EON, 4),
		FLAG_REG(REG_EON, 5),
		FLAG_REG(REG_EON, 6),
		FLAG_REG(REG_EON, 7));
	say(INFO, "%8s: %s %s %s %s %s %s %s %s",
		"PMON",
		FLAG_REG(REG_PMON, 0),
		FLAG_REG(REG_PMON, 1),
		FLAG_REG(REG_PMON, 2),
		FLAG_REG(REG_PMON, 3),
		FLAG_REG(REG_PMON, 4),
		FLAG_REG(REG_PMON, 5),
		FLAG_REG(REG_PMON, 6),
		FLAG_REG(REG_PMON, 7));
	for (int i = 0; i <= VREG_COEF; i++) {
		const char *tmpl = dsp_vreg_template(i);

		if (tmpl == NULL)
			continue;

		say(INFO, "%8s: $%02x $%02x $%02x $%02x $%02x $%02x $%02x $%02x",
			tmpl,
			regs[0x00 | i],
			regs[0x10 | i],
			regs[0x20 | i],
			regs[0x30 | i],
			regs[0x40 | i],
			regs[0x50 | i],
			regs[0x60 | i],
			regs[0x70 | i]);
	}

	say(INFO, "     ESA: $%02x00       ED: $%02x    EFB: $%02x",
			regs[REG_ESA], regs[REG_ED], regs[REG_EFB]);
	say(INFO, "     DIR: $%02x00   %s %s %s  NFREQ: $%02x",
			regs[REG_DIR],
			(regs[REG_FLG] & FLG_SOFT_RESET) ? "RST" : "---",
			(regs[REG_FLG] & FLG_MUTE) ? "RST" : "---",
			(regs[REG_FLG] & FLG_ECHO_DISABLED) ? "---" : "ECH",
			(regs[REG_FLG] & 0xf));
	say(INFO, "MVOL L/R: $%02x $%02x       EVOL L/R: $%02x $%02x",
			regs[REG_MVOLL], regs[REG_MVOLR],
			regs[REG_EVOLL], regs[REG_EVOLR]);
}

#define BRR_BLOCK_SIZE		9
#define BRR_SAMPLE_PAIRS	8
#define BRR_BLOCK_SAMPLES	(BRR_SAMPLE_PAIRS * 2)
#define BRR_END			(1 << 0)
#define BRR_LOOP		(1 << 1)
#define BRR_FLAGS		(BRR_END|BRR_LOOP)

typedef union nybs {
	struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		int8_t lo:4;
		int8_t hi:4;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		int8_t hi:4;
		int8_t lo:4;
#else
#error "Unsupported endian for this"
#endif
	};
	uint8_t byte;
} nybs_t;

struct brr_pair {
	int16_t s[2];
};


__attribute__((always_inline))
static inline struct brr_pair brr_pair(const int16_t sample1, const int16_t sample2)
{
	return (struct brr_pair) {
		.s[0] = sample1,
		.s[1] = sample2,
	};
}

__attribute__((always_inline))
static inline struct brr_pair brr_pair_extract(const uint8_t byte)
{
	const nybs_t n = {
		.byte = byte,
	};

	return brr_pair(n.hi, n.lo);
}

__attribute__((always_inline))
static inline struct brr_pair brr_pair_load(const uint16_t addr)
{
	return brr_pair_extract(aram[addr]);
}

static inline struct brr_pair brr_pair_scale(const struct brr_pair in, uint8_t shift)
{
	return (struct brr_pair) {
		.s[0] = ((int)in.s[0] << shift) >> 1,
		.s[1] = ((int)in.s[1] << shift) >> 1,
	};
}

struct brr_block {
	int16_t s[BRR_BLOCK_SAMPLES];
};

struct brr_filter_state {
	int16_t older;
	int16_t old;
};

__attribute__((always_inline))
static inline
struct brr_filter_state brr_filter_state(const int16_t older, const int16_t old)
{
	return (struct brr_filter_state){
		.older = older,
		.old = old,
	};
}

/* multiply by 15/16 = 0.9735 */
static int coeff1_mul(int p)
{
	return p + (-p >> 4);
}

/* multiply by 61/32 = 1.90625 */
static int coeff2_mul(int p)
{
	return (p << 1) + ((-p * 3) >> 5);
}

/* multiply by 115/64 = 1.796875 */
static int coeff3_mul(int p)
{
	return (p << 1) + ((-p * 13) >> 6);
}

/* multiply by 13/16 = 0.8125 */
static int coeff4_mul(int p)
{
	return p + ((-p * 3) >> 4);
}

static int brr_filter1(const int s, const int p)
{
	return s + coeff1_mul(p);
}

static int brr_filter2(const int s, const int p, const int pp)
{
	return s + coeff2_mul(p) - coeff1_mul(pp);
}

static int brr_filter3(const int s, const int p, const int pp)
{
	return s + coeff3_mul(p) - coeff4_mul(pp);
}

static struct brr_block decode_brr(uint16_t aptr,
					const struct brr_filter_state *st,
					bool *end, bool *loop)
{
	const uint8_t ctrl = aram[aptr];
	const uint8_t filter = (ctrl >> 2) & 3;
	const uint8_t scale = ctrl >> 4;
	const uint8_t shift = (scale > 12) ? 12 : scale;
	struct brr_block blk;
	struct brr_filter_state cur = {
		.older = st->older,
		.old = st->old,
	};

	xassert(scale <= 12);

	brr_decode_trace("ctrl=$%02x filter=%u scale=%u", ctrl, filter, scale);
#if BRR_DECODE_TRACE
	hex_dump_addr(aram + aptr, BRR_BLOCK_SIZE, 0, aptr);
#endif

	aptr++;

	switch (filter) {
	case 0:
		for (int i = 0, o = 0; i < 8; i++) {
			const struct brr_pair sp = brr_pair_load(aptr++);
			const struct brr_pair s = brr_pair_scale(sp, shift);

			blk.s[o++] = s.s[0];
			blk.s[o++] = s.s[1];
		}
		break;
	case 1:
		for (int i = 0, o = 0; i < 8; i++) {
			const struct brr_pair sp = brr_pair_load(aptr++);
			const struct brr_pair s = brr_pair_scale(sp, shift);

			cur.older = blk.s[o++] = brr_filter1(s.s[0], cur.old);
			cur.old   = blk.s[o++] = brr_filter1(s.s[1], cur.older);
		}
		break;
	case 2:
		for (int i = 0, o = 0; i < 8; i++) {
			const struct brr_pair sp = brr_pair_load(aptr++);
			const struct brr_pair s = brr_pair_scale(sp, shift);

			cur.older  = blk.s[o++] = brr_filter2(s.s[0], cur.old, cur.older);
			cur.old    = blk.s[o++] = brr_filter2(s.s[1], cur.older, cur.old);
		}
		break;
	case 3:
		for (int i = 0, o = 0; i < 8; i++) {
			const struct brr_pair sp = brr_pair_load(aptr++);
			const struct brr_pair s = brr_pair_scale(sp, shift);

			cur.older  = blk.s[o++] = brr_filter3(s.s[0], cur.old, cur.older);
			cur.old    = blk.s[o++] = brr_filter3(s.s[1], cur.older, cur.old);
		}
		break;
	default:
		unreachable();
	}

	*end = ctrl & BRR_END;
	*loop = ctrl & BRR_LOOP;

	return blk;
}

static bool decode_brr_list(uint16_t aptr,
				struct brr_filter_state *st,
				wav_t *wav)
{
	while (true) {
		bool end, loop;
		const struct brr_block blk = decode_brr(aptr, st, &end, &loop);

		st->older = blk.s[14];
		st->old = blk.s[15];

		for (size_t i = 0; i < ARRAY_SIZE(blk.s); i++) {
			printf(" %d", blk.s[i]);
		}
		printf("\n");

		if (end) {
			return loop;
		}

		aptr += BRR_BLOCK_SIZE;
		if (!wav_write_samples16(wav, blk.s, ARRAY_SIZE(blk.s)))
			abort();
	}
}

static uint16_t dirp_effective_addr(void)
{
	return (uint16_t)regs[REG_DIR] << 8;
}

static uint16_t srcn_effective_addr(const uint8_t srcn)
{
	return dirp_effective_addr() + ((uint16_t)srcn << 2);
}

static uint16_t voice_srcn_pointer(unsigned int i, const struct vregs *v)
{
#if 0
	if (i == 7) {
		return srcn_effective_addr(0x05);
	}
	if (i == 5) {
		return srcn_effective_addr(0x42);
	}
#endif
	return srcn_effective_addr(v->srcn);
}

__attribute__((always_inline))
static inline uint16_t read_aram_byte(const uint16_t addr)
{
	return aram[addr];
}

__attribute__((always_inline))
static inline uint16_t read_aram_word(const uint16_t addr)
{
	const uint16_t word_lo = addr + 0;
	const uint16_t word_hi = addr + 1;

	return (aram[word_hi] << 8) | aram[word_lo];
}

struct dir_entry {
	const uint16_t base;
	const uint16_t loop;
};

static struct dir_entry load_dir_entry(const uint16_t addr)
{
	return (struct dir_entry){
		.base = read_aram_word(addr + 0),
		.loop = read_aram_word(addr + 2),
	};
}

static struct dir_entry dir_entry(const uint8_t srcn)
{
	return load_dir_entry(srcn_effective_addr(srcn));
}

static inline void dump_srcn(const uint8_t srcn)
{
	const struct dir_entry ent = dir_entry(srcn);
	struct brr_filter_state st = {0, };
	char wavname[16];
	wav_t *wav;

	snprintf(wavname, sizeof(wavname), "src-%u.wav", srcn);
	wav = wav_create(wavname);

	xassert(wav != NULL);

	say(INFO, "base $%04x -> %s", ent.base, wavname);

	if (decode_brr_list(ent.base, &st, wav) && ent.loop != ent.base) {
		say(INFO, "loop $%04x", ent.loop);
		decode_brr_list(ent.loop, &st, wav);
	}

	if (!wav_close(wav))
		abort();
}

static inline void dump_samples(void)
{
	const uint8_t kon = regs[REG_KON];

	// hex_dump(aram + dirp, 0x100, 0);

	for (int i = 0; i < DSP_CHANNELS; i++) {
		const struct vregs *v = voice(i);
		const uint8_t srcn = v->srcn;

		if (!(kon & (1 << i)))
			continue;

		say(INFO, "V%dSRCn = $%02x", i, srcn);
		dump_srcn(srcn);
	}
}

__attribute__((always_inline))
static inline uint8_t brr_byte(struct vstate * const st)
{
	return aram[st->brr_addr + st->brr_off++];
}

static inline struct brr_filter_state vfilter_state(const struct vstate * const st)
{
	if (st->buf_pos) {
		return brr_filter_state(st->buf[st->buf_pos - 2],
					st->buf[st->buf_pos - 1]);
	} else {
		return brr_filter_state(st->buf[BRR_BUF_SZ - 2],
					st->buf[BRR_BUF_SZ - 1]);
	}
}

static void brr_sample4(struct vstate * const st)
{
	const uint8_t filter = (st->brr_hdr >> 2) & 3;
	const uint8_t scale = st->brr_hdr >> 4;
	const uint8_t shift = (scale > 12) ? 12 : scale;
	const struct brr_filter_state prev = vfilter_state(st);
	const struct brr_pair in[2] = {
		brr_pair_scale(brr_pair_extract(brr_byte(st)), shift),
		brr_pair_scale(brr_pair_extract(brr_byte(st)), shift),
	};
	int a, b, c, d;

	switch (__builtin_expect(filter, 2)) {
	case 0:
		a = in[0].s[0];
		b = in[0].s[1];
		c = in[1].s[0];
		d = in[1].s[1];
		break;
	case 1:
		a = brr_filter1(in[0].s[0], prev.old);
		b = brr_filter1(in[0].s[1], a);
		c = brr_filter1(in[1].s[0], b);
		d = brr_filter1(in[1].s[1], c);
		break;
	case 2:
		a = brr_filter2(in[0].s[0], prev.old, prev.older);
		b = brr_filter2(in[0].s[1], a, prev.old);
		c = brr_filter2(in[1].s[0], b, a);
		d = brr_filter2(in[1].s[1], c, b);
		break;
	case 3:
		a = brr_filter3(in[0].s[0], prev.old, prev.older);
		b = brr_filter3(in[0].s[1], a, prev.old);
		c = brr_filter3(in[1].s[0], b, a);
		d = brr_filter3(in[1].s[1], c, b);
		break;
	default:
		unreachable();
	}

	st->buf[st->buf_pos + 0] = clamp16(a);
	st->buf[st->buf_pos + 1] = clamp16(b);
	st->buf[st->buf_pos + 2] = clamp16(c);
	st->buf[st->buf_pos + 3] = clamp16(d);
	st->buf_pos += 4;
	if (st->buf_pos >= BRR_BUF_SZ)
		st->buf_pos = 0;
}

static int16_t const gauss[512] = {
	   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
	   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,
	   2,   2,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,
	   6,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  10,
	  11,  11,  11,  12,  12,  13,  13,  14,  14,  15,  15,  15,  16,  16,  17,  17,
	  18,  19,  19,  20,  20,  21,  21,  22,  23,  23,  24,  24,  25,  26,  27,  27,
	  28,  29,  29,  30,  31,  32,  32,  33,  34,  35,  36,  36,  37,  38,  39,  40,
	  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
	  58,  59,  60,  61,  62,  64,  65,  66,  67,  69,  70,  71,  73,  74,  76,  77,
	  78,  80,  81,  83,  84,  86,  87,  89,  90,  92,  94,  95,  97,  99, 100, 102,
	 104, 106, 107, 109, 111, 113, 115, 117, 118, 120, 122, 124, 126, 128, 130, 132,
	 134, 137, 139, 141, 143, 145, 147, 150, 152, 154, 156, 159, 161, 163, 166, 168,
	 171, 173, 175, 178, 180, 183, 186, 188, 191, 193, 196, 199, 201, 204, 207, 210,
	 212, 215, 218, 221, 224, 227, 230, 233, 236, 239, 242, 245, 248, 251, 254, 257,
	 260, 263, 267, 270, 273, 276, 280, 283, 286, 290, 293, 297, 300, 304, 307, 311,
	 314, 318, 321, 325, 328, 332, 336, 339, 343, 347, 351, 354, 358, 362, 366, 370,
	 374, 378, 381, 385, 389, 393, 397, 401, 405, 410, 414, 418, 422, 426, 430, 434,
	 439, 443, 447, 451, 456, 460, 464, 469, 473, 477, 482, 486, 491, 495, 499, 504,
	 508, 513, 517, 522, 527, 531, 536, 540, 545, 550, 554, 559, 563, 568, 573, 577,
	 582, 587, 592, 596, 601, 606, 611, 615, 620, 625, 630, 635, 640, 644, 649, 654,
	 659, 664, 669, 674, 678, 683, 688, 693, 698, 703, 708, 713, 718, 723, 728, 732,
	 737, 742, 747, 752, 757, 762, 767, 772, 777, 782, 787, 792, 797, 802, 806, 811,
	 816, 821, 826, 831, 836, 841, 846, 851, 855, 860, 865, 870, 875, 880, 884, 889,
	 894, 899, 904, 908, 913, 918, 923, 927, 932, 937, 941, 946, 951, 955, 960, 965,
	 969, 974, 978, 983, 988, 992, 997,1001,1005,1010,1014,1019,1023,1027,1032,1036,
	1040,1045,1049,1053,1057,1061,1066,1070,1074,1078,1082,1086,1090,1094,1098,1102,
	1106,1109,1113,1117,1121,1125,1128,1132,1136,1139,1143,1146,1150,1153,1157,1160,
	1164,1167,1170,1174,1177,1180,1183,1186,1190,1193,1196,1199,1202,1205,1207,1210,
	1213,1216,1219,1221,1224,1227,1229,1232,1234,1237,1239,1241,1244,1246,1248,1251,
	1253,1255,1257,1259,1261,1263,1265,1267,1269,1270,1272,1274,1275,1277,1279,1280,
	1282,1283,1284,1286,1287,1288,1290,1291,1292,1293,1294,1295,1296,1297,1297,1298,
	1299,1300,1300,1301,1302,1302,1303,1303,1303,1304,1304,1304,1304,1304,1305,1305,
};

__attribute__((pure))
static uint8_t wrap12(const uint8_t val)
{
	xassert(val <= 8 + 7 + 3);

	return (val >= 12) ? (val - 12) : val;
}

struct envelope {
	int env;
	int rate;
};

__attribute__((pure))
static struct envelope run_adsr_env(struct vstate * const st,
					const uint8_t adsr1,
					const uint8_t adsr2)
{
	int r;

	switch (st->env_mode) {
	case ENV_ATTACK:
		r = (adsr1 & ADSR1_ATTACK_RATE_MASK) * 2 + 1;
		return (struct envelope){
			.env = st->env + ((r == 0x1f) ? 0x400 : 0x20),
			.rate = r,
		};
	case ENV_SUSTAIN:
		r = adsr2 & ADSR2_SUSTAIN_RATE_MASK;
		return (struct envelope) {
			.env = st->env - ((st->env >> 8) + 1),
			.rate = r,
		};
	case ENV_DECAY:
		r = 0x10 + ((adsr1 >> ADSR1_DECAY_RATE_SHIFT) & ADSR1_DECAY_RATE_MASK);
		return (struct envelope) {
			.env = st->env - ((st->env >> 8) + 1),
			.rate = r,
		};
	case ENV_RELEASE:
	default:
		unreachable();
	}
}

__attribute__((pure,noinline))
static struct envelope run_gain_env(struct vstate * const st,
					const uint8_t adsr1,
					const uint8_t gain)
{
	const bool custom = gain & GAIN_MODE_CUSTOM;
	const uint8_t mode = (gain >> GAIN_MODE_SHIFT) & GAIN_MODE_MASK;
	const int rate = gain & GAIN_RATE_MASK;
	int env;

	if (!custom) {
		return (struct envelope) {
			.env = gain * 0x10,
			.rate = 31,
		};
	}

	switch (mode) {
	case 0: /* linear decrease */
		env = st->env - 0x20;
		break;
	case 1: /* exponential decrease */
		env = st->env - ((st->env >> 8) + 1);
		break;
	case 2: /* linear increase */
	case 3: /* bent increase */
		env = st->env + 0x20;
		break;
	default:
		unreachable();
	}

	return (struct envelope) {
		.env = env,
		.rate = rate,
	};
}

static inline void envelope_release(struct vstate * const st)
{
	if (st->env > 8) {
		st->env -= 8;
	} else {
		st->env = 0;
	}
}

static void run_envelope(struct vstate * const st,
			const uint8_t adsr1,
			const uint8_t adsr2,
			const uint8_t gain)
{
	uint8_t sustain_target;
	struct envelope ret;

	if (st->env_mode == ENV_RELEASE) {
		/* release works the same in all modes */
		envelope_release(st);
		return;
	}

	if (likely(adsr1 & ADSR1_USE_ADSR)) {
		sustain_target = adsr2 >> ADSR2_SUSTAIN_LEVEL_SHIFT;
		ret = run_adsr_env(st, adsr1, adsr2);
	} else {
		sustain_target = gain >> ADSR2_SUSTAIN_LEVEL_SHIFT;
		ret = run_gain_env(st, adsr1, gain);
	}

	/* trigger sustain? */
	if ((ret.env >> 8) == sustain_target && st->env_mode == ENV_DECAY) {
		st->env_mode = ENV_SUSTAIN;
	}

	/* trigger decay? */
	if ((unsigned)ret.env > 0x7ff) {
		ret.env = (ret.env < 0) ? 0 : 0x7ff;
		if (st->env_mode == ENV_ATTACK) {
			st->env_mode = ENV_DECAY;
		}
	}

	if (ctr_read(ret.rate)) {
		st->env = ret.env;
	}
}

__attribute__((pure))
static int interpolate(struct vstate * const st)
{
	const uint8_t interp_hi = (st->interp_pos >> 12) & 0x7;
	const uint8_t interp_mid = st->interp_pos >> 4;
	const uint8_t buf_pos = st->buf_pos + interp_hi;
	const int16_t * const fwd = gauss + 255 - interp_mid;
	const int16_t * const rev = gauss + interp_mid;
	int16_t in[4] = {
		st->buf[wrap12(buf_pos + 0)],
		st->buf[wrap12(buf_pos + 1)],
		st->buf[wrap12(buf_pos + 2)],
		st->buf[wrap12(buf_pos + 3)],
	};
	int out = 0;

	out += (fwd[000] * (int)in[0]) >> 11;
	out += (fwd[256] * (int)in[1]) >> 11;
	out += (rev[256] * (int)in[2]) >> 11;
	out += (rev[000] * (int)in[3]) >> 11;

	return out & ~1;
}

static void voice_run(const unsigned int i)
{
	struct vregs *v = voice(i);
	struct vstate *st = &vstate[i];
	const uint8_t bit = (1U << i);

	/* CLOCK: cycle 1 */

	st->srcn_ptr = voice_srcn_pointer(i, v);

	/* CLOCK: cycle 2 */

	/* Calculate BRR effective address */
	if (!st->attack_delay) {
		st->srcn_ptr += 2;
	}
	st->next_brr_addr = read_aram_word(st->srcn_ptr);

	/* TODO: read envelope 0 */

	/* CLOCK: cycle 3a */

	/* XXX: pitch-read should be split over the two cycles */
	st->pitch = voice_pitch(v);

	/* CLOCK: cycle 3b */
	st->brr_hdr = read_aram_byte(st->brr_addr);
	// st->brr_byte = read_aram_byte(st->brr_addr + st->brr_off);

	/* CLOCK: cycle 3c */

	if (regs[REG_PMON] & bit) {
		/* TODO: pitch mod with previous voice */
	}

	if (st->attack_delay) {
		if (st->attack_delay == 5) {
			st->brr_addr = st->next_brr_addr;
			st->brr_off = 1;
			st->buf_pos = 0;
			st->brr_hdr = 0;
		}

		st->attack_delay--;
		if (st->attack_delay <= 3) {
			st->interp_pos = 0x4000;
		} else {
			st->interp_pos = 0;
		}

		st->pitch = 0;
		st->env = 0;
	}

	if (st->env) {
		if (regs[REG_NON] & bit) {
			/* TODO: noise */
			say(WARN, "noise sample");
		} else {
			st->output_sample = interpolate(st);
		}

		/* XXX: buffer this for later */
		v->outx = st->output_sample >> 8;

		/* apply envelope */
		st->output_sample = ((st->output_sample * st->env) >> 11) & ~1;

		/* XXX: buffer this for later */
		v->envx = (st->env >> 4);
	} else {
		v->outx = 0;
		v->envx = 0;
		st->output_sample = 0;
	}

	/* output silence due to reset or end of sample eilence */
	if (regs[REG_FLG] & FLG_SOFT_RESET || (st->brr_hdr & BRR_FLAGS) == BRR_END) {
		st->env_mode = ENV_RELEASE;
		st->env = 0;
	}

	if (!toggle) {
		if (koff & bit) {
			if (st->env_mode != ENV_RELEASE) {
				// say(DEBUG, "V%u: key-off", i);
				st->env_mode = ENV_RELEASE;
			}
		}

		if (kon & bit) {
			// say(DEBUG, "V%u: key-on", i);

			st->env_mode = ENV_ATTACK;
			st->attack_delay = 5;
		}
	}

	if (!st->attack_delay) {
		run_envelope(st, v->adsr1, v->adsr2, v->gain);
		if (st->env_mode == ENV_RELEASE && st->env == 0)
			return;
	}

	/* CLOCK: cycle 4 */

	/* Decode BRR */
	if (st->interp_pos >= 0x4000) {
		brr_sample4(st);
		if (st->brr_off >= BRR_BLOCK_SIZE) {
			st->brr_addr += BRR_BLOCK_SIZE;
			if (st->brr_hdr & BRR_END) {
				st->brr_addr = st->next_brr_addr;
				/* XXX: buffer */
				regs[REG_ENDX] |= bit;
			}
			st->brr_off = 1;
		}
	}

	/* apply pitch */
	st->interp_pos = (st->interp_pos & 0x3fff) + st->pitch;
	if (st->interp_pos > 0x7fff)
		st->interp_pos = 0x7fff;

	/* output left channel */
	if (regs[REG_EON] & bit) {
	}

	/* CLOCK: cycle 5 */

	/* output right channel */
	if (regs[REG_EON] & bit) {
	}

	/* buffer ENDX */
	if (st->attack_delay == 5) {
		/* XXX: buffer */
		regs[REG_ENDX] &= ~bit;
	}

	/* CLOCK: cycle 6 */
	/* TODO: buffer OUTX */

	/* CLOCK: cycle 7 */
	/* TODO: expose ENDX, buffer ENVX */

	/* CLOCK: cycle 8 */
	/* TODO: expose OUTX */

	/* CLOCK: cycle 9 */
	/* TODO: expose ENVX */
}

__attribute__((always_inline))
static inline struct sample sample_blend(const struct sample a, const struct sample b)
{
	const int l = (int)a.left + (int)b.left;
	const int r = (int)a.right + (int)b.right;

	return (struct sample) {
		.left = clamp16(l),
		.right = clamp16(r),
	};
}

__attribute__((pure))
static struct sample voice_sample(const unsigned int i)
{
	const struct vregs *v = voice(i);
	struct vstate *st = &vstate[i];

	return (struct sample) {
		.left = ((int)st->output_sample * v->voll) >> 7,
		.right = ((int)st->output_sample * v->volr) >> 7,
	};
}

/* Run 32 cycles */
static struct sample next_sample(void)
{
	struct sample sample = {0, };

	// say(DEBUG, "DSP 32 clocks");

	/* poll KON/KOF every other sample */
	toggle ^= 1;

	if (!toggle) {
		kon = regs[REG_KON] & ~kon;
		koff = regs[REG_KOFF];

		if (kon) {
			//say(DEBUG, "new kon $%02x", kon);
		}
	}

	ctr_run();

	/* TODO: sample noise */

	for (int i = 0; i < DSP_CHANNELS; i++) {
		voice_run(i);
		sample = sample_blend(sample, voice_sample(i));
	}

	return sample;
}

#define SECONDS 60
void _dsp_run32(void)
{
	const struct sample sample = next_sample();
	static unsigned int nr_samples;
	static wav_t *wav;

	if (unlikely(wav == NULL)) {
		wav = wav_create("out.wav");
	}

	if (!wav_write_samples16(wav, sample.arr, 2)) {
		abort();
	}

	if (++nr_samples >= 32000 * SECONDS) {
		if (!wav_close(wav))
			abort();
		exit(0);
	}

}

static void dump_dir(void)
{
	say(INFO, "256 entry BRR sample source table:");
	for (int i = 0; i < 0x100; i += 8) {
		const struct dir_entry e[] = {
			dir_entry(i + 0),
			dir_entry(i + 1),
			dir_entry(i + 2),
			dir_entry(i + 3),
			dir_entry(i + 4),
			dir_entry(i + 5),
			dir_entry(i + 6),
			dir_entry(i + 7),
		};

		say(INFO,
			" %02x:"
			" %04x:%04x %04x:%04x %04x:%04x %04x:%04x"
			" %04x:%04x %04x:%04x %04x:%04x %04x:%04x",
			i,
			e[0].base, e[0].loop,
			e[1].base, e[1].loop,
			e[2].base, e[2].loop,
			e[3].base, e[3].loop,
			e[4].base, e[4].loop,
			e[5].base, e[5].loop,
			e[6].base, e[6].loop,
			e[7].base, e[7].loop);
	}
}

static void init(void)
{
#if 0
	regs[REG_KON] = 0;
	regs[REG_KOFF] = 0;
	regs[REG_FLG] = 0x60;
	regs[REG_ED] = 0x0;
	dump_samples();
#endif
	dump_regs();
	dump_dir();
	ctr_init();
}

__attribute__((cold))
void dsp_restore(const uint8_t saved[static 0x80])
{
	memcpy(regs, saved, sizeof(regs));
	init();
}

__attribute__((cold))
void dsp_reset(void)
{
	memset(regs, 0, sizeof(regs));
	init();
}

static void store(const uint8_t addr, const uint8_t byte)
{
	const uint8_t prev = regs[addr];

	if (byte != prev) {
		mmio_trace("dsp store $%02x -> %s", byte, dsp_reg_name(addr));
	}

	switch (addr) {
	case REG_ENDX:
		/* all writes clear the register */
		regs[REG_ENDX] = 0;
		return;
	default:
		break;
	}

	regs[addr] = byte;
}

static inline uint8_t load(const uint8_t addr)
{
	const uint8_t byte = regs[addr];

	// say(TRACE, "dsp  load %s -> $%02x", dsp_reg_name(addr), byte);

	return byte;
}

__attribute__((noinline))
static void bad_store(const uint8_t addr, const uint8_t byte)
{
	say(WARN, "bad dsp store ($%02x) $%02x", addr, byte);
	abort();
}

__attribute__((noinline))
static uint8_t open_bus(const uint8_t addr)
{
	say(WARN, "bad dsp load ($%02x)", addr);
	abort();
	return 0xff;
}

void _dsp_store(const uint8_t addr, const uint8_t byte)
{
	if (unlikely(addr & 0x80)) {
		bad_store(addr, byte);
		return;
	}

	store(addr & 0x7f, byte);
}

__attribute__((pure))
uint8_t _dsp_load(const uint8_t addr)
{
	if (unlikely(addr & 0x80)) {
		return open_bus(addr);
	}

	return load(addr & 0x7f);
}
