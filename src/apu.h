#pragma once

/* The register space */
#define APU_TEST	0x00f0
#define APU_CTRL	0x00f1
#define APU_DSP_ADDR	0x00f2
#define APU_DSP_DATA	0x00f3
#define APU_IO0		0x00f4
#define APU_IO1		0x00f5
#define APU_IO2		0x00f6
#define APU_IO3		0x00f7
#define APU_AUX0	0x00f8
#define APU_AUX1	0x00f9
#define APU_T0DIV	0x00fa
#define APU_T1DIV	0x00fb
#define APU_T2DIV	0x00fc
#define APU_T0OUT	0x00fd
#define APU_T1OUT	0x00fe
#define APU_T2OUT	0x00ff

#define APU_REG(x)	(x & 0x0f)
#define APU_OFF(x, y)	(x - APU_REG(x))

#define ROM_ADDR	0xffc0

#define CTRL_T0		(1U << 0)
#define CTRL_T1		(1U << 1)
#define CTRL_T2		(1U << 2)
#define CTRL_IOC01	(1U << 4)
#define CTRL_IOC23	(1U << 5)
#define CTRL_BOOT_ROM	(1U << 7)

#define APU_MMIO_BASE 0x00f0
static inline bool apu_mmio_address(const uint16_t addr)
{
	/* There are 16 bytes of register space */
	return (addr & 0xfff0) == APU_MMIO_BASE;
}

#define IPL_ROM_MASK 0xffc0
#define IPL_ROM_BASE IPL_ROM_MASK
#define IPL_ROM_SIZE 0x40
static inline bool ipl_rom_address(const uint16_t addr)
{
	/* Top 64 bytes of address space are the IPL ROM */
	return (addr & IPL_ROM_MASK) == IPL_ROM_BASE;
}

void _apu_mmio_store(const uint16_t addr, const uint8_t byte);
uint8_t _apu_mmio_load(const uint16_t addr);
void _apu_update_clocks(unsigned int master_cycles_2048kHz);

void _apu_set_show_ipl_rom(const bool show);
bool _apu_get_show_ipl_rom(void);
