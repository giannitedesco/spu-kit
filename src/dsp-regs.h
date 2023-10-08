#pragma once

#include "system.h"

#include <stdint.h>
#include <stdio.h>

#define DSP_CHANNELS	8

enum vreg_e {
	VREG_VOLL,
	VREG_VOLR,
	VREG_PLO,
	VREG_PHI,
	VREG_SRCN,
	VREG_ADSR1,
	VREG_ADSR2,
	VREG_GAIN,
	VREG_ENVX,
	VREG_OUTX,

	VREG_COEF = 0x0f,
};

#define ADSR1_USE_ADSR			0x80
#define ADSR1_DECAY_RATE_SHIFT		3
#define ADSR1_DECAY_RATE_MASK		0xe
#define ADSR1_ATTACK_RATE_MASK		0xf

#define ADSR2_SUSTAIN_RATE_MASK		0x1f
#define ADSR2_SUSTAIN_LEVEL_SHIFT	5

#define GAIN_MODE_CUSTOM		0x80
#define GAIN_DIRECT_MASK		0x7f
#define GAIN_MODE_SHIFT			5
#define GAIN_MODE_MASK			0x03
#define GAIN_RATE_MASK			0x1f

struct vregs {
	int8_t voll;
	int8_t volr;
	union {
		struct {
			uint8_t pitch_lo;
			uint8_t pitch_hi;
		};
		uint16_t pitch;
	};
	uint8_t srcn;
	uint8_t adsr1;
	uint8_t adsr2;
	uint8_t gain;
	uint8_t envx;
	uint8_t outx;
	uint8_t _unused[2];
};

/* bottom 5 bits of FLG are noise frequency */
#define FLG_SOFT_RESET		(1U << 7)
#define FLG_MUTE		(1U << 6)
#define FLG_ECHO_DISABLED	(1U << 5)
#define FLG_FLAGS		(FLG_SOFT_RESET | FLG_MUTE | FLG_ECHO_DISABLED)

#define REG_MVOLL		0x0c
#define REG_MVOLR		0x1c
#define REG_EVOLL		0x2c
#define REG_EVOLR		0x3c
#define REG_KON			0x4c
#define REG_KOFF		0x5c
#define REG_FLG			0x6c
#define REG_ENDX		0x7c

#define REG_EFB			0x0d
#define REG__UNUSED		0x1d
#define REG_PMON		0x2d /* pitch modulation */
#define REG_NON			0x3d /* noise */
#define REG_EON			0x4d /* echo */
#define REG_DIR			0x5d /* directory addr (page number) */
#define REG_ESA			0x6d /* echo buffer (page number) */
#define REG_EDL			0x7d /* echo delay, 4 bits */

__attribute__((pure))
static inline uint8_t reg_coeff(const uint8_t chan)
{
	xassert(chan < DSP_CHANNELS);
	return (chan << 4) | VREG_COEF;
}

#define REG_COEF(x)	({ \
	uint8_t _reg_addr; \
	if (__builtin_constant_p(x)) { \
		static_assert(x < DSP_CHANNELS, "Bad channel index"); \
		_reg_addr = (x << 4) | VREG_COEF; \
	} else { \
		_reg_addr = reg_coeff(x); \
	} \
	_reg_addr; \
})

#define REG_COEF_0	REG_COEF(0)
#define REG_COEF_1	REG_COEF(1)
#define REG_COEF_2	REG_COEF(2)
#define REG_COEF_3	REG_COEF(3)
#define REG_COEF_4	REG_COEF(4)
#define REG_COEF_5	REG_COEF(5)
#define REG_COEF_6	REG_COEF(6)
#define REG_COEF_7	REG_COEF(7)

__attribute__((pure))
static inline const char *dsp_vreg_template(uint8_t addr_lo)
{
	switch (addr_lo & 0x0f) {
	case 0x0: return "VxVOLL";
	case 0x1: return "VxVOLR";
	case 0x2: return "VxP@lo";
	case 0x3: return "VxP@hi";
	case 0x4: return "VxSRCn";
	case 0x5: return "VxADSR1";
	case 0x6: return "VxADSR2";
	case 0x7: return "VxGAIN";
	case 0x8: return "VxENVX";
	case 0x9: return "VxOUTX";
	case 0xa: return NULL;
	case 0xb: return NULL;
	case 0xc: return NULL;
	case 0xd: return NULL;
	case 0xe: return NULL;
	case 0xf: return "VxCOEF";
	default: unreachable();
	}
}

static inline const char *dsp_reg_name(uint8_t addr)
{
	const uint8_t addr_lo = addr & 0x0f;
	static char buf[16];
	const char * const tmpl = dsp_vreg_template(addr_lo);

	if (tmpl) {
		const uint8_t chan = (addr >> 4);

		xassert(chan < DSP_CHANNELS);

		strcpy(buf, tmpl);
		buf[1] = '0' + chan;
		return buf;
	}

	switch (addr) {
	/* c block */
	case REG_MVOLL: return "MVOLL";
	case REG_MVOLR: return "MVOLR";
	case REG_EVOLL: return "EVOLL";
	case REG_EVOLR: return "EVOLR";
	case REG_KON: return "KON";
	case REG_KOFF: return "KOFF";
	case REG_FLG: return "FLG";
	case REG_ENDX: return "ENDX";

	/* d block */
	case REG_EFB: return "EFB";
	/* -- unused */
	case REG_PMON: return "PMON";
	case REG_NON: return "NON";
	case REG_EON: return "EON";
	case REG_DIR: return "DIR";
	case REG_ESA: return "ESA";
	case REG_EDL: return "EDL";

	/* e block of 8 is all unused */
	default:
		snprintf(buf, sizeof(buf), "REG$%02x", addr);
		return buf;
	}
}
