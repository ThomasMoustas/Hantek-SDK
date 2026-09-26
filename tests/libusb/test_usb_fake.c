/*
 * Runs HantekUSB's real USB code (firmware upload, re-enumeration, vendor
 * requests, streaming with queued bulk transfers, stop, unplug) against
 * fake_libusb.c, a model of the 6022BL. Linux only; see run_libusb_tests.sh.
 */
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../libusb/HantekUSB.h"
#include "fake_libusb.h"

static int failures;

#define CHECK(cond) do { \
		if (cond) printf("ok    %s\n", #cond); \
		else { printf("FAIL  %s  (line %d) %s\n", #cond, __LINE__, huLastError()); failures++; } \
	} while (0)

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static unsigned char pat1(double idx) { long long i = (long long)idx; return (unsigned char)(i * 7 + 3); }
static unsigned char pat2(double idx) { long long i = (long long)idx; return (unsigned char)((i >> 8) + (i >> 16) * 31); }

static int pattern_ok(const unsigned char *a, const unsigned char *b, int n, double first)
{
	int k;

	for (k = 0; k < n; k++) {
		if (a[k] != pat1(first + k) || b[k] != pat2(first + k)) {
			printf("      mismatch at stream index %.0f: %u %u\n", first + k, a[k], b[k]);
			return 0;
		}
	}
	return 1;
}

static int file_equals(const char *path, const unsigned char *data, size_t len)
{
	static unsigned char buf[0x4001];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	return n == len && memcmp(buf, data, len) == 0;
}

/* Streams for `ms` milliseconds, reading every 10 ms; checks the pattern and
 * that the indices are contiguous. Returns the number of samples read. */
static double stream_for(unsigned ms, int *contiguous, int *pattern)
{
	static unsigned char c1[1 << 20], c2[1 << 20];
	double first, next = 0, total = 0;
	unsigned t;
	int n;

	*contiguous = *pattern = 1;
	for (t = 0; t < ms; t += 10) {
		sleep_ms(10);
		while ((n = getStreamData(c1, c2, 1 << 20, &first)) > 0) {
			*contiguous &= first == next;
			*pattern &= pattern_ok(c1, c2, n, first);
			next = first + n;
			total += n;
		}
		if (n < 0)
			break;
	}
	stopStreaming();
	while ((n = getStreamData(c1, c2, 1 << 20, &first)) > 0) {
		*contiguous &= first == next;
		*pattern &= pattern_ok(c1, c2, n, first);
		next = first + n;
		total += n;
	}
	return total;
}

static unsigned char b1[2000000], b2[2000000];

int main(int argc, char **argv)
{
	const char *fwdir = argc > 1 ? argv[1] : ".";
	char fwpath[1200];
	const unsigned char *ram;
	size_t ramlen;
	double st[HU_STATUS_LEN], total;
	int g1, g2, rate, ch, sampling, contiguous, pattern, r;

	snprintf(fwpath, sizeof(fwpath), "%s/fx2lafw-hantek-6022bl.fw", fwdir);
	huSetFirmwareDir(fwdir);

	printf("-- open: firmware upload and re-enumeration\n");
	fake_plug_cold();
	CHECK(huOpen() == 1);
	ram = fake_ram(&ramlen);
	CHECK(fake_uploads() == 1);
	CHECK(file_equals(fwpath, ram, ramlen));   /* image written byte for byte */
	fake_scope_state(&g1, &g2, &rate, &ch, &sampling);
	CHECK(g1 == 1 && g2 == 1 && rate == 1 && ch == 2 && !sampling);

	printf("-- settings reach the scope\n");
	CHECK(huSetGain(0, 10) == 0 && huSetGain(1, 5) == 0);
	CHECK(huSetSampleRate(8e6) == 0);
	fake_scope_state(&g1, &g2, &rate, &ch, &sampling);
	CHECK(g1 == 10 && g2 == 5 && rate == 8);

	printf("-- block read at 8 MSa/s\n");
	CHECK(huReadBlock(b1, b2, 2000000) == 2000000);
	CHECK(pattern_ok(b1, b2, 2000000, 0));    /* start-up garbage flushed exactly */
	fake_scope_state(&g1, &g2, &rate, &ch, &sampling);
	CHECK(!sampling);
	CHECK(fake_lost_samples() == 0);

	printf("-- streaming 1.5 s at 8 MSa/s\n");
	CHECK(startStreaming(0) == 0);
	total = stream_for(1500, &contiguous, &pattern);
	CHECK(getQueueStatus(st) == 0);
	printf("      read %.0f samples, measured %.0f Sa/s, %.0f bytes per transfer\n",
		total, st[8], st[11]);
	CHECK(contiguous && pattern);
	CHECK(st[0] == 0 && st[4] == 0);
	CHECK(st[3] == total);
	CHECK(st[8] > 0.97 * 8e6 && st[8] < 1.03 * 8e6);
	CHECK(fake_lost_samples() == 0);
	fake_scope_state(&g1, &g2, &rate, &ch, &sampling);
	CHECK(!sampling);

	printf("-- streaming 0.5 s at 100 kSa/s\n");
	CHECK(huSetSampleRate(100e3) == 0);
	CHECK(startStreaming(0) == 0);
	total = stream_for(500, &contiguous, &pattern);
	printf("      read %.0f samples\n", total);
	CHECK(contiguous && pattern && total > 40000);

	printf("-- unplug while streaming\n");
	CHECK(huSetSampleRate(1e6) == 0);
	CHECK(startStreaming(0) == 0);
	sleep_ms(200);
	fake_unplug();
	sleep_ms(50);
	while ((r = getStreamData(b1, b2, 2000000, &total)) > 0)
		;
	CHECK(r == HU_ERR_STREAM && strstr(huLastError(), "disconnected"));
	CHECK(getQueueStatus(st) == 0 && st[0] == -1);
	CHECK(stopStreaming() == 0);             /* returns, does not hang */
	huClose();

	printf("-- firmware already running: no second upload\n");
	fake_plug_cold();
	CHECK(huOpen() == 1);
	CHECK(fake_uploads() == 2);
	huClose();
	CHECK(huOpen() == 1);                    /* now 1D50:608E is on the bus */
	CHECK(fake_uploads() == 2);
	huClose();

	printf("-- errors\n");
	fake_plug_cold();
	huSetFirmwareDir("no-such-folder");
	CHECK(huOpen() == HU_ERR_FIRMWARE);
	huSetFirmwareDir(fwdir);
	fake_set_open_error(LIBUSB_ERROR_NOT_SUPPORTED); /* Windows without WinUSB */
	r = huOpen();
	CHECK(r == HU_ERR_NOT_FOUND && strstr(huLastError(), "Zadig"));
	printf("      %s\n", huLastError());
	fake_set_open_error(0);
	fake_unplug();
	CHECK(huOpen() == HU_ERR_NOT_FOUND);

	printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL FAKE-USB TESTS PASSED\n", failures);
	return failures ? 1 : 0;
}
