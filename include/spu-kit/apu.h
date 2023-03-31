#pragma once

struct apu_state {
	uint8_t test;
	uint8_t ctrl;

	uint8_t dsp_addr;
	uint8_t dsp_data;

	/* CPU I/O latches */
	uint8_t io_in[4];

	/* AUX */
	uint8_t aux[2];

	/* timer setup */
	uint8_t tdiv[3];
	uint8_t tout[3];
};

struct apu_state apu_state_from_aram(const uint8_t aram[static 0x10000]);
void apu_restore(const struct apu_state st);
void apu_reset(void);
