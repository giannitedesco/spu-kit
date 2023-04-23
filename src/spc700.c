#include <string.h>

#include <spu-kit/system.h>
#include <spu-kit/spc700.h>
#include <spu-kit/dsp.h>

#include "aram.h"
#include "apu.h"

/* Accuracy */
// #define ACCURATE_SPC700
#ifdef ACCURATE_SPC700
#define ACCURATE_INSN_FETCH
#define ACCURATE_IPL_ROM
#endif

//#define TRACE_FOR_COMPARISON
//#define INSN_TRACE

#ifdef INSN_TRACE
#define insn_trace(...) say(TRACE, __VA_ARGS__)
#else
#define insn_trace(...) do { } while (0)
#endif

typedef struct bitaddr_s {
	union {
		struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
			uint16_t addr:13;
			uint16_t bit:3;
#else
			uint16_t bit:3;
			uint16_t addr:13;
#endif
		};
		uint16_t word;
	};
} bitaddr_t;

#define BITADDR_INIT(a, b) ((bitaddr_t){ .addr = a, .bit = b })

/* CPU registers */
static uint16_t pc;
static uint8_t a;
static uint8_t x;
static uint8_t y;
static uint8_t sp;
static bool carry;
static bool zero;
static bool psw_i; // interrupt enable
static bool half_carry;
static bool psw_b; // break
static bool psw_p; // direct page
static bool overflow;
static bool negative;

/* RAM */
uint8_t aram[0x10000];

/* Mask ROM */
static const uint8_t ipl_rom[0x40] = {
		/* Setup the stack */
/* reset: */	0xcd, 0xef,		/* mov  x,#$ef		*/
		0xbd,			/* mov  sp,x		*/

		/* Clear the zero page excluding registers at $00f0 and above */
/* @loop: */	0xe8, 0x00,		/* mov  a,#$00		*/
		0xc6,			/* mov  (x),a		*/
		0x1d,			/* dec  x		*/
		0xd0, 0xfc,		/* bne  @loop  ; $ffc5	*/

		/* Wait for the CPU to respond CC to our AA/BB */
		0x8f, 0xaa, 0xf4,	/* mov  $f4,#$aa	*/
		0x8f, 0xbb, 0xf5,	/* mov  $f5,#$bb	*/
/* @wait: */	0x78, 0xcc, 0xf4,	/* cmp  $f4,#$cc	*/
		0xd0, 0xfb,		/* bne  @wait  ; $ffcf	*/
		0x2f, 0x19,		/* bra  @main  ; $ffef	*/

		/* Transfer a block of data from CPU to ARAM */
/* block: */	0xeb, 0xf4,		/* mov  y,$f4		*/
		0xd0, 0xfc,		/* bne  block  ; $ffd6	*/
/* @bytes: */	0x7e, 0xf4,		/* cmp  y,$f4		*/
		0xd0, 0x0b,		/* bne  @retry ; $ffe9	*/
		0xe4, 0xf5,		/* mov  a,$f5		*/
		0xcb, 0xf4,		/* mov  $f4,y		*/
		0xd7, 0x00,		/* mov  ($00)+y,a	*/
		0xfc,			/* inc  y		*/
		0xd0, 0xf3,		/* bne  @bytes ; $ffda	*/
		0xab, 0x01,		/* inc  $01		*/
/* @retry: */	0x10, 0xef,		/* bpl  @bytes ; $ffda	*/
		0x7e, 0xf4,		/* cmp  y,$f4		*/
		0x10, 0xeb,		/* bpl  @bytes ; $ffda	*/

		/* Either fetch a block of data, or jump to address */
/* @main: */	0xba, 0xf6,		/* movw ya,$f6		*/
		0xda, 0x00,		/* movw $00,ya		*/
		0xba, 0xf4,		/* movw ya,$f4		*/
		0xc4, 0xf4,		/* mov  $f4,a		*/
		0xdd,			/* mov  a,y		*/
		0x5d,			/* mov  x,a		*/
		0xd0, 0xdb,		/* bne  block  ; $ffd6	*/
		0x1f, 0x00, 0x00,	/* jmp  ($0000+x)	*/

		/* reset vector to self $ffc0 */
		0xc0, 0xff,
};

enum {
	PSW_SHIFT_C,
	PSW_SHIFT_Z,
	PSW_SHIFT_I,
	PSW_SHIFT_H,
	PSW_SHIFT_B,
	PSW_SHIFT_P,
	PSW_SHIFT_V,
	PSW_SHIFT_N,
};
#define PSW_C (1U << PSW_SHIFT_C)
#define PSW_Z (1U << PSW_SHIFT_Z)
#define PSW_I (1U << PSW_SHIFT_I)
#define PSW_H (1U << PSW_SHIFT_H)
#define PSW_B (1U << PSW_SHIFT_B)
#define PSW_P (1U << PSW_SHIFT_P)
#define PSW_V (1U << PSW_SHIFT_V)
#define PSW_N (1U << PSW_SHIFT_N)

static void psw_decompose(const uint8_t psw)
{
	carry = psw & PSW_C;
	zero = psw & PSW_Z;
	psw_i = psw & PSW_I;
	half_carry = psw & PSW_H;
	psw_b = psw & PSW_B;
	psw_p = psw & PSW_P;
	overflow = psw & PSW_V;
	negative = psw & PSW_N;
}

static uint8_t psw_compose(void)
{
	return (carry << PSW_SHIFT_C)
		| (zero << PSW_SHIFT_Z)
		| (psw_i << PSW_SHIFT_I)
		| (half_carry << PSW_SHIFT_H)
		| (psw_b << PSW_SHIFT_B)
		| (psw_p << PSW_SHIFT_P)
		| (overflow << PSW_SHIFT_V)
		| (negative << PSW_SHIFT_N);
}

struct psw_str {
	union {
		struct {
			char n;
			char v;
			char p;
			char h;
			char i;
			char z;
			char c;
			char nul;
		};
		char str[8];
	};
};

static inline struct psw_str psw(void)
{
	return (struct psw_str){
		.c = (carry) ? 'C' : '-',
		.z = (zero) ? 'Z' : '-',
		.i = (psw_i) ? 'I' : '-',
		.h = (half_carry) ? 'H' : '-',
		.p = (psw_p) ? 'P' : '-',
		.v = (overflow) ? 'V' : '-',
		.n = (negative) ? 'N' : '-',
		.nul = '\0',
	};
}

static uint16_t stack_addr(void)
{
	return 0x0100 | sp;
}

static void branch_taken(void)
{
	/* This is where we would add 2 cycles to the time taken, if we were
	 * doing cycle-accurate emulation. Which we may add in the future.
	 */
}

static inline void dump_stack(const char *desc)
{
	uint16_t right_end = 0x01f0;

	while (--right_end > stack_addr()) {
		if (aram[right_end] != 0xff)
			break;
	}

	printf("stack %s ($%04x) ", desc, stack_addr());
	for (uint16_t addr = stack_addr() + 1; addr <= right_end; addr++) {
		printf("[%02x]", aram[addr]);
	}
	printf("\n");
}

static inline void dump_regs(void)
{
	say(INFO, "  SPC700: PC=%04x SP=%04x X=%02x Y=%02x A=%02x [%s]",
		pc, stack_addr(), x, y, a, psw().str);
}

static inline void dump_cpu_state(void)
{
	// dump_stack("dump");
	dump_regs();
}

static uint16_t get_ya(void)
{
	return (y << 8) | a;
}

static void set_ya(const uint16_t ya)
{
	y = ya >> 8;
	a = ya & 0xff;
}

static uint8_t extra_ram[IPL_ROM_SIZE];
static bool show_rom;

__attribute__((pure))
bool _apu_get_show_ipl_rom(void)
{
	return show_rom;
}

#ifdef ACCURATE_IPL_ROM
static uint8_t ipl_rom_load(const uint16_t addr)
{
	return ipl_rom[addr - IPL_ROM_MASK];
}

void _apu_set_show_ipl_rom(const bool show)
{
	show_rom = show;
}
#else
void _apu_set_show_ipl_rom(const bool show)
{
	if (show == show_rom)
		return;

	show_rom = show;

	if (show_rom) {
		memcpy(extra_ram, aram + IPL_ROM_BASE, IPL_ROM_SIZE);
		memcpy(aram + IPL_ROM_BASE, ipl_rom, IPL_ROM_SIZE);
	} else {
		memcpy(aram + IPL_ROM_BASE, extra_ram, IPL_ROM_SIZE);
	}
}
#endif

static inline void mem_store(const uint16_t addr, const uint8_t byte)
{
	if (apu_mmio_address(addr)) {
		/* APU register stores are forwarded to RAM */
		_apu_mmio_store(addr, byte);
	}
	aram[addr] = byte;
}

static inline uint8_t mem_load(const uint16_t addr)
{
	if (apu_mmio_address(addr)) {
		return _apu_mmio_load(addr);
	}
#ifdef ACCURATE_IPL_ROM
	if (unlikely(show_rom && ipl_rom_address(addr))) {
		return ipl_rom_load(addr);
	}
#endif
	return aram[addr];
}

static void set_regs(const struct spc700_regs r)
{
	pc = r.pc;
	a = r.a;
	x = r.x;
	y = r.y;
	sp = r.sp;

	psw_decompose(r.psw);
}

static uint16_t mem_load_word(const uint16_t addr)
{
	const uint8_t lo = mem_load(addr + 0);
	const uint8_t hi = mem_load(addr + 1);

	return (hi << 8) | lo;
}

static void mem_store_word(const uint16_t addr, const uint16_t word)
{
	mem_store(addr + 0, word & 0xff);
	mem_store(addr + 1, word >> 8);
}

static inline uint8_t fetch_insn(void)
{
#ifdef ACCURATE_INSN_FETCH
	return mem_load(pc++);
#else
#ifdef ACCURATE_IPL_ROM
	if (unlikely(show_rom && ipl_rom_address(addr))) {
		return ipl_rom_load(addr);
	}
#endif
	return aram[pc++];
#endif
}

static inline int8_t relative(void)
{
	return fetch_insn();
}

static inline uint8_t immediate(void)
{
	return fetch_insn();
}

static inline uint16_t direct_page_effective(const uint8_t addr)
{
	return (psw_p << 8) | addr;
}

static inline uint16_t direct_page(void)
{
	const uint8_t lo = fetch_insn();

	return direct_page_effective(lo);
}

__attribute__((cold))
void spc700_restore(const struct spc700_regs r,
			const uint8_t in[static 0x10000],
			const uint8_t extra[static 0x40])
{
	set_regs(r);
	memcpy(aram, in, sizeof(aram));
	memcpy(extra_ram, extra, sizeof(extra_ram));

	dump_cpu_state();
}

__attribute__((cold))
void spc700_reset(void)
{
	const uint16_t reset_vector = mem_load_word(0xfffe);
	const struct spc700_regs regs = {
		.pc = reset_vector,
		.sp = 0xef,
		.psw = PSW_Z,
	};

	set_regs(regs);
}

