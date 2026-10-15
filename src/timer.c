/* TSC-based timekeeping, calibrated against PIT channel 2 (channel 0 is
 * left alone so a resident BIOS keeps its tick). */
#include "lib.h"

static u32 tsc_khz = 1000000;

void timer_init(void)
{
	/* Gate channel 2 on, speaker off; mode 0, count 11932 (~10 ms). */
	outb(0x61, (inb(0x61) & ~0x02) | 0x01);
	outb(0x43, 0xb0);
	outb(0x42, 11932 & 0xff);
	outb(0x42, 11932 >> 8);
	u64 t0 = rdtsc();
	u32 spins = 0;
	while (!(inb(0x61) & 0x20) && ++spins < 50000000)
		;
	u64 t1 = rdtsc();
	if (spins >= 50000000 || t1 <= t0) return;
	tsc_khz = (u32)((t1 - t0) / 10);
	if (!tsc_khz) tsc_khz = 1000000;
}

u32 cpu_mhz(void) { return tsc_khz / 1000; }
u64 timer_us(void) { return rdtsc() * 1000 / tsc_khz; }

void udelay(u32 us)
{
	u64 end = rdtsc() + (u64)us * tsc_khz / 1000;
	while (rdtsc() < end) __asm__ volatile("pause");
}

void mdelay(u32 ms) { while (ms--) udelay(1000); }
