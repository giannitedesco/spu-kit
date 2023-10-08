#include <spu-kit/apu.h>

#include "system.h"
#include "apu.h"
#include "dsp.h"

#include <string.h>


//#define MMIO_TRACE
//#define TIMER_TRACE

#ifdef MMIO_TRACE
#define mmio_trace(...) say(INFO, __VA_ARGS__)
#else
#define mmio_trace(...) do { } while (0)
#endif

#ifdef TIMER_TRACE
static const uint8_t timer_trace[] = {
#include "../timer-trace.h"
};
static unsigned int timer_off;
#endif

/* APU registers */
struct timer {
	uint16_t cycles;
	uint16_t target;
	bool enabled;
};

static_assert(sizeof(struct apu_state) == 16);
union state {
	struct apu_state regs;
	uint8_t sram[16];
};


union state s;
static uint8_t io_in[4];
static uint8_t io_out[4];
static struct timer T[3];

__attribute__((pure))
struct apu_state apu_state_from_aram(const uint8_t aram[static 0x10000])
{
	return *((struct apu_state *)(aram + APU_MMIO_BASE));
}

static inline void dump_apu_state(void)
{
	say(INFO, "APU: [%c%c%c] T0=$%03x T1=$%03x T2=$%03x IPL-ROM=%s",
		(s.regs.ctrl & CTRL_T0) ? '0' : '-',
		(s.regs.ctrl & CTRL_T1) ? '1' : '-',
		(s.regs.ctrl & CTRL_T2) ? '2' : '-',
		s.regs.tdiv[0],
		s.regs.tdiv[1],
		s.regs.tdiv[2],
		_apu_get_show_ipl_rom() ? "EN" : "XX");
}

static struct timer timer_init(const uint8_t div_reg)
{
	return (struct timer) {
		.target = (div_reg) ? div_reg : 0x100,
		.enabled = true,
	};
}

static void timer_disable(struct timer *t)
{
	t->enabled = false;
}

static void timer_enable(const uint8_t index)
{
	const uint8_t div = s.regs.tdiv[index];

	if (!T[index].enabled)
		mmio_trace("timer_setup: APU_T0DIV $%02x", div);
	T[index] = timer_init(div);
	s.regs.tout[index] = 0;
}

__attribute__((noinline))
static void apu_ctrl_store(const uint8_t byte)
{
	mmio_trace("APU_CTRL store $%02x", byte);

	if (byte & CTRL_T0) {
		timer_enable(0);
	} else {
		mmio_trace("Timer: T0: disable");
		timer_disable(&T[0]);
	}

	if (byte & CTRL_T1) {
		timer_enable(1);
	} else {
		mmio_trace("Timer: T1: disable");
		timer_disable(&T[1]);
	}

	if (byte & CTRL_T2) {
		timer_enable(1);
	} else {
		mmio_trace("Timer: T2: disable");
		timer_disable(&T[2]);
	}

	if (byte & CTRL_IOC01) {
		s.regs.io_in[0] = 0;
		s.regs.io_in[1] = 0;
	}

	if (byte & CTRL_IOC23) {
		s.regs.io_in[2] = 0;
		s.regs.io_in[3] = 0;
	}

	_apu_set_show_ipl_rom(byte & CTRL_BOOT_ROM);
}

__attribute__((pure))
static uint8_t io_load(const uint16_t addr)
{
	xassert(addr < ARRAY_SIZE(io_out));
	return io_in[addr];
}

static void io_store(const uint16_t addr, const uint8_t byte)
{
	xassert(addr < ARRAY_SIZE(io_out));
	io_out[addr] = byte;
}

__attribute__((noinline))
void _apu_mmio_store(const uint16_t addr, const uint8_t byte)
{
	const uint8_t reg = APU_REG(addr);

	s.sram[reg] = byte;

	switch (reg) {
	case APU_REG(APU_TEST):
		say(WARN, "TEST store $%02x", byte);
		break;
	case APU_REG(APU_CTRL):
		mmio_trace("APU_CTRL store $%02x", byte);
		apu_ctrl_store(byte);
		break;
	case APU_REG(APU_DSP_ADDR):
		mmio_trace("APU_DSP_ADDR store $%02x", byte);
		break;
	case APU_REG(APU_DSP_DATA):
		_dsp_store(s.regs.dsp_addr, byte);
		break;
	case APU_REG(APU_IO0):
	case APU_REG(APU_IO1):
	case APU_REG(APU_IO2):
	case APU_REG(APU_IO3):
		/* IN/OUT ports are mirrored, so writes to these shouldn't affect anything */
		mmio_trace("APUIO%u store $%02x",
				APU_OFF(reg, APU_IO0),
				byte);
		io_store(addr - APU_IO0, byte);
		break;
	case APU_REG(APU_AUX0):
	case APU_REG(APU_AUX1):
		/* Aux registers just act like RAM */
		say(WARN, "AUX%u store $%02x",
				APU_OFF(reg, APU_AUX0),
				byte);
		break;
	case APU_REG(APU_T0DIV):
	case APU_REG(APU_T1DIV):
	case APU_REG(APU_T2DIV):
		/* You can write to these, they are ignored until the relevant
		 * control bit is toggled to on, and then they become effective
		 */
		mmio_trace("T%uDIV store $%02x",
				APU_OFF(reg, APU_T0DIV),
				byte);
		break;
	case APU_REG(APU_T0OUT):
	case APU_REG(APU_T1OUT):
	case APU_REG(APU_T2OUT):
		mmio_trace("T%uOUT store $%02x",
				APU_OFF(reg, APU_T0OUT),
				byte);
		break;
	default:
		unreachable();
	}
}

