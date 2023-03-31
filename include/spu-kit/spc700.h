#pragma once

#include <stdint.h>

struct spc700_regs {
	uint16_t pc;
	uint8_t a;
	uint8_t x;
	uint8_t y;
	uint8_t psw;
	uint8_t sp;
};

void spc700_reset(void);
void spc700_restore(const struct spc700_regs r,
			const uint8_t in[static 0x10000],
			const uint8_t extra[static 0x40]);

void spc700_run_forever(void);