/* d+X - direct page address, indexed by X register*/
static inline uint16_t direct_page_x(void)
{
	const uint8_t lo = fetch_insn();

	return direct_page_effective(lo + x);
}

/* (d)+Y - pointer from direct page, indexed by Y register */
static inline uint16_t direct_page_indirect_y(void)
{
	const uint16_t ind_addr = direct_page();

	return mem_load_word(ind_addr) + y;
}

/* (d+X) - at direct page, indexed by X, is a pointer */
static inline uint16_t direct_page_x_indirect(void)
{
	const uint16_t addr = direct_page_x();

	return mem_load_word(addr);
}

/* (X) X register is a pointer into direct page */
static inline uint16_t indirect_x(void)
{
	return direct_page_effective(x);
}

/* (Y) Y register is a pointer into direct page */
static inline uint16_t indirect_y(void)
{
	return direct_page_effective(y);
}

/* !a - next two bytes after instruction encode the effective address */
static inline uint16_t absolute(void)
{
	const uint8_t lo = fetch_insn();
	const uint8_t hi = fetch_insn();

	return (hi << 8) | lo;
}

/* !a+X - absolute address, indexed by X register */
static inline uint16_t absolute_x(void)
{
	return absolute() + x;
}

/* !a+Y - absolute address, indexed by Y register */
static inline uint16_t absolute_y(void)
{
	return absolute() + y;
}

/* (!a+X) - absolute address, indexed by X, is a pointer */
static inline uint16_t absolute_x_indirect(void)
{
	uint16_t addr = absolute_x();

	return mem_load_word(addr);
}

__attribute__((always_inline))
static inline bitaddr_t bitaddr_init(const uint16_t word)
{
	return BITADDR_INIT(word & 0x1fff, word >> 13);
}

__attribute__((always_inline))
static inline bitaddr_t bitaddr(void)
{
	return bitaddr_init(absolute());
}

static bool bitaddr_load(const bitaddr_t addr)
{
	const uint8_t bit = (1U << addr.bit);
	const uint8_t byte = mem_load(addr.addr);

	return byte & bit;
}

static void bitaddr_store(const bitaddr_t addr, bool val)
{
	const uint8_t bit = (1U << addr.bit);
	const uint8_t byte = mem_load(addr.addr);
	const uint8_t result = (val) ? (byte | bit) : (byte & ~bit);

	mem_store(addr.addr, result);
}

static void push_byte(const uint8_t byte)
{
	mem_store(stack_addr(), byte);
	sp--;
	// dump_stack("push");
}

static uint8_t pop_byte(void)
{
	// dump_stack("pop ");
	sp++;
	return mem_load(stack_addr());
}

static void push_word(const uint16_t word)
{
	push_byte(word >> 8);
	push_byte(word & 0xff);
}

static uint16_t pop_word(void)
{
	const uint8_t lo = pop_byte();
	const uint8_t hi = pop_byte();
	const uint16_t ret = (hi << 8) | lo;

	return ret;
}

/* Set zero flag for an ALU op */
static void set_z(const uint8_t result)
{
	zero = !result;
}

/* Set zero and negative flag for an ALU op */
static void set_zn(const uint8_t result)
{
	set_z(result);
	negative = result & 0x80;
}

static void set_zn16(const uint16_t result)
{
	zero = !result;
	negative = result & 0x8000;
}

static uint8_t alu_asl(const uint8_t operand)
{
	const uint8_t result = (operand << 1);

	carry = operand & 0x80;
	set_zn(result);

	return result;
}

static uint8_t alu_rol(const uint8_t operand)
{
	const uint16_t result = ((uint16_t)operand << 1) | carry;
	const uint8_t trunc = result;

	carry = result & 0x100;
	set_zn(trunc);

	return trunc;
}

static uint8_t alu_lsr(const uint8_t operand)
{
	const uint8_t result = (operand >> 1);

	carry = operand & 0x1;
	set_zn(result);

	return result;
}

static uint8_t alu_ror(const uint8_t operand)
{
	const uint8_t result = (carry << 7) | (operand >> 1);

	carry = operand & 0x1;
	set_zn(result);

	return result;
}

static uint8_t alu_or(const uint8_t a, const uint8_t b)
{
	const uint8_t result = a | b;

	set_zn(result);

	return result;
}

static uint8_t alu_and(const uint8_t a, const uint8_t b)
{
	const uint8_t result = a & b;

	set_zn(result);

	return result;
}

static uint8_t alu_eor(const uint8_t a, const uint8_t b)
{
	const uint8_t result = a ^ b;

	set_zn(result);

	return result;
}

static uint8_t adc(const uint8_t a, const uint8_t b)
{
	const uint16_t result = a + b + carry;
	const uint8_t trunc = result;

	/* https://www.righto.com/2012/12/the-6502-overflow-flag-explained.html */
	carry = result & 0xff00;
	half_carry = (a ^ b ^ trunc) & 0x10;
	overflow = (a ^ trunc) & (b ^ trunc) & 0x80;

	return trunc;
}

__attribute__((always_inline))
static inline uint8_t sbc(const uint8_t a, const uint8_t b)
{
	return adc(a, ~b);
}

static uint8_t alu_adc(const uint8_t a, const uint8_t b)
{
	const uint8_t result = adc(a, b);

	set_zn(result);
	return result;
}

static uint8_t alu_sbc(const uint8_t a, const uint8_t b)
{
	const uint8_t result = sbc(a, b);

	set_zn(result);
	return result;
}

#if 0
static uint16_t alu_add_wide(const uint16_t a, const uint16_t b)
{
	const uint32_t result = a + b + carry;
	const uint16_t trunc = result;

	/* https://www.righto.com/2012/12/the-6502-overflow-flag-explained.html */
	carry = result & 0xff0000;
	overflow = (a ^ trunc) & (b ^ trunc) & 0x8000;

	set_zn16(trunc);

	return trunc;
}
#endif

static uint16_t alu_addw(const uint16_t a, const uint16_t b)
{
	uint16_t result;

	carry = false;
	result = adc(a, b) | (adc(a >> 8, b >> 8) << 8);
	set_zn16(result);
	return result;
#if 0
	carry = false;
	return alu_add_wide(a, b);
#endif
}

static uint16_t alu_subw(const uint16_t a, const uint16_t b)
{
	uint16_t result;

	carry = true;
	result = sbc(a, b) | (sbc(a >> 8, b >> 8) << 8);
	set_zn16(result);
	return result;
#if 0
	carry = true;
	return alu_add_wide(a, ~b);
#endif
}

static void alu_cmp(const uint8_t a, const uint8_t b)
{
	const int16_t cmp = (int16_t)a - (int16_t)b;

	carry = cmp >= 0;
	set_zn(cmp);
}

static void set_db(const uint8_t bit)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = operand | (1U << bit);

	mem_store(addr, result);

	insn_trace("set  d.%u      $%02x -> $%02x ($%04x)",
		bit, operand, result, addr);
}

static void clr_db(const uint8_t bit)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = operand & ~(1U << bit);

	mem_store(addr, result);

	insn_trace("clr  d.%u      $%02x -> $%02x ($%04x)",
		bit, operand, result, addr);
}

static void bbs_db(const uint8_t bit)
{
	const uint16_t addr = direct_page();
	const int8_t disp = relative();
	const uint8_t operand = mem_load(addr);
	const bool result = operand & (1U << bit);

	if (result) {
		pc += disp;
		branch_taken();
		insn_trace("bbs  d.%u      ($%04x) $%02x taken -> $%04x",
				bit, addr, operand, pc);
	} else {
		insn_trace("bbs  d.%u      ($%04x) $%02x not taken",
				bit, addr, operand);
	}
}

static void bbc_db(const uint8_t bit)
{
	const uint16_t addr = direct_page();
	const int8_t disp = relative();
	const uint8_t operand = mem_load(addr);
	const bool result = !(operand & (1U << bit));

	if (result) {
		pc += disp;
		branch_taken();
		insn_trace("bbc  d.%u      ($%04x) $%02x taken -> $%04x",
				bit, addr, operand, pc);
	} else {
		insn_trace("bbc  d.%u      ($%04x) $%02x not taken",
				bit, addr, operand);
	}
}

static void call(const uint16_t addr)
{
	push_word(pc);
	pc = addr;
}

static void tcall(const uint8_t slot)
{
	const uint16_t tbl_addr = 0xffc0 + ((0xf - (slot & 0xf)) << 1);
	const uint16_t addr = mem_load_word(tbl_addr);

	insn_trace("tcall %u       ($%04x) $%04x", slot, tbl_addr, addr);
	call(addr);
}

/* 0x00 - nop */
static void insn_nop(void)
{
	insn_trace("nop");
}

/* 0x9f - exchange the high and low nybbles of the accumulator */
static void insn_xcn(void)
{
	const uint8_t result = (a << 4) | (a >> 4);

	set_zn(result);

	insn_trace("xcn  A        $%02x -> $%02x", a, result);

	a = result;
}

/* 0x3f - call absolute */
static void insn_call_abs(void)
{
	const uint16_t addr = absolute();

	insn_trace("call !a       $%04x (RET=$%04x)", addr, pc);
	call(addr);
}

/* 0x01 table-call slot 0 */
static void insn_tcall_0(void)
{
	tcall(0);
}

/* 0x11 table-call slot 1 */
static void insn_tcall_1(void)
{
	tcall(1);
}

/* 0x21 table-call slot 2 */
static void insn_tcall_2(void)
{
	tcall(2);
}

/* 0x31 table-call slot 3 */
static void insn_tcall_3(void)
{
	tcall(3);
}

/* 0x41 table-call slot 4 */
static void insn_tcall_4(void)
{
	tcall(4);
}

/* 0x51 table-call slot 5 */
static void insn_tcall_5(void)
{
	tcall(5);
}

/* 0x61 table-call slot 6 */
static void insn_tcall_6(void)
{
	tcall(6);
}

/* 0x71 table-call slot 7 */
static void insn_tcall_7(void)
{
	tcall(7);
}

/* 0x08 table-call slot 8 */
static void insn_tcall_8(void)
{
	tcall(8);
}

/* 0x91 table-call slot 9 */
static void insn_tcall_9(void)
{
	tcall(0);
}

/* 0xa1 table-call slot 10 */
static void insn_tcall_10(void)
{
	tcall(10);
}

/* 0xb1 table-call slot 11 */
static void insn_tcall_11(void)
{
	tcall(11);
}

/* 0xc1 table-call slot 12 */
static void insn_tcall_12(void)
{
	tcall(12);
}

/* 0xd1 table-call slot 13 */
static void insn_tcall_13(void)
{
	tcall(13);
}

/* 0xe1 table-call slot 14 */
static void insn_tcall_14(void)
{
	tcall(14);
}

