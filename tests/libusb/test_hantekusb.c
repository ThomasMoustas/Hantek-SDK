/*
 * Tests of HantekUSB without a scope: argument checks, firmware lookup, the
 * "no scope" path, and block reads / streaming / ring-buffer overflow in
 * simulation mode. In test-pattern mode (2) every sample is a known function
 * of its stream index, so any error in the index or gap bookkeeping shows.
 * Built and run by run_libusb_tests.sh (Linux .so and Windows DLL under Wine).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../libusb/HantekUSB.h"

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}
#endif

static int failures;

#define CHECK(cond) do { \
		if (cond) printf("ok    %s\n", #cond); \
		else { printf("FAIL  %s  (line %d) %s\n", #cond, __LINE__, huLastError()); failures++; } \
	} while (0)

static unsigned char pat1(double idx) { long long i = (long long)idx; return (unsigned char)(i * 7 + 3); }
static unsigned char pat2(double idx) { long long i = (long long)idx; return (unsigned char)((i >> 8) + (i >> 16) * 31); }

static int pattern_ok(const unsigned char *a, const unsigned char *b, int n, double first)
{
	int k;

	for (k = 0; k < n; k++) {
		if (a[k] != pat1(first + k) || b[k] != pat2(first + k)) {
			printf("      mismatch at stream index %.0f\n", first + k);
			return 0;
		}
	}
	return 1;
}

#define CHUNK 100000
static unsigned char c1[CHUNK], c2[CHUNK];
static unsigned char b1[400000], b2[400000];

int main(int argc, char **argv)
{
	const char *fwdir = argc > 1 ? argv[1] : ".";
	double st[HU_STATUS_LEN], first, next, total;
	int r, n, k, contiguous, pattern, min1, max1, min2, max2;

	printf("-- arguments\n");
	CHECK(huSetGain(2, 1) == HU_ERR_ARG);
	CHECK(huSetGain(0, 3) == HU_ERR_ARG);
	CHECK(huSetSampleRate(1.23e6) == HU_ERR_ARG);
	CHECK(huSetSampleRate(1e6) == 0);
	CHECK(huGetSampleRate() == 1e6);
	CHECK(huSetSimulation(3) == HU_ERR_ARG);
	CHECK(startStreaming(0) == HU_ERR_NOT_OPEN);
	CHECK(huReadBlock(b1, b2, 10) == HU_ERR_NOT_OPEN);
	CHECK(getQueueStatus(NULL) == HU_ERR_ARG);

	printf("-- firmware files\n");
	CHECK(huSetFirmwareDir(NULL) == 0);          /* folder of the library */
	CHECK(huCheckFirmware(1) == 16312);
	CHECK(huSetFirmwareDir(fwdir) == 0);
	CHECK(huCheckFirmware(1) == 16312);
	CHECK(huCheckFirmware(2) == 16312);
	CHECK(huCheckFirmware(3) == HU_ERR_ARG);
	CHECK(huSetFirmwareDir("no-such-folder") == 0);
	CHECK(huCheckFirmware(1) == HU_ERR_FIRMWARE && strstr(huLastError(), "no-such-folder"));
	huSetFirmwareDir(NULL);

	printf("-- real hardware, none connected\n");
	r = huOpen();
	printf("      huOpen() = %d: %s\n", r, huLastError());
	CHECK(r == HU_ERR_NOT_FOUND || r == HU_ERR_USB);

	printf("-- simulation, block read\n");
	CHECK(huSetSimulation(2) == 0);
	CHECK(huOpen() == 1);
	CHECK(huSetSimulation(0) == HU_ERR_BUSY);     /* not while open */
	CHECK(huSetGain(0, 10) == 0);
	CHECK(huReadBlock(b1, b2, 400000) == 400000);
	CHECK(pattern_ok(b1, b2, 400000, 0));

	printf("-- simulation, gapless streaming for 1.2 s\n");
	CHECK(startStreaming(0) == 0);
	CHECK(startStreaming(0) == HU_ERR_BUSY);
	CHECK(huSetSampleRate(2e6) == HU_ERR_BUSY);
	CHECK(huSetGain(1, 2) == HU_ERR_BUSY);
	CHECK(huReadBlock(b1, b2, 10) == HU_ERR_BUSY);
	next = 0;
	total = 0;
	contiguous = pattern = 1;
	for (k = 0; k < 60; k++) {
		sleep_ms(20);
		while ((n = getStreamData(c1, c2, CHUNK, &first)) > 0) {
			contiguous &= first == next;
			pattern &= pattern_ok(c1, c2, n, first);
			next = first + n;
			total += n;
		}
		if (n < 0)
			break;
	}
	CHECK(n == 0);
	CHECK(stopStreaming() == 0);
	while ((n = getStreamData(c1, c2, CHUNK, &first)) > 0) { /* left in the buffer */
		contiguous &= first == next;
		pattern &= pattern_ok(c1, c2, n, first);
		next = first + n;
		total += n;
	}
	CHECK(contiguous);
	CHECK(pattern);
	CHECK(getQueueStatus(st) == 0);
	printf("      read %.0f samples in %.3f s, measured rate %.0f Sa/s\n", total, st[6], st[8]);
	CHECK(st[0] == 0);
	CHECK(st[4] == 0 && st[5] == 0);            /* nothing dropped */
	CHECK(st[3] == total);                      /* everything delivered was read */
	CHECK(st[8] > 0.97e6 && st[8] < 1.03e6);
	CHECK(total > 1.1e6);

	printf("-- simulation, ring buffer overflow\n");
	CHECK(startStreaming(65536) == 0);
	sleep_ms(300);                              /* ~300000 samples into a 65536 ring */
	CHECK(stopStreaming() == 0);
	CHECK(getQueueStatus(st) == 0);
	CHECK(st[2] == 65536 && st[4] > 0 && st[5] >= 1);
	n = getStreamData(c1, c2, CHUNK, &first);
	CHECK(n == 65536 && first == 0);            /* stops at the gap */
	CHECK(pattern_ok(c1, c2, n, first));
	CHECK(getQueueStatus(st) == 0 && st[9] == 65536 + st[4]); /* next index jumps the gap */
	n = getStreamData(c1, c2, CHUNK, &first);
	CHECK(n == 0);                              /* newest samples were the dropped ones */
	CHECK(st[3] == 65536 + st[4]);

	printf("-- simulation, gap in the middle of a stream\n");
	CHECK(startStreaming(65536) == 0);
	sleep_ms(30);
	total = 0;
	while ((n = getStreamData(c1, c2, CHUNK, &first)) > 0) total += n;
	sleep_ms(300);                              /* overflow now */
	next = total;
	contiguous = 1;
	pattern = 1;
	for (k = 0; k < 20; k++) {
		sleep_ms(10);
		while ((n = getStreamData(c1, c2, CHUNK, &first)) > 0) {
			if (first != next)
				contiguous = 0;                 /* expected once: the gap */
			pattern &= pattern_ok(c1, c2, n, first);
			next = first + n;
			total += n;
		}
	}
	CHECK(stopStreaming() == 0);
	while ((n = getStreamData(c1, c2, CHUNK, &first)) > 0) {
		pattern &= pattern_ok(c1, c2, n, first);
		total += n;
	}
	CHECK(getQueueStatus(st) == 0);
	CHECK(!contiguous && pattern);
	CHECK(total + st[4] == st[3]);              /* read + dropped = delivered */
	huClose();
	CHECK(getStreamData(c1, c2, CHUNK, &first) == HU_ERR_NOT_OPEN);

	printf("-- simulation, signals\n");
	CHECK(huSetSimulation(1) == 0);
	CHECK(huOpen() == 1);
	CHECK(huSetGain(0, 1) == 0 && huSetGain(1, 1) == 0);
	CHECK(huReadBlock(b1, b2, 100000) == 100000);
	min1 = max1 = b1[0];
	min2 = max2 = b2[0];
	for (k = 0; k < 100000; k++) {
		if (b1[k] < min1) min1 = b1[k];
		if (b1[k] > max1) max1 = b1[k];
		if (b2[k] < min2) min2 = b2[k];
		if (b2[k] > max2) max2 = b2[k];
	}
	printf("      CH1 %d..%d, CH2 %d..%d counts\n", min1, max1, min2, max2);
	CHECK(min1 >= 126 && min1 <= 129 && max1 >= 208 && max1 <= 211); /* 0 / 3.3 V */
	CHECK(min2 >= 101 && max2 <= 155);                                /* +-1 V sine */
	huClose();

	printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL HANTEKUSB TESTS PASSED\n", failures);
	return failures ? 1 : 0;
}
