/* Host test: settings written by setenv survive a re-init from CMOS, a
 * corrupted block is detected, and nothing outside 0xa0-0xff is touched. */
#include "nvram.h"
#include "blockdev.h"
#include "platform.h"
#include "console.h"

u8 test_cmos[256];
struct blockdev *bdevs[MAX_BDEV];
int nbdevs;
struct sysinfo sysinfo;
void console_write(const char *s, size_t n) { (void)s; (void)n; }
void console_putc(char c) { (void)c; }

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; report(#c, __LINE__); } } while (0)
void report(const char *what, int line);

int test_main(void)
{
	for (int i = 0; i < 256; i++) test_cmos[i] = 0x5a;	/* "garbage" incl. coreboot's area */
	nv_init();
	CHECK(nv_was_reset());
	nv_set("diag-switch?", "false");
	nv_set("boot-device", "cdrom disk net");
	nv_set("boot-file", "/platform/i86pc/kernel/unix");
	nv_set("oem-banner?", "true");
	nv_set("oem-banner", "SPARCstation 5");
	nv_set("boot-entry", "2");
	for (int i = 0; i < 0xa0; i++) CHECK(test_cmos[i] == 0x5a);

	nv_init();	/* "reboot" */
	CHECK(!nv_was_reset());
	CHECK(!nv_get_bool("diag-switch?"));
	CHECK(nv_get_bool("auto-boot?"));
	CHECK(!strcmp(nv_get("boot-device"), "cdrom disk net"));
	CHECK(!strcmp(nv_get("boot-file"), "/platform/i86pc/kernel/unix"));
	CHECK(nv_get_bool("oem-banner?"));
	CHECK(!strcmp(nv_get("oem-banner"), "SPARCstation 5"));
	CHECK(nv_get_int("boot-entry") == 2);

	test_cmos[0xb0] ^= 0xff;	/* corrupt */
	nv_init();
	CHECK(nv_was_reset());
	CHECK(nv_get_bool("diag-switch?") == false && !strcmp(nv_get("boot-device"), "disk cdrom"));
	return fails;
}