/* 0xf1 table-call slot 15 */
static void insn_tcall_15(void)
{
	tcall(15);
}

/* 0x02 set bit 0 in direct page byte */
static void insn_set_db_0(void)
{
	set_db(0);
}

/* 0x22 set bit 1 in direct page byte */
static void insn_set_db_1(void)
{
	set_db(1);
}

/* 0x42 set bit 2 in direct page byte */
static void insn_set_db_2(void)
{
	set_db(2);
}

/* 0x62 set bit 3 in direct page byte */
static void insn_set_db_3(void)
{
	set_db(3);
}

/* 0x82 set bit 4 in direct page byte */
static void insn_set_db_4(void)
{
	set_db(4);
}

/* 0xa2 set bit 5 in direct page byte */
static void insn_set_db_5(void)
{
	set_db(5);
}

/* 0xc2 set bit 6 in direct page byte */
static void insn_set_db_6(void)
{
	set_db(6);
}

/* 0xe2 set bit 7 in direct page byte */
static void insn_set_db_7(void)
{
	set_db(7);
}

/* 0x02 clear bit 0 in direct page byte */
static void insn_clr_db_0(void)
{
	clr_db(0);
}

/* 0x22 clear bit 1 in direct page byte */
static void insn_clr_db_1(void)
{
	clr_db(1);
}

/* 0x42 clear bit 2 in direct page byte */
static void insn_clr_db_2(void)
{
	clr_db(2);
}

/* 0x62 clear bit 3 in direct page byte */
static void insn_clr_db_3(void)
{
	clr_db(3);
}

/* 0x82 clear bit 4 in direct page byte */
static void insn_clr_db_4(void)
{
	clr_db(4);
}

/* 0xa2 clear bit 5 in direct page byte */
static void insn_clr_db_5(void)
{
	clr_db(5);
}

/* 0xc2 clear bit 6 in direct page byte */
static void insn_clr_db_6(void)
{
	clr_db(6);
}

/* 0xe2 clear bit 7 in direct page byte */
static void insn_clr_db_7(void)
{
	clr_db(7);
}

/* 0x02 branch if bit 0 set in direct page byte */
static void insn_bbs_db_0(void)
{
	bbs_db(0);
}

/* 0x22 branch if bit 1 set in direct page byte */
static void insn_bbs_db_1(void)
{
	bbs_db(1);
}

/* 0x42 branch if bit 2 set in direct page byte */
static void insn_bbs_db_2(void)
{
	bbs_db(2);
}

/* 0x62 branch if bit 3 set in direct page byte */
static void insn_bbs_db_3(void)
{
	bbs_db(3);
}

/* 0x82 branch if bit 4 set in direct page byte */
static void insn_bbs_db_4(void)
{
	bbs_db(4);
}

/* 0xa2 branch if bit 5 set in direct page byte */
static void insn_bbs_db_5(void)
{
	bbs_db(5);
}

/* 0xc2 branch if bit 6 set in direct page byte */
static void insn_bbs_db_6(void)
{
	bbs_db(6);
}

/* 0xe2 branch if bit 7 set in direct page byte */
static void insn_bbs_db_7(void)
{
	bbs_db(7);
}

/* 0x02 branch if bit 0 not set in direct page byte */
static void insn_bbc_db_0(void)
{
	bbc_db(0);
}

/* 0x22 branch if bit 1 not set in direct page byte */
static void insn_bbc_db_1(void)
{
	bbc_db(1);
}

/* 0x42 branch if bit 2 not set in direct page byte */
static void insn_bbc_db_2(void)
{
	bbc_db(2);
}

/* 0x62 branch if bit 3 not set in direct page byte */
static void insn_bbc_db_3(void)
{
	bbc_db(3);
}

/* 0x82 branch if bit 4 not set in direct page byte */
static void insn_bbc_db_4(void)
{
	bbc_db(4);
}

/* 0xa2 branch if bit 5 not set in direct page byte */
static void insn_bbc_db_5(void)
{
	bbc_db(5);
}

/* 0xc2 branch if bit 6 not set in direct page byte */
static void insn_bbc_db_6(void)
{
	bbc_db(6);
}

/* 0xe2 branch if bit 7 not set in direct page byte */
static void insn_bbc_db_7(void)
{
	bbc_db(7);
}