__attribute__((noinline))
uint8_t _apu_mmio_load(const uint16_t addr)
{
	const uint8_t reg = APU_REG(addr);
	const uint8_t byte = s.sram[reg];

	switch (reg) {
	case APU_REG(APU_TEST):
		say(WARN, "TEST load $%02x", byte);
		break;
	case APU_REG(APU_CTRL):
		mmio_trace("APU_CTRL load $%02x", byte);
		break;
	case APU_REG(APU_DSP_ADDR):
		mmio_trace("APU_DSP_ADDR load $%02x", byte);
		break;
	case APU_REG(APU_DSP_DATA):
		return _dsp_load(s.regs.dsp_addr);
	case APU_REG(APU_IO0):
	case APU_REG(APU_IO1):
	case APU_REG(APU_IO2):
	case APU_REG(APU_IO3):
		mmio_trace("APUIO%u load $%02x",
				APU_OFF(reg, APU_IO0),
				byte);
		return io_load(addr - APU_IO0);
	case APU_REG(APU_AUX0):
	case APU_REG(APU_AUX1):
		say(WARN, "AUX%u load $%02x",
				APU_OFF(reg, APU_AUX0),
				byte);
		break;
	case APU_REG(APU_T0DIV):
	case APU_REG(APU_T1DIV):
	case APU_REG(APU_T2DIV):
		mmio_trace("T%uDIV load $%02x",
				APU_OFF(reg, APU_T0DIV),
				byte);
		break;
	case APU_REG(APU_T0OUT):
	case APU_REG(APU_T1OUT):
	case APU_REG(APU_T2OUT):
		mmio_trace("T%uOUT load $%02x",
				APU_OFF(reg, APU_T0OUT),
				byte);
#ifndef TIMER_TRACE
		s.regs.tout[APU_OFF(reg, APU_T0OUT)] = 0;
		break;
#else
		do {
			xassert(APU_OFF(reg, APU_T0OUT) == timer_trace[timer_off + 0]);
			const uint8_t trace_byte = timer_trace[timer_off + 1];
			timer_off += 2;
			printf("ti/%d %02x\n", APU_OFF(reg, APU_T0OUT), trace_byte);
			return trace_byte;
		} while (0);
#endif
	default:
		unreachable();
	}

	return byte;
}

static inline void do_timer_tick(uint8_t index)
{
	struct timer * const t = &T[index];

	if (!t->enabled)
		return;

	if (++t->cycles >= t->target) {
		t->cycles = 0;
		s.regs.tout[index]++;
		s.regs.tout[index] &= 0xf;
	}
}

static void timer_tick(uint8_t index)
{
	xassert(index < ARRAY_SIZE(T));
	do_timer_tick(index);
}

#pragma GCC push_options
#pragma GCC diagnostic ignored "-Wsuggest-attribute=cold"
__attribute__((hot))
void _apu_update_clocks(unsigned int cycle)
{
	xassert((cycle & 0x0f) == 0);

	/* 64KHz clock (T2) */
	timer_tick(2);

	/* 32KHz is enough time for the DSP to output a sample */
	if ((cycle & 0x1f) == 0) {
		_dsp_run32();
	}

	/* 8KHz clock (T0 and T1) */
	if ((cycle & 0x7f) == 0) {
		timer_tick(0);
		timer_tick(1);
	}
}
#pragma GCC pop_options

__attribute__((cold))
void apu_restore(const struct apu_state st)
{
	s.regs = st;
	apu_ctrl_store(st.ctrl);
	s.regs = st;

	for (unsigned int i = 0; i < ARRAY_SIZE(io_in); i++) {
		io_in[i] = st.io_in[i];
	}

	dump_apu_state();
}

__attribute__((cold))
void apu_reset(void)
{
	s.regs = (struct apu_state) {
		0,
	};
	memset(&T, 0, sizeof(T));
	memset(io_out, 0, sizeof(io_out));
	_apu_set_show_ipl_rom(true);
}