/* 0x0b - Arithmetic shift left : direct page */
static void insn_asl_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_asl(operand);

	mem_store(addr, result);

	insn_trace("asl  d        $%02x << 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0x0c - Arithmetic shift left : absolute */
static void insn_asl_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_asl(operand);

	mem_store(addr, result);

	insn_trace("asl  !a       $%02x << 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0x1b - Arithmetic shift left X-indexed direct-page */
static void insn_asl_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint16_t operand = mem_load(addr);
	const uint8_t result = alu_asl(operand);

	insn_trace("asl  d+X      $%02x << 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);

	mem_store(addr, result);
}

/* 0x1c - Arithmetic shift left A register */
static void insn_asl_a(void)
{
	const uint8_t result = alu_asl(a);

	insn_trace("asl  A        $%02x << 1 -> $%02x [%s]",
		a, result, psw().str);

	a = result;
}

/* 0x2b - Rotate-left : direct page */
static void insn_rol_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_rol(operand);

	mem_store(addr, result);

	insn_trace("rol  d        $%02x <<< 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0x2c - Rotate-left : absolute */
static void insn_rol_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_rol(operand);

	mem_store(addr, result);

	insn_trace("rol  !a       $%02x <<< 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0x3b - Rotate-left X-indexed direct-page */
static void insn_rol_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint16_t operand = mem_load(addr);
	const uint8_t result = alu_rol(operand);

	insn_trace("rol  d+X      $%02x <<< 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);

	mem_store(addr, result);
}

/* 0x3c - Rotate-left A register */
static void insn_rol_a(void)
{
	const uint8_t result = alu_rol(a);

	insn_trace("rol  A        $%02x <<< 1 -> $%02x [%s]",
		a, result, psw().str);

	a = result;
}

/* 0x4b - Logical Shift Right : direct page */
static void insn_lsr_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_lsr(operand);

	mem_store(addr, result);

	insn_trace("lsr  d        $%02x >> 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0x4c - Logical Shift Right : absolute */
static void insn_lsr_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_lsr(operand);

	mem_store(addr, result);

	insn_trace("lsr  !a       $%02x >> 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0x5b - Logical Shift Right X-indexed direct-page */
static void insn_lsr_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint16_t operand = mem_load(addr);
	const uint8_t result = alu_lsr(operand);

	insn_trace("lsr  d+X      $%02x >> 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);

	mem_store(addr, result);
}

/* 0x5c - Logical Shift Right A register */
static void insn_lsr_a(void)
{
	const uint8_t result = alu_lsr(a);

	insn_trace("lsr  A        $%02x >> 1 -> $%02x [%s]",
		a, result, psw().str);

	a = result;
}

/* 0x6b - Rotate Right : direct page */
static void insn_ror_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_ror(operand);

	mem_store(addr, result);

	insn_trace("ror  d        $%02x >>> 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0x6c - Rotate Right : absolute */
static void insn_ror_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_ror(operand);

	mem_store(addr, result);

	insn_trace("ror  !a       $%02x >>> 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0x7b - Rotate Right X-indexed direct-page */
static void insn_ror_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint16_t operand = mem_load(addr);
	const uint8_t result = alu_ror(operand);

	insn_trace("ror  d+X      $%02x >>> 1 -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);

	mem_store(addr, result);
}

/* 0x7c - Rotate Right A register */
static void insn_ror_a(void)
{
	const uint8_t result = alu_ror(a);

	insn_trace("ror  A        $%02x >>> 1 -> $%02x [%s]",
		a, result, psw().str);

	a = result;
}

/* 0x8b - Decrement direct-page */
static void insn_dec_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = operand - 1;

	set_zn(result);
	insn_trace("dec  d        $%02x -> $%02x ($%04x)",
			operand, result, addr);

	mem_store(addr, result);
}

/* 0x8c - Decrement : absolute */
static void insn_dec_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = operand - 1;

	set_zn(result);
	insn_trace("dec  !a       $%02x -> $%02x ($%04x)",
			operand, result, addr);

	mem_store(addr, result);
}

/* 0x9b - decrement X-indexed direct-page */
static void insn_dec_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint16_t operand = mem_load(addr);
	const uint8_t result = operand - 1;

	set_zn(result);
	insn_trace("dec  d+X      $%02x-- -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);

	mem_store(addr, result);
}

/* 0x9c - decrement A register */
static void insn_dec_a(void)
{
	const uint8_t result = a - 1;

	set_zn(result);
	insn_trace("dec  A        $%02x-- -> $%02x [%s]",
		a, result, psw().str);

	a = result;
}

/* 0xab - Increment direct-page */
static void insn_inc_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = operand + 1;

	set_zn(result);
	insn_trace("inc  d        $%02x++ -> $%02x ($%04x)",
			operand, result, addr);

	mem_store(addr, result);
}

/* 0xac - Increment : absolute */
static void insn_inc_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = operand + 1;

	mem_store(addr, result);
	set_zn(result);

	insn_trace("inc  !a       $%02x++ -> $%02x ($%04x) [%s]",
		operand, result, addr, psw().str);
}

/* 0xbb - Increment direct-page */
static void insn_inc_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = operand + 1;

	set_zn(result);
	insn_trace("inc  d+x      $%02x -> $%02x ($%04x)",
			operand, result, addr);

	mem_store(addr, result);
}

/* 0xbc - Increment A register */
static void insn_inc_a(void)
{
	const uint8_t result = a + 1;

	set_zn(result);
	insn_trace("inc  A        $%02x++ -> $%02x [%s]",
		a, result, psw().str);

	a = result;
}

/* 0xcb - store Y register into direct page */
static void insn_mov_dp_y(void)
{
	const uint16_t addr = direct_page();

	insn_trace("mov  d,Y      $%02x ($%04x)", y, addr);
	mem_store(addr, y);
}

/* 0x04 - OR A with direct-page */
static void insn_or_a_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(a, operand);

	insn_trace("or   A,d      $%02x |= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x05 - OR A with absolute */
static void insn_or_a_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(a, operand);

	insn_trace("or   A,!a     $%02x |= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x06 - OR A with indirect X */
static void insn_or_a_ix(void)
{
	const uint16_t addr = indirect_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(a, operand);

	insn_trace("or   A,(X)    $%02x |= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x07 - OR A with X-indexed direct-page */
static void insn_or_a_dx_ind(void)
{
	const uint16_t addr = direct_page_x_indirect();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(a, operand);

	insn_trace("or   A,(d+X)  $%02x |= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x08 - OR immediate into A */
static void insn_or_a_imm(void)
{
	const uint8_t imm = immediate();
	const uint8_t result = alu_or(a, imm);

	insn_trace("or   A,#i     $%02x |= #$%02x -> $%02x [%s]",
		a, imm, result, psw().str);

	a = result;
}

/* 0x09 - OR A with X-indexed direct-page */
static void insn_or_dp_dp(void)
{
	const uint16_t src = direct_page();
	const uint16_t dst = direct_page();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_or(src_val, dst_val);

	insn_trace("or   d,d      $%02x | $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0x14 - OR A with X-indexed direct-page */
static void insn_or_a_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(a, operand);

	insn_trace("or   A,d+X    $%02x |= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x15 - OR A with X-indexed absolute */
static void insn_or_a_absx(void)
{
	const uint16_t addr = absolute_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(a, operand);

	insn_trace("or   A,!a+X   $%02x |= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x16 - OR A with Y-indexed direct-page */
static void insn_or_a_absy(void)
{
	const uint16_t addr = absolute_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(a, operand);

	insn_trace("or   A,!a+Y   $%02x |= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x17 - OR A with Y-indexed direct-page */
static void insn_or_a_dpiy(void)
{
	const uint16_t addr = direct_page_indirect_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(a, operand);

	insn_trace("or   A,(d)+Y  $%02x |= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x18 - OR immediate into direct-page byte */
static void insn_or_dp_imm(void)
{
	const uint8_t imm = immediate();
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_or(imm, operand);

	mem_store(addr, result);

	insn_trace("or   d,#i     $%02x |= #$%02x -> $%02x ($%04x) [%s]",
		operand, imm, result, addr, psw().str);
}

/* 0x19 - OR A with X-indexed direct-page */
static void insn_or_ix_iy(void)
{
	const uint16_t dst = indirect_x();
	const uint16_t src = indirect_y();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_or(src_val, dst_val);

	insn_trace("or   (X),(Y)  $%02x | $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0x24 - AND A with direct-page */
static void insn_and_a_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(a, operand);

	insn_trace("and  A,d      $%02x &= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x25 - AND A with absolute */
static void insn_and_a_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(a, operand);

	insn_trace("and  A,!a     $%02x &= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x26 - AND A with indirect X */
static void insn_and_a_ix(void)
{
	const uint16_t addr = indirect_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(a, operand);

	insn_trace("and  A,(X)    $%02x &= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x27 - AND A with X-indexed direct-page */
static void insn_and_a_dx_ind(void)
{
	const uint16_t addr = direct_page_x_indirect();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(a, operand);

	insn_trace("and  A,(d+X)  $%02x &= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x28 - AND immediate into A */
static void insn_and_a_imm(void)
{
	const uint8_t imm = immediate();
	const uint8_t result = alu_and(a, imm);

	insn_trace("and  A,#i     $%02x &= #$%02x -> $%02x [%s]",
		a, imm, result, psw().str);

	a = result;
}

/* 0x29 - AND A with X-indexed direct-page */
static void insn_and_dp_dp(void)
{
	const uint16_t src = direct_page();
	const uint16_t dst = direct_page();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_and(src_val, dst_val);

	insn_trace("and  d,d      $%02x & $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0x34 - AND A with X-indexed direct-page */
static void insn_and_a_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(a, operand);

	insn_trace("and  A,d+X    $%02x &= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x35 - AND A with X-indexed absolute */
static void insn_and_a_absx(void)
{
	const uint16_t addr = absolute_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(a, operand);

	insn_trace("and  A,!a+X   $%02x &= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x36 - AND A with Y-indexed direct-page */
static void insn_and_a_absy(void)
{
	const uint16_t addr = absolute_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(a, operand);

	insn_trace("and  A,!a+Y   $%02x &= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x37 - AND A with Y-indexed direct-page */
static void insn_and_a_dpiy(void)
{
	const uint16_t addr = direct_page_indirect_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(a, operand);

	insn_trace("and  A,(d)+Y  $%02x &= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x38 - AND immediate into direct-page byte */
static void insn_and_dp_imm(void)
{
	const uint8_t imm = immediate();
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_and(imm, operand);

	mem_store(addr, result);

	insn_trace("and  d,#i     $%02x &= #$%02x -> $%02x ($%04x) [%s]",
		operand, imm, result, addr, psw().str);
}

/* 0x39 - AND A with X-indexed direct-page */
static void insn_and_ix_iy(void)
{
	const uint16_t dst = indirect_x();
	const uint16_t src = indirect_y();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_and(src_val, dst_val);

	insn_trace("and  (X),(Y)  $%02x & $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0x44 - EOR A with direct-page */
static void insn_eor_a_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(a, operand);

	insn_trace("eor  A,d      $%02x ^= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x45 - EOR A with absolute */
static void insn_eor_a_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(a, operand);

	insn_trace("eor  A,!a     $%02x ^= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x46 - EOR A with indirect X */
static void insn_eor_a_ix(void)
{
	const uint16_t addr = indirect_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(a, operand);

	insn_trace("eor  A,(X)    $%02x ^= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x47 - EOR A with X-indexed direct-page */
static void insn_eor_a_dx_ind(void)
{
	const uint16_t addr = direct_page_x_indirect();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(a, operand);

	insn_trace("eor  A,(d+X)  $%02x ^= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x48 - EOR immediate into A */
static void insn_eor_a_imm(void)
{
	const uint8_t imm = immediate();
	const uint8_t result = alu_eor(a, imm);

	insn_trace("eor  A,#i     $%02x ^= #$%02x -> $%02x [%s]",
		a, imm, result, psw().str);

	a = result;
}

/* 0x49 - EOR A with X-indexed direct-page */
static void insn_eor_dp_dp(void)
{
	const uint16_t src = direct_page();
	const uint16_t dst = direct_page();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_eor(src_val, dst_val);

	insn_trace("eor  d,d      $%02x ^ $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0x54 - EOR A with X-indexed direct-page */
static void insn_eor_a_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(a, operand);

	insn_trace("eor  A,d+X    $%02x ^= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x55 - EOR A with X-indexed absolute */
static void insn_eor_a_absx(void)
{
	const uint16_t addr = absolute_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(a, operand);

	insn_trace("eor  A,!a+X   $%02x ^= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x56 - EOR A with Y-indexed direct-page */
static void insn_eor_a_absy(void)
{
	const uint16_t addr = absolute_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(a, operand);

	insn_trace("eor  A,!a+Y   $%02x ^= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x57 - EOR A with Y-indexed direct-page */
static void insn_eor_a_dpiy(void)
{
	const uint16_t addr = direct_page_indirect_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(a, operand);

	insn_trace("eor  A,(d)+Y  $%02x ^= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x58 - EOR immediate into direct-page byte */
static void insn_eor_dp_imm(void)
{
	const uint8_t imm = immediate();
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_eor(imm, operand);

	mem_store(addr, result);

	insn_trace("eor  d,#i     $%02x ^= #$%02x -> $%02x ($%04x) [%s]",
		operand, imm, result, addr, psw().str);
}

/* 0x59 - EOR A with X-indexed direct-page */
static void insn_eor_ix_iy(void)
{
	const uint16_t dst = indirect_x();
	const uint16_t src = indirect_y();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_eor(src_val, dst_val);

	insn_trace("eor  (X),(Y)  $%02x ^ $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0xdc - decrement Y register */
static void insn_dec_y(void)
{
	const uint8_t result = y - 1;

	set_zn(result);
	insn_trace("dec  Y        $%02x-- -> $%02x [%s]",
		y, result, psw().str);

	y = result;
}

/* 0xfc - increment Y register */
static void insn_inc_y(void)
{
	const uint8_t result = y + 1;

	set_zn(result);
	insn_trace("inc  Y        $%02x++ -> $%02x [%s]",
		y, result, psw().str);

	y = result;
}

/* 0x1f - jump absolute x-indexed indirect*/
static void insn_jmp_absx_ind(void)
{
	const uint16_t addr = absolute_x_indirect();

	pc = addr;
	insn_trace("jmp  [!a+x]   $%04x", addr);
}

/* 0x5f - jump absolute */
static void insn_jmp_abs(void)
{
	const uint16_t addr = absolute();

	pc = addr;
	insn_trace("jmp  !a       $%04x", addr);
}

/* 0x1d - decrement X register */
static void insn_dec_x(void)
{
	const uint8_t result = x - 1;

	set_zn(result);
	insn_trace("dec  X        $%02x-- -> $%02x [%s]",
		x, result, psw().str);

	x = result;
}

/* 0x0d - push psw */
static void insn_push_psw(void)
{
	const uint8_t psw = psw_compose();

	push_byte(psw);
	insn_trace("push PSW      $%02x", psw);
}

/* 0x2d - push a */
static void insn_push_a(void)
{
	push_byte(a);
	insn_trace("push A        $%02x", a);
}

/* 0x3d - Increment X register */
static void insn_inc_x(void)
{
	const uint8_t result = x + 1;

	set_zn(result);
	insn_trace("inc  X        $%02x++ -> $%02x [%s]",
		x, result, psw().str);

	x = result;
}

/* 0x4d - push x */
static void insn_push_x(void)
{
	push_byte(x);
	insn_trace("push X        $%02x", x);
}

/* 0x6d - push y */
static void insn_push_y(void)
{
	push_byte(y);
	insn_trace("push Y        $%02x", y);
}

/* 0x5d - copy A register into X */
static void insn_mov_x_a(void)
{
	set_zn(a);

	insn_trace("mov  X,A      $%02x -> $%02x [%s]", x, a, psw().str);

	x = a;
}

/* 0x8d - store immediate value into Y */
static void insn_mov_y_imm(void)
{
	const uint8_t operand = immediate();

	set_zn(operand);
	y = operand;

	insn_trace("mov  Y,#i     #$%02x [%s]",
		operand, psw().str);
}

/* 0xcd - store immediate value into X */
static void insn_mov_x_imm(void)
{
	const uint8_t operand = immediate();

	set_zn(operand);
	x = operand;

	insn_trace("mov  X,#i     #$%02x [%s]", operand, psw().str);
}

/* 0xeb - load direct-page byte into Y */
static void insn_mov_y_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);

	insn_trace("mov  Y,d      $%02x -> $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);

	y = operand;
}

/* 0xfb - load x-indexed direct page byte into Y */
static void insn_mov_y_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	y = operand;

	insn_trace("mov  Y,d+X    Y := $%02x ($%04x) [%s]",
		y, addr, psw().str);
}

/* 0x7d - copy X register into A */
static void insn_mov_a_x(void)
{
	set_zn(x);
	a = x;

	insn_trace("mov  A,X      A := $%02x [%s]", a, psw().str);

}

/* 0xd8 - store X register into direct page */
static void insn_mov_dp_x(void)
{
	const uint16_t addr = direct_page();

	insn_trace("mov  d,X      $%02x ($%04x)", y, addr);
	mem_store(addr, x);
}

/* 0xdd - copy Y register into A */
static void insn_mov_a_y(void)
{
	set_zn(y);
	a = y;

	insn_trace("mov  A,Y      A := $%02x [%s]", a, psw().str);

}

/* 0xfd - copy A register into Y */
static void insn_mov_y_a(void)
{
	set_zn(a);
	y = a;

	insn_trace("mov  Y,A      Y := $%02x [%s]", y, psw().str);
}

/* 0x60 - clear carry */
static void insn_clrc(void)
{
	carry = false;
	insn_trace("clrc");
}

/* 0x80 - clear carry */
static void insn_setc(void)
{
	carry = true;
	insn_trace("setc");
}

/* 0xed - flip carry */
static void insn_notc(void)
{
	carry = !carry;
	insn_trace("notc");
}

/* 0xe0 - clear overflow and half-cary */
static void insn_clrv(void)
{
	overflow = false;
	half_carry = false;
	insn_trace("clrv");
}

/* 0x20 - clear direct-page (to zero-page) */
static void insn_clrp(void)
{
	psw_p = false;
	insn_trace("clrp");
}

/* 0x40 - set direct-page (to stack-page) */
static void insn_setp(void)
{
	psw_p = true;
	insn_trace("setp");
}

/* 0xa0 - enable interrupts */
static void insn_ei(void)
{
	psw_i = true;
	insn_trace("ei");
}

/* 0xc0 - disable interrupts */
static void insn_di(void)
{
	psw_i = false;
	insn_trace("di");
}

/* 0x64 - Compare A with direct-page */
static void insn_cmp_a_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);

	alu_cmp(a, operand);

	insn_trace("cmp  A,d      $%02x == $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);
}

/* 0x65 - Compare A with absolute */
static void insn_cmp_a_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);

	alu_cmp(a, operand);

	insn_trace("cmp  A,!a     $%02x == $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);
}

/* 0x66 - Compare A with indirect X */
static void insn_cmp_a_ix(void)
{
	const uint16_t addr = indirect_x();
	const uint8_t operand = mem_load(addr);

	alu_cmp(a, operand);

	insn_trace("cmp  A,(X)    $%02x == $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);
}

/* 0x67 - Compare A with X-indexed direct-page */
static void insn_cmp_a_dx_ind(void)
{
	const uint16_t addr = direct_page_x_indirect();
	const uint8_t operand = mem_load(addr);

	alu_cmp(a, operand);

	insn_trace("cmp  A,(d+X)  $%02x == $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);
}

/* 0x68 - Compare immediate with A */
static void insn_cmp_a_imm(void)
{
	const uint8_t imm = immediate();

	alu_cmp(a, imm);

	insn_trace("cmp  A,#i     $%02x == #$%02x [%s]",
		a, imm, psw().str);
}

/* 0x69 - Compare direct-page with direct-page */
static void insn_cmp_dp_dp(void)
{
	const uint16_t src = direct_page();
	const uint16_t dst = direct_page();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);

	alu_cmp(src_val, dst_val);

	insn_trace("cmp  d,d      $%02x == $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, dst, src, psw().str);
}

/* 0x7e - Compare Y with direct-page */
static void insn_cmp_y_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);

	alu_cmp(y, operand);

	insn_trace("cmp  Y,d      $%02x == $%02x ($%04x) [%s]",
		y, operand, addr, psw().str);
}

/* 0xad - compare Y register with immediate value */
static void insn_cmp_y_imm(void)
{
	const uint8_t operand = immediate();

	alu_cmp(y, operand);

	insn_trace("cmp  Y,#i     $%02x - #$%02x [%s]", a, operand, psw().str);
}

/* 0x6f - return */
static void insn_ret(void)
{
	const uint16_t reta = pop_word();

	insn_trace("ret  ($%04x)", reta);
	pc = reta;
}

/* 0x74 - Compare A with X-indexed direct-page */
static void insn_cmp_a_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);

	alu_cmp(a, operand);

	insn_trace("cmp  A,d+X    $%02x == $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);
}

/* 0x75 - Compare A with X-indexed absolute */
static void insn_cmp_a_absx(void)
{
	const uint16_t addr = absolute_x();
	const uint8_t operand = mem_load(addr);

	alu_cmp(a, operand);

	insn_trace("cmp  A,!a+X   $%02x == $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);
}

/* 0x76 - Compare A with Y-indexed direct-page */
static void insn_cmp_a_absy(void)
{
	const uint16_t addr = absolute_y();
	const uint8_t operand = mem_load(addr);

	alu_cmp(a, operand);

	insn_trace("cmp  A,!a+Y   $%02x == $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);
}

/* 0x77 - Compare A with Y-indexed direct-page */
static void insn_cmp_a_dpiy(void)
{
	const uint16_t addr = direct_page_indirect_y();
	const uint8_t operand = mem_load(addr);

	alu_cmp(a, operand);

	insn_trace("cmp  A,(d)+Y  $%02x == $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);
}

/* 0x78 - Compare immediate into direct-page byte */
static void insn_cmp_dp_imm(void)
{
	const uint8_t imm = immediate();
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);

	alu_cmp(operand, imm);

	insn_trace("cmp  d,#i     $%02x == #$%02x ($%04x) [%s]",
		operand, imm, addr, psw().str);
}

/* 0x79 - Compare A with X-indexed direct-page */
static void insn_cmp_ix_iy(void)
{
	const uint16_t dst = indirect_x();
	const uint16_t src = indirect_y();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);

	alu_cmp(src_val, dst_val);

	insn_trace("cmp  (X),(Y)  $%02x == $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, dst, src, psw().str);
}

/* 0x84 - ADC A with direct-page */
static void insn_adc_a_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(a, operand);

	insn_trace("adc  A,d      $%02x += $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x85 - ADC A with absolute */
static void insn_adc_a_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(a, operand);

	insn_trace("adc  A,!a     $%02x += $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x86 - ADC A with indirect X */
static void insn_adc_a_ix(void)
{
	const uint16_t addr = indirect_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(a, operand);

	insn_trace("adc  A,(X)    $%02x += $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x87 - ADC A with X-indexed direct-page */
static void insn_adc_a_dx_ind(void)
{
	const uint16_t addr = direct_page_x_indirect();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(a, operand);

	insn_trace("adc  A,(d+X)  $%02x += $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x88 - ADC immediate into A */
static void insn_adc_a_imm(void)
{
	const uint8_t imm = immediate();
	const uint8_t result = alu_adc(a, imm);

	insn_trace("adc  A,#i     $%02x += #$%02x -> $%02x [%s]",
		a, imm, result, psw().str);

	a = result;
}

/* 0x89 - ADC A with X-indexed direct-page */
static void insn_adc_dp_dp(void)
{
	const uint16_t src = direct_page();
	const uint16_t dst = direct_page();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_adc(src_val, dst_val);

	insn_trace("adc  d,d      $%02x + $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0x94 - ADC A with X-indexed direct-page */
static void insn_adc_a_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(a, operand);

	insn_trace("adc  A,d+X    $%02x += $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x95 - ADC A with X-indexed absolute */
static void insn_adc_a_absx(void)
{
	const uint16_t addr = absolute_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(a, operand);

	insn_trace("adc  A,!a+X   $%02x += $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x96 - ADC A with Y-indexed direct-page */
static void insn_adc_a_absy(void)
{
	const uint16_t addr = absolute_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(a, operand);

	insn_trace("adc  A,!a+Y   $%02x += $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x97 - ADC A with Y-indexed direct-page */
static void insn_adc_a_dpiy(void)
{
	const uint16_t addr = direct_page_indirect_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(a, operand);

	insn_trace("adc  A,(d)+Y  $%02x += $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0x98 - ADC immediate into direct-page byte */
static void insn_adc_dp_imm(void)
{
	const uint8_t imm = immediate();
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_adc(operand, imm);

	mem_store(addr, result);

	insn_trace("adc  d,#i     $%02x += #$%02x -> $%02x ($%04x) [%s]",
		operand, imm, result, addr, psw().str);
}

/* 0x99 - ADC A with X-indexed direct-page */
static void insn_adc_ix_iy(void)
{
	const uint16_t dst = indirect_x();
	const uint16_t src = indirect_y();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_adc(src_val, dst_val);

	insn_trace("adc  (X),(Y)  $%02x + $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0xa4 - SBC A with direct-page */
static void insn_sbc_a_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(a, operand);

	insn_trace("sbc  A,d      $%02x -= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0xa5 - SBC A with absolute */
static void insn_sbc_a_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(a, operand);

	insn_trace("sbc  A,!a     $%02x -= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0xa6 - SBC A with indirect X */
static void insn_sbc_a_ix(void)
{
	const uint16_t addr = indirect_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(a, operand);

	insn_trace("sbc  A,(X)    $%02x -= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0xa7 - SBC A with X-indexed direct-page */
static void insn_sbc_a_dx_ind(void)
{
	const uint16_t addr = direct_page_x_indirect();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(a, operand);

	insn_trace("sbc  A,(d-X)  $%02x -= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0xa8 - SBC immediate into A */
static void insn_sbc_a_imm(void)
{
	const uint8_t imm = immediate();
	const uint8_t result = alu_sbc(a, imm);

	insn_trace("sbc  A,#i     $%02x -= #$%02x -> $%02x [%s]",
		a, imm, result, psw().str);

	a = result;
}

/* 0xa9 - SBC A with X-indexed direct-page */
static void insn_sbc_dp_dp(void)
{
	const uint16_t src = direct_page();
	const uint16_t dst = direct_page();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_sbc(src_val, dst_val);

	insn_trace("sbc  d,d      $%02x - $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0xb4 - SBC A with X-indexed direct-page */
static void insn_sbc_a_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(a, operand);

	insn_trace("sbc  A,d+X    $%02x -= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0xb5 - SBC A with X-indexed absolute */
static void insn_sbc_a_absx(void)
{
	const uint16_t addr = absolute_x();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(a, operand);

	insn_trace("sbc  A,!a+X   $%02x -= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0xb6 - SBC A with Y-indexed direct-page */
static void insn_sbc_a_absy(void)
{
	const uint16_t addr = absolute_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(a, operand);

	insn_trace("sbc  A,!a+Y   $%02x -= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0xb7 - SBC A with Y-indexed direct-page */
static void insn_sbc_a_dpiy(void)
{
	const uint16_t addr = direct_page_indirect_y();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(a, operand);

	insn_trace("sbc  A,(d)-Y  $%02x -= $%02x -> $%02x ($%04x) [%s]",
		a, operand, result, addr, psw().str);

	a = result;
}

/* 0xb8 - SBC immediate into direct-page byte */
static void insn_sbc_dp_imm(void)
{
	const uint8_t imm = immediate();
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = alu_sbc(operand, imm);

	mem_store(addr, result);

	insn_trace("sbc  d,#i     $%02x -= #$%02x -> $%02x ($%04x) [%s]",
		operand, imm, result, addr, psw().str);
}

/* 0xb9 - SBC A with X-indexed direct-page */
static void insn_sbc_ix_iy(void)
{
	const uint16_t dst = indirect_x();
	const uint16_t src = indirect_y();
	const uint8_t src_val = mem_load(src);
	const uint8_t dst_val = mem_load(dst);
	const uint8_t result = alu_sbc(src_val, dst_val);

	insn_trace("sbc  (X),(Y)  $%02x - $%02x -> $%02x ($%04x $%04x) [%s]",
		dst_val, src_val, result, dst, src, psw().str);

	mem_store(dst, result);
}

/* 0x2f - bra - Branch */
static void insn_bra(void)
{
	const int8_t disp = relative();

	pc += disp;
	branch_taken();
	insn_trace("bra  rel      %d taken -> $%04x", disp, pc);
}

/* 0x90 - bcc - Branch if carry clear */
static void insn_bcc(void)
{
	const int8_t disp = relative();

	if (!carry) {
		pc += disp;
		branch_taken();
		insn_trace("bcc  rel      %d taken -> $%04x", disp, pc);
	} else {
		insn_trace("bcc  rel      %d not taken", disp);
	}
}

/* 0xb0 - bcs - Branch if carry set */
static void insn_bcs(void)
{
	const int8_t disp = relative();

	if (carry) {
		pc += disp;
		branch_taken();
		insn_trace("bcs  rel      %d taken -> $%04x", disp, pc);
	} else {
		insn_trace("bcs  rel      %d not taken", disp);
	}
}

/* 0xdb - Store Y in to x-indexed direct-page*/
static void insn_mov_dpx_y(void)
{
	const uint16_t addr = direct_page_x();

	insn_trace("mov  d+x,Y    $%02x ($%04x)", y, addr);

	mem_store(addr, y);
}

/* 0x1a Decrement 16 bit word at direct page */
static void insn_decw_dp(void)
{
	const uint16_t addr = direct_page();
	const uint16_t operand = mem_load_word(addr);
	const uint16_t result = operand - 1;

	set_zn16(result);
	insn_trace("decw d        $%04x-- -> $%04x ($%04x)",
			operand, result, addr);
	mem_store(addr, result);
}

/* 0x3a - Increment 16 bit word at direct page */
static void insn_incw_dp(void)
{
	const uint16_t addr = direct_page();
	const uint16_t operand = mem_load_word(addr);
	const uint16_t result = operand + 1;

	set_zn16(result);
	insn_trace("incw d        $%04x++ -> $%04x ($%04x)",
			operand, result, addr);
	mem_store(addr, result);
}

/* 0x5a - compare 16 bit word from direct page to YA */
static void insn_cmpw_ya_d(void)
{
	const uint16_t addr = direct_page();
	const uint16_t operand = mem_load_word(addr);
	const uint16_t ya = get_ya();
	const int32_t result = (int32_t)ya - (int32_t)operand;

	carry = result >= 0;
	set_zn16(result);

	insn_trace("cmpw YA,d     $%04x == $%04x ($%04x) [%s]",
		ya, operand, addr, psw().str);
}

/* 0x7a - add 16 bit word from direct page to YA */
static void insn_addw_ya_dp(void)
{
	const uint16_t addr = direct_page();
	const uint16_t operand = mem_load_word(addr);
	const uint16_t ya = get_ya();
	const uint16_t result = alu_addw(ya, operand);

	insn_trace("addw YA,d     $%04x + $%04x ($%04x) -> $%04x [%s]",
		ya, operand, addr, result, psw().str);

	set_ya(result);
}

/* 0x9a - subtract 16 bit word from direct page to YA */
static void insn_subw_ya_dp(void)
{
	const uint16_t addr = direct_page();
	const uint16_t operand = mem_load_word(addr);
	const uint16_t ya = get_ya();
	const uint16_t result = alu_subw(ya, operand);

	insn_trace("subw YA,d     $%04x - $%04x ($%04x) -> $%04x [%s]",
		ya, operand, addr, result, psw().str);

	set_ya(result);
}

/* 0xba - load 16 bit word from direct page into YA */
static void insn_movw_ya_dp(void)
{
	const uint16_t addr = direct_page();
	const uint16_t operand = mem_load_word(addr);

	set_ya(operand);

	set_zn16(operand);

	insn_trace("movw YA,d     YA <- #$%04x ($%04x) [%s]",
		operand, addr, psw().str);
}

/* 0xda - store YA into direct page */
static void insn_movw_dp_ya(void)
{
	const uint16_t addr = direct_page();
	const uint16_t operand = get_ya();

	mem_store_word(addr, operand);

	insn_trace("movw d,YA     $%04x ($%04x)",
		operand,
		addr);
}

/* 0x9e - divide YA by X and put quotient in A and remainder in Y */
static void insn_divw_ya_x(void)
{
	const uint16_t ya = get_ya();

	half_carry = (y & 0xf) >= (x & 0xf);
	overflow = y >= x;

	if (y < (x << 1)) {
		a = ya / x;
		y = ya % x;
	} else {
		a = 0xff - (ya - (x << 9)) / (0x100 - x);
		y =    x + (ya - (x << 9)) % (0x100 - x);
	}

	set_zn(a);

	insn_trace("divw YA,X     $%04x / $%02x -> $%02x r $%02x  [%s]",
		ya, x, a, y, psw().str);
}

/* 0xcf multiply A by Y and put result in YA */
static void insn_mul_ya(void)
{
	const uint16_t result = a * y;

	set_zn16(result);

	insn_trace("mul  YA       $%02x * $%02x -> $%04x [%s]",
		a, y, result, psw().str);

	set_ya(result);
}

/* 0x8f - store immediate into direct page */
static void insn_mov_dp_imm(void)
{
	const uint8_t imm = immediate();
	const uint16_t addr = direct_page();

	mem_store(addr, imm);

	insn_trace("mov  d,#i     #$%02x -> $%04x", imm, addr);
}

/* 0xfa - move direct page byte to another direct page byte */
static void insn_mov_dp_dp(void)
{
	const uint16_t src = direct_page();
	const uint16_t dst = direct_page();
	const uint8_t operand = mem_load(src);

	mem_store(dst, operand);

	insn_trace("mov  d,d      $%02x ($%04x) -> ($%04x)",
		operand, src, dst);
}

/* 0x10 - bpl - Branch if not negative */
static void insn_bpl(void)
{
	const int8_t disp = relative();

	if (!negative) {
		pc += disp;
		branch_taken();
		insn_trace("bpl  rel      %d taken -> $%04x", disp, pc);
	} else {
		insn_trace("bpl  rel      %d not taken", disp);
	}
}

/* 0x30 - bmi - Branch if negative */
static void insn_bmi(void)
{
	const int8_t disp = relative();

	if (negative) {
		pc += disp;
		branch_taken();
		insn_trace("bpl  rel      %d taken -> $%04x", disp, pc);
	} else {
		insn_trace("bpl  rel      %d not taken", disp);
	}
}

/* 0xd0 - bne - Branch if not equal */
static void insn_bne(void)
{
	const int8_t disp = relative();

	if (!zero) {
		pc += disp;
		branch_taken();
		insn_trace("bne  rel      %d taken -> $%04x", disp, pc);
	} else {
		insn_trace("bne  rel      %d not taken", disp);
	}
}

/* 0xf0 - beq - Branch if equal */
static void insn_beq(void)
{
	const int8_t disp = relative();

	if (zero) {
		pc += disp;
		branch_taken();
		insn_trace("beq  rel      %d taken -> $%04x", disp, pc);
	} else {
		insn_trace("beq  rel      %d not taken", disp);
	}
}

/* 0x6e - dbnz d,rel */
static void insn_dbnz_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const uint8_t result = operand - 1;
	const int8_t disp = relative();

	zero = !result;

	if (!zero) {
		pc += disp;
		branch_taken();
		insn_trace("dbnz d,rel    $%02x -> $%02x ($%04x) %d taken -> $%04x",
				operand, result, addr, disp, pc);
	} else {
		insn_trace("dbnz d,rel    $%02x -> $%02x ($%04x) not taken",
				operand, result, addr);
	}

	mem_store(addr, result);
}

/* 0xfe - dbnz Y,rel */
static void insn_dbnz_y(void)
{
	const uint8_t result = y - 1;
	const int8_t disp = relative();

	//zero = !result;

	if (result) {
		pc += disp;
		branch_taken();
		insn_trace("dbnz Y,rel    $%02x -> $%02x %d taken -> $%04x",
				y, result, disp, pc);
	} else {
		insn_trace("dbnz Y,rel    $%02x -> $%02x not taken",
				y, result);
	}

	y = result;
}

/* 0xc4 - store A register into direct page */
static void insn_mov_dp_a(void)
{
	const uint16_t addr = direct_page();

	mem_store(addr, a);

	insn_trace("mov  d,A      ($%04x) <- $%02x", addr, a);
}

/* 0xc5 - store A register to absolute address */
static void insn_mov_abs_a(void)
{
	const uint16_t addr = absolute();

	insn_trace("mov  !a,A     $%02x ($%04x)", a, addr);
	mem_store(addr, a);
}

/* 0xc6 store A to indirect X register */
static void insn_mov_ix_a(void)
{
	const uint16_t addr = indirect_x();

	insn_trace("mov  (X),A    $%02x ($%04x)", a, addr);
	mem_store(addr, a);
}

/* 0xc7 store A to X-indexed direct-page indirect */
static void insn_mov_dx_ind_a(void)
{
	const uint16_t addr = direct_page_x();

	insn_trace("mov  (d+X),A  $%02x ($%04x)", a, addr);
	mem_store(addr, a);
}

/* 0xc8 - compare X register with immediate value */
static void insn_cmp_x_imm(void)
{
	const uint8_t operand = immediate();

	alu_cmp(x, operand);

	insn_trace("cmp  X,#i     $%02x - #$%02x [%s]", a, operand, psw().str);
}

/* 0xc9 - store X register to absolute address */
static void insn_mov_abs_x(void)
{
	const uint16_t addr = absolute();

	insn_trace("mov  !a,X     $%02x ($%04x)", x, addr);
	mem_store(addr, x);
}

/* 0xe9 - load X register from absolute address */
static void insn_mov_x_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);

	x = operand;
	set_zn(operand);

	insn_trace("mov  X,!a     $%02x ($%04x)", x, addr);
}

/* 0xf8 - load direct page byte into X register */
static void insn_mov_x_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);

	x = operand;

	insn_trace("mov  X,d      $%02x ($%04x)", a, addr);
}

/* 0xcc - Store Y to absolute */
static void insn_mov_abs_y(void)
{
	const uint16_t addr = absolute();

	insn_trace("mov  !a,Y     $%02x ($%04x)", y, addr);
	mem_store(addr, y);
}

/* 0xec - load Y register from absolute address */
static void insn_mov_y_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	y = operand;

	insn_trace("mov  Y,!a     $%02x ($%04x)", y, addr);
}

/* 0xd4 - store A to x-indexed direct-page */
static void insn_mov_dpx_a(void)
{
	const uint16_t addr = direct_page_x();

	mem_store(addr, a);

	insn_trace("mov  d+X,A    $%02x ($%04x)", a, addr);
}

/* 0xd5 - store A to X-indexed absolute address */
static void insn_mov_absx_a(void)
{
	const uint16_t addr = absolute_x();

	mem_store(addr, a);

	insn_trace("mov  !a+X,A   $%02x ($%04x)", a, addr);
}

/* 0xd6 - store A to Y-indexed absolute address */
static void insn_mov_absy_a(void)
{
	const uint16_t addr = absolute_y();

	mem_store(addr, a);

	insn_trace("mov  !a+Y,A   $%02x ($%04x)", a, addr);
}

/* 0xd7 - store A to Y-indexed direct-page */
static void insn_mov_dpiy_a(void)
{
	const uint16_t addr = direct_page_indirect_y();

	mem_store(addr, a);

	insn_trace("mov  (d)+Y,A  $%02x ($%04x)", a, addr);
}

/* 0xe4 - load direct page byte into A register */
static void insn_mov_a_dp(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	a = operand;

	insn_trace("mov  A,d      $%02x <- ($%04x)", a, addr);
}

/* 0xe5 - mov A, abs */
static void insn_mov_a_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	a = operand;

	insn_trace("mov  A,!a     $%02x ($%04x) [%s]",
		operand, addr, psw().str);
}

/* 0xe6 load A from indirect X register */
static void insn_mov_a_ix(void)
{
	const uint16_t addr = indirect_x();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	a = operand;

	insn_trace("mov  A,(X)    $%02x ($%04x) [%s]",
		operand, addr, psw().str);
}

/* 0xe7 load A from X-indexed direct-page indirect */
static void insn_mov_a_dx_ind(void)
{
	const uint16_t addr = direct_page_x_indirect();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	a = operand;

	insn_trace("mov  A,(d+X)  $%02x ($%04x) [%s]",
		operand, addr, psw().str);
}

/* 0xe8 - store immediate value into A */
static void insn_mov_a_imm(void)
{
	const uint8_t operand = immediate();

	set_zn(operand);
	a = operand;

	insn_trace("mov  A,#i     #$%02x [%s]", operand, psw().str);
}

/* 0xf4 - move x-indexed direct page byte into A */
static void insn_mov_a_dpx(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);

	insn_trace("mov  A,d+X    $%02x -> $%02x ($%04x) [%s]",
		a, operand, addr, psw().str);

	a = operand;
}

/* 0xf5 - load abs + X into A */
static void insn_mov_a_absx(void)
{
	const uint16_t addr = absolute_x();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	a = operand;

	insn_trace("mov  A,!a+X   $%02x ($%04x) [%s]",
		operand, addr, psw().str);
}

/* 0xf6 - load abs + Y into A */
static void insn_mov_a_absy(void)
{
	const uint16_t addr = absolute_y();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	a = operand;

	insn_trace("mov  A,!a+Y   $%02x ($%04x) [%s]",
		operand, addr, psw().str);
}

/* 0xf7 - load A from Y-indexed direct-page */
static void insn_mov_a_dpiy(void)
{
	const uint16_t addr = direct_page_indirect_y();
	const uint8_t operand = mem_load(addr);

	set_zn(operand);
	a = operand;

	insn_trace("mov  A,(d)+Y  $%02x ($%04x) [%s]",
		a, addr, psw().str);
}

/* 0xae - pop A */
static void insn_pop_a(void)
{
	const uint8_t operand = pop_byte();

	a = operand;
	insn_trace("pop  A        $%02x", a);
}

/* 0x8e - pop PSW */
static void insn_pop_psw(void)
{
	const uint8_t operand = pop_byte();

	psw_decompose(operand);

	insn_trace("pop  PSW      $%02x", operand);
}

/* 0xce - pop X */
static void insn_pop_x(void)
{
	const uint8_t operand = pop_byte();

	x = operand;
	insn_trace("pop  X        $%02x", x);
}

/* 0xee - pop Y */
static void insn_pop_y(void)
{
	const uint8_t operand = pop_byte();

	y = operand;
	insn_trace("pop  Y        $%02x", y);
}

/* 0x0a - or a bit into the carry flag */
static void insn_or1(void)
{
	const bitaddr_t operand = bitaddr();
	const bool bit = bitaddr_load(operand);

	carry |= bit;

	insn_trace("or1  mb       ($%04x.%u) [%s]",
		operand.addr, operand.bit, psw().str);
}

/* 0x2a - or the complement of a bit into the carry flag */
static void insn_or1_not(void)
{
	const bitaddr_t operand = bitaddr();
	const bool bit = bitaddr_load(operand);

	carry |= !bit;

	insn_trace("or1  /mb      ($%04x.%u) [%s]",
		operand.addr, operand.bit, psw().str);
}

/* 0x4a - or a bit into the carry flag */
static void insn_and1(void)
{
	const bitaddr_t operand = bitaddr();
	const bool bit = bitaddr_load(operand);

	carry &= bit;

	insn_trace("and1 mb       ($%04x.%u) [%s]",
		operand.addr, operand.bit, psw().str);
}

/* 0x6a - and the complement of a bit into the carry flag */
static void insn_and1_not(void)
{
	const bitaddr_t operand = bitaddr();
	const bool bit = bitaddr_load(operand);

	carry &= !bit;

	insn_trace("and1 /mb      ($%04x.%u) [%s]",
		operand.addr, operand.bit, psw().str);
}

/* 0x8a - xor a bit with carry flag */
static void insn_eor1(void)
{
	const bitaddr_t operand = bitaddr();
	const bool bit = bitaddr_load(operand);

	carry ^= bit;

	insn_trace("eor1 mb       ($%04x.%u) [%s]",
		operand.addr, operand.bit, psw().str);
}

/* 0xaa - load a bit into the carry flag */
static void insn_mov1_load(void)
{
	const bitaddr_t operand = bitaddr();
	const bool bit = bitaddr_load(operand);

	carry = bit;

	insn_trace("mov1 C,mb     ($%04x.%u) [%s]",
		operand.addr, operand.bit, psw().str);
}

/* 0xca - store the carry flag into a bit */
static void insn_mov1_store(void)
{
	const bitaddr_t operand = bitaddr();

	bitaddr_store(operand, carry);

	insn_trace("mov1 mb,C     ($%04x.%u) [%s]",
		operand.addr, operand.bit, psw().str);
}

/* 0xea - toggle a bit in memory */
static void insn_not1(void)
{
	const bitaddr_t operand = bitaddr();
	const uint8_t bit = (1U << operand.bit);
	const uint8_t byte = mem_load(operand.addr);
	const uint8_t result = byte ^ bit;

	mem_store(operand.addr, result);

	insn_trace("not1 mb       %u ($%04x.%u)",
		(bool)(result & bit),
		operand.addr,
		operand.bit);
}

/* 0x0e - set flags from A into a byte in memory z/n flags set on byte & A */
static void insn_tset1_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t out = operand | a;
	const uint8_t result = a - operand;

	mem_store(addr, out);
	set_zn(result);

	insn_trace("tset mb       A=$%02x $%02x -> $%02x ($%04x) [%s]",
		a, operand, out, addr, psw().str);
}

/* 0x4e - unset flags from A from a byte in memory z/n flags set on byte & A */
static void insn_tclr1_abs(void)
{
	const uint16_t addr = absolute();
	const uint8_t operand = mem_load(addr);
	const uint8_t out = operand & ~a;
	const uint8_t result = a - operand;

	mem_store(addr, out);
	set_zn(result);

	insn_trace("tclr mb       A=$%02x $%02x -> $%02x ($%04x) [%s]",
		a, operand, out, addr, psw().str);
}

/* 0x2e - CBNE d,rel */
static void insn_cbne_dp_rel(void)
{
	const uint16_t addr = direct_page();
	const uint8_t operand = mem_load(addr);
	const int8_t disp = relative();

	if (operand != a) {
		pc += disp;
		branch_taken();
		insn_trace("cbne d,rel    $%02x != $%02x ($%04x) %d taken -> $%04x",
				operand, a, addr, disp, pc);
	} else {
		insn_trace("cbne d,rel    $%02x == $%02x ($%04x) not taken",
				operand, a, addr);
	}
}

/* 0xde - CBNE d+X,rel */
static void insn_cbne_dpx_rel(void)
{
	const uint16_t addr = direct_page_x();
	const uint8_t operand = mem_load(addr);
	const int8_t disp = relative();

	if (operand != a) {
		pc += disp;
		branch_taken();
		insn_trace("cbne d+X,rel  $%02x != $%02x ($%04x) %d taken -> $%04x",
				operand, a, addr, disp, pc);
	} else {
		insn_trace("cbne d+X,rel  $%02x -> $%02x ($%04x) not taken",
				operand, a, addr);
	}
}

typedef void (*insn_t)(void);

static const insn_t opcode_tbl[0x100] = {
	[0x00] = insn_nop,
	[0x01] = insn_tcall_0,
	[0x02] = insn_set_db_0,
	[0x03] = insn_bbs_db_0,
	[0x04] = insn_or_a_dp,
	[0x05] = insn_or_a_abs,
	[0x06] = insn_or_a_ix,
	[0x07] = insn_or_a_dx_ind,
	[0x08] = insn_or_a_imm,
	[0x09] = insn_or_dp_dp,
	[0x0a] = insn_or1,
	[0x0b] = insn_asl_dp,
	[0x0c] = insn_asl_abs,
	[0x0d] = insn_push_psw,
	[0x0e] = insn_tset1_abs,
	[0x0f] = NULL, /* brk */
	[0x10] = insn_bpl,
	[0x11] = insn_tcall_1,
	[0x12] = insn_clr_db_0,
	[0x13] = insn_bbc_db_0,
	[0x14] = insn_or_a_dpx,
	[0x15] = insn_or_a_absx,
	[0x16] = insn_or_a_absy,
	[0x17] = insn_or_a_dpiy,
	[0x18] = insn_or_dp_imm,
	[0x19] = insn_or_ix_iy,
	[0x1a] = insn_decw_dp,
	[0x1b] = insn_asl_dpx,
	[0x1c] = insn_asl_a,
	[0x1d] = insn_dec_x,
	[0x1e] = NULL, /* cmp X,abs */
	[0x1f] = insn_jmp_absx_ind,
	[0x20] = insn_clrp,
	[0x21] = insn_tcall_2,
	[0x22] = insn_set_db_1,
	[0x23] = insn_bbs_db_1,
	[0x24] = insn_and_a_dp,
	[0x25] = insn_and_a_abs,
	[0x26] = insn_and_a_ix,
	[0x27] = insn_and_a_dx_ind,
	[0x28] = insn_and_a_imm,
	[0x29] = insn_and_dp_dp,
	[0x2a] = insn_or1_not,
	[0x2b] = insn_rol_dp,
	[0x2c] = insn_rol_abs,
	[0x2d] = insn_push_a,
	[0x2e] = insn_cbne_dp_rel,
	[0x2f] = insn_bra,
	[0x30] = insn_bmi,
	[0x31] = insn_tcall_3,
	[0x32] = insn_clr_db_1,
	[0x33] = insn_bbc_db_1,
	[0x34] = insn_and_a_dpx,
	[0x35] = insn_and_a_absx,
	[0x36] = insn_and_a_absy,
	[0x37] = insn_and_a_dpiy,
	[0x38] = insn_and_dp_imm,
	[0x39] = insn_and_ix_iy,
	[0x3a] = insn_incw_dp,
	[0x3b] = insn_rol_dpx,
	[0x3c] = insn_rol_a,
	[0x3d] = insn_inc_x,
	[0x3e] = NULL, /* cmp X,dp */
	[0x3f] = insn_call_abs,
	[0x40] = insn_setp,
	[0x41] = insn_tcall_4,
	[0x42] = insn_set_db_2,
	[0x43] = insn_bbs_db_2,
	[0x44] = insn_eor_a_dp,
	[0x45] = insn_eor_a_abs,
	[0x46] = insn_eor_a_ix,
	[0x47] = insn_eor_a_dx_ind,
	[0x48] = insn_eor_a_imm,
	[0x49] = insn_eor_dp_dp,
	[0x4a] = insn_and1,
	[0x4b] = insn_lsr_dp,
	[0x4c] = insn_lsr_abs,
	[0x4d] = insn_push_x,
	[0x4e] = insn_tclr1_abs,
	[0x4f] = NULL, /* pcall */
	[0x50] = NULL, /* bvc */
	[0x51] = insn_tcall_5,
	[0x52] = insn_clr_db_2,
	[0x53] = insn_bbc_db_2,
	[0x54] = insn_eor_a_dpx,
	[0x55] = insn_eor_a_absx,
	[0x56] = insn_eor_a_absy,
	[0x57] = insn_eor_a_dpiy,
	[0x58] = insn_eor_dp_imm,
	[0x59] = insn_eor_ix_iy,
	[0x5a] = insn_cmpw_ya_d,
	[0x5b] = insn_lsr_dpx,
	[0x5c] = insn_lsr_a,
	[0x5d] = insn_mov_x_a,
	[0x5e] = NULL, /* cmp Y,abs */
	[0x5f] = insn_jmp_abs,
	[0x60] = insn_clrc,
	[0x61] = insn_tcall_6,
	[0x62] = insn_set_db_3,
	[0x63] = insn_bbs_db_3,
	[0x64] = insn_cmp_a_dp,
	[0x65] = insn_cmp_a_abs,
	[0x66] = insn_cmp_a_ix,
	[0x67] = insn_cmp_a_dx_ind,
	[0x68] = insn_cmp_a_imm,
	[0x69] = insn_cmp_dp_dp,
	[0x6a] = insn_and1_not,
	[0x6b] = insn_ror_dp,
	[0x6c] = insn_ror_abs,
	[0x6d] = insn_push_y,
	[0x6e] = insn_dbnz_dp,
	[0x6f] = insn_ret,
	[0x70] = NULL, /* bvs */
	[0x71] = insn_tcall_7,
	[0x72] = insn_clr_db_3,
	[0x73] = insn_bbc_db_3,
	[0x74] = insn_cmp_a_dpx,
	[0x75] = insn_cmp_a_absx,
	[0x76] = insn_cmp_a_absy,
	[0x77] = insn_cmp_a_dpiy,
	[0x78] = insn_cmp_dp_imm,
	[0x79] = insn_cmp_ix_iy,
	[0x7a] = insn_addw_ya_dp,
	[0x7b] = insn_ror_dpx,
	[0x7c] = insn_ror_a,
	[0x7d] = insn_mov_a_x,
	[0x7e] = insn_cmp_y_dp,
	[0x7f] = NULL, /* reti */
	[0x80] = insn_setc,
	[0x81] = insn_tcall_8,
	[0x82] = insn_set_db_4,
	[0x83] = insn_bbs_db_4,
	[0x84] = insn_adc_a_dp,
	[0x85] = insn_adc_a_abs,
	[0x86] = insn_adc_a_ix,
	[0x87] = insn_adc_a_dx_ind,
	[0x88] = insn_adc_a_imm,
	[0x89] = insn_adc_dp_dp,
	[0x8a] = insn_eor1,
	[0x8b] = insn_dec_dp,
	[0x8c] = insn_dec_abs,
	[0x8d] = insn_mov_y_imm,
	[0x8e] = insn_pop_psw,
	[0x8f] = insn_mov_dp_imm,
	[0x90] = insn_bcc,
	[0x91] = insn_tcall_9,
	[0x92] = insn_clr_db_4,
	[0x93] = insn_bbc_db_4,
	[0x94] = insn_adc_a_dpx,
	[0x95] = insn_adc_a_absx,
	[0x96] = insn_adc_a_absy,
	[0x97] = insn_adc_a_dpiy,
	[0x98] = insn_adc_dp_imm,
	[0x99] = insn_adc_ix_iy,
	[0x9a] = insn_subw_ya_dp,
	[0x9b] = insn_dec_dpx,
	[0x9c] = insn_dec_a,
	[0x9d] = NULL, /* mov X, SP */
	[0x9e] = insn_divw_ya_x,
	[0x9f] = insn_xcn,
	[0xa0] = insn_ei,
	[0xa1] = insn_tcall_10,
	[0xa2] = insn_set_db_5,
	[0xa3] = insn_bbs_db_5,
	[0xa4] = insn_sbc_a_dp,
	[0xa5] = insn_sbc_a_abs,
	[0xa6] = insn_sbc_a_ix,
	[0xa7] = insn_sbc_a_dx_ind,
	[0xa8] = insn_sbc_a_imm,
	[0xa9] = insn_sbc_dp_dp,
	[0xaa] = insn_mov1_load,
	[0xab] = insn_inc_dp,
	[0xac] = insn_inc_abs,
	[0xad] = insn_cmp_y_imm,
	[0xae] = insn_pop_a,
	[0xaf] = NULL, /* mov (X)+, A */
	[0xb0] = insn_bcs,
	[0xb1] = insn_tcall_11,
	[0xb2] = insn_clr_db_5,
	[0xb3] = insn_bbc_db_5,
	[0xb4] = insn_sbc_a_dpx,
	[0xb5] = insn_sbc_a_absx,
	[0xb6] = insn_sbc_a_absy,
	[0xb7] = insn_sbc_a_dpiy,
	[0xb8] = insn_sbc_dp_imm,
	[0xb9] = insn_sbc_ix_iy,
	[0xba] = insn_movw_ya_dp,
	[0xbb] = insn_inc_dpx,
	[0xbc] = insn_inc_a,
	[0xbd] = NULL, /* mov SP,X */
	[0xbe] = NULL, /* DAS */
	[0xbf] = NULL, /* mov A,(X)+ */
	[0xc0] = insn_di,
	[0xc1] = insn_tcall_12,
	[0xc2] = insn_set_db_6,
	[0xc3] = insn_bbs_db_6,
	[0xc4] = insn_mov_dp_a,
	[0xc5] = insn_mov_abs_a,
	[0xc6] = insn_mov_ix_a,
	[0xc7] = insn_mov_dx_ind_a,
	[0xc8] = insn_cmp_x_imm,
	[0xc9] = insn_mov_abs_x,
	[0xca] = insn_mov1_store,
	[0xcb] = insn_mov_dp_y,
	[0xcc] = insn_mov_abs_y,
	[0xcd] = insn_mov_x_imm,
	[0xce] = insn_pop_x,
	[0xcf] = insn_mul_ya,
	[0xd0] = insn_bne,
	[0xd1] = insn_tcall_13,
	[0xd2] = insn_clr_db_6,
	[0xd3] = insn_bbc_db_6,
	[0xd4] = insn_mov_dpx_a,
	[0xd5] = insn_mov_absx_a,
	[0xd6] = insn_mov_absy_a,
	[0xd7] = insn_mov_dpiy_a,
	[0xd8] = insn_mov_dp_x,
	[0xd9] = NULL, /* mov dp+Y,X */
	[0xda] = insn_movw_dp_ya,
	[0xdb] = insn_mov_dpx_y,
	[0xdc] = insn_dec_y,
	[0xdd] = insn_mov_a_y,
	[0xde] = insn_cbne_dpx_rel,
	[0xdf] = NULL, /* DAA */
	[0xe0] = insn_clrv,
	[0xe1] = insn_tcall_14,
	[0xe2] = insn_set_db_7,
	[0xe3] = insn_bbs_db_7,
	[0xe4] = insn_mov_a_dp,
	[0xe5] = insn_mov_a_abs,
	[0xe6] = insn_mov_a_ix,
	[0xe7] = insn_mov_a_dx_ind,
	[0xe8] = insn_mov_a_imm,
	[0xe9] = insn_mov_x_abs,
	[0xea] = insn_not1,
	[0xeb] = insn_mov_y_dp,
	[0xec] = insn_mov_y_abs,
	[0xed] = insn_notc,
	[0xee] = insn_pop_y,
	[0xef] = NULL, /* sleep */
	[0xf0] = insn_beq,
	[0xf1] = insn_tcall_15,
	[0xf2] = insn_clr_db_7,
	[0xf3] = insn_bbc_db_7,
	[0xf4] = insn_mov_a_dpx,
	[0xf5] = insn_mov_a_absx,
	[0xf6] = insn_mov_a_absy,
	[0xf7] = insn_mov_a_dpiy,
	[0xf8] = insn_mov_x_dp,
	[0xf9] = NULL, /* mov X,dp+Y */
	[0xfa] = insn_mov_dp_dp,
	[0xfb] = insn_mov_y_dpx,
	[0xfc] = insn_inc_y,
	[0xfd] = insn_mov_y_a,
	[0xfe] = insn_dbnz_y,
	[0xff] = NULL, /* halt */
};

static unsigned long cycs;

__attribute__((destructor))
static void dtor(void)
{
	printf("%lu cpu cycles\n", cycs);
}

__attribute__((hot,noinline))
void spc700_run_forever(void)
{
	unsigned int cycle = 0;

	while (true) {
		const uint16_t cur_pc = pc;
		const uint8_t opcode = fetch_insn();
		const insn_t cb = opcode_tbl[opcode];

		if (unlikely(cb == NULL)) {
			say(INFO, "halt: $%04x opcode $%02x", cur_pc, opcode);
			return;
		}

#ifdef TRACE_FOR_COMPARISON
		printf("%04x %02x %02x %02x %02x %02x\n",
			cur_pc, opcode, x, y, a, psw_compose());
#endif
		cycs++;
		(*cb)();

		/* TODO: average 3.9 cycles per instruction, conveniently a common
		 * divisor of 0x20 which means we can do the fast check below.
		 */
		cycle += 4;

		if ((cycle & 0xf) == 0) {
			_apu_update_clocks(cycle);
		}
	}
}
