/*
 * HantekUSB - native 64-bit access to the Hantek 6022BL / 6022BE through
 * libusb and the open-source sigrok fx2lafw firmware. See HantekUSB.h.
 *
 * Protocol (from sigrok-firmware-fx2lafw 0.1.7 include/scope.inc and the
 * libsigrok 0.5.2 hantek-6xxx driver):
 *  - Power-on state: USB 04B4:602A (BL) / 04B4:6022 (BE); 04B5:xxxx if the
 *    Hantek driver already loaded its own firmware. The firmware is uploaded
 *    to RAM with the FX2 boot-loader request 0xA0 (CPU held in reset through
 *    CPUCS 0xE600) and the scope re-enumerates as 1D50:608E with bcdDevice
 *    0x0003 (BL) or 0x0001 (BE).
 *  - Vendor OUT requests with one data byte: 0xE0/0xE1 gain CH1/CH2 (1, 2, 5,
 *    10), 0xE2 sample rate code, 0xE3 = 1 start, 0xE4 number of channels.
 *    Every vendor request first stops sampling, so settings can only change
 *    while stopped.
 *  - Samples stream on bulk endpoint 0x86 (interface 0, alt setting 0) as
 *    CH1, CH2 byte pairs, as long as sampling runs. The first bytes after a
 *    start are discarded (libsigrok and the Hantek SDK do the same).
 */
#ifndef _WIN32
#define _GNU_SOURCE /* dladdr */
#endif
#include "HantekUSB.h"

#include <libusb.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------------
 * Platform: threads, lock, sleep, monotonic clock, folder of this library
 * ------------------------------------------------------------------------ */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HANDLE hu_thread_t;
typedef CRITICAL_SECTION hu_mutex_t;
#define THREAD_RET DWORD WINAPI
#define THREAD_RETURN return 0
typedef LPTHREAD_START_ROUTINE hu_thread_fn;

static int thread_start(hu_thread_t *t, hu_thread_fn fn)
{
	*t = CreateThread(NULL, 0, fn, NULL, 0, NULL);
	return *t ? 0 : -1;
}

static void thread_join(hu_thread_t t)
{
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}

static void mutex_init(hu_mutex_t *m) { InitializeCriticalSection(m); }
static void mutex_lock(hu_mutex_t *m) { EnterCriticalSection(m); }
static void mutex_unlock(hu_mutex_t *m) { LeaveCriticalSection(m); }
static void sleep_ms(unsigned ms) { Sleep(ms); }

static double now_s(void)
{
	LARGE_INTEGER f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

typedef pthread_t hu_thread_t;
typedef pthread_mutex_t hu_mutex_t;
#define THREAD_RET void *
#define THREAD_RETURN return NULL
typedef void *(*hu_thread_fn)(void *);

static int thread_start(hu_thread_t *t, hu_thread_fn fn)
{
	return pthread_create(t, NULL, fn, NULL) == 0 ? 0 : -1;
}

static void thread_join(hu_thread_t t) { pthread_join(t, NULL); }
static void mutex_init(hu_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void mutex_lock(hu_mutex_t *m) { pthread_mutex_lock(m); }
static void mutex_unlock(hu_mutex_t *m) { pthread_mutex_unlock(m); }

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

/* ------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------ */
#define FW_VID          0x1d50  /* after the fx2lafw firmware is running */
#define FW_PID          0x608e
#define EP_IN           0x86
#define USB_IFACE       0
#define REQ_FW_LOAD     0xa0    /* FX2 boot loader: write internal RAM */
#define FX2_CPUCS       0xe600
#define FW_CHUNK        4096
#define FW_MAX_SIZE     0x4000  /* FX2LP internal program RAM */
#define REQ_GAIN_CH1    0xe0
#define REQ_GAIN_CH2    0xe1
#define REQ_SAMPLERATE  0xe2
#define REQ_START       0xe3
#define REQ_CHANNELS    0xe4
#define CTRL_TIMEOUT_MS 1000
#define RENUM_WAIT_S    10.0    /* re-enumeration, incl. Windows driver binding */
#define FLUSH_BYTES     2048    /* start-up bytes to discard (1024 samples) */
#define NUM_XFERS       16      /* bulk transfers kept in flight */
#define XFER_MIN        (16 * 1024)
#define XFER_MAX        (1024 * 1024)
#define MAX_GAPS        4096
#define RING_MIN        65536
#define RING_MAX        (512u * 1024 * 1024)
#define RING_DEFAULT_S  10.0
#define NO_DATA_S       3.0

struct profile {
	uint16_t vid, pid;      /* power-on USB ID */
	uint16_t fw_bcd;        /* bcdDevice once the fx2lafw firmware runs */
	int model;              /* 1 = 6022BL, 2 = 6022BE */
	const char *name;
	const char *firmware;
};

static const struct profile profiles[] = {
	{ 0x04b4, 0x602a, 0x0003, 1, "Hantek 6022BL", "fx2lafw-hantek-6022bl.fw" },
	{ 0x04b5, 0x602a, 0x0003, 1, "Hantek 6022BL", "fx2lafw-hantek-6022bl.fw" },
	{ 0x04b4, 0x6022, 0x0001, 2, "Hantek 6022BE", "fx2lafw-hantek-6022be.fw" },
	{ 0x04b5, 0x6022, 0x0001, 2, "Hantek 6022BE", "fx2lafw-hantek-6022be.fw" },
};
#define NUM_PROFILES (sizeof(profiles) / sizeof(profiles[0]))

static const struct {
	double rate;
	unsigned char code;     /* value for REQ_SAMPLERATE */
} samplerates[] = {
	{ 48e6, 48 }, { 30e6, 30 }, { 24e6, 24 }, { 16e6, 16 }, { 12e6, 12 },
	{ 8e6, 8 }, { 4e6, 4 }, { 2e6, 2 }, { 1e6, 1 }, { 500e3, 50 },
	{ 200e3, 20 }, { 100e3, 10 },
};
#define NUM_RATES (sizeof(samplerates) / sizeof(samplerates[0]))

/* ------------------------------------------------------------------------
 * State (one scope)
 * ------------------------------------------------------------------------ */
struct gap {
	uint64_t at;            /* ring position (samples written) where it occurs */
	uint64_t n;             /* samples missing there */
};

static struct {
	int initialized;
	hu_mutex_t lock;        /* ring buffer and stream state */
	char err[512];          /* huLastError(), written by the API thread only */

	/* settings */
	int sim;                /* 0 hardware, 1 signals, 2 test pattern */
	double rate;
	unsigned char rate_code;
	int gain[2];
	char fwdir[1024];       /* "" = folder of this library */

	/* device */
	int open;
	int model;
	libusb_context *ctx;
	libusb_device_handle *h;

	/* acquisition */
	int streaming;
	int state;              /* 0 idle, 1 streaming, -1 error */
	char stream_err[256];   /* written by the USB thread, under lock */
	int last_usb_error;
	double t_start, t_stop, t_last_data;

	/* ring buffer of CH1,CH2 byte pairs; counters are absolute */
	unsigned char *ring;
	uint64_t cap, wr, rd;
	uint64_t skipped;       /* dropped samples before the read position */
	uint64_t delivered;     /* samples from the scope after the flush, incl. dropped */
	uint64_t dropped, gap_events;
	uint64_t bytes_in;      /* all bytes from the scope, incl. flushed */
	uint64_t flush_left;
	int have_carry;
	unsigned char carry;
	struct gap gaps[MAX_GAPS];
	int gap_head, gap_count;

	/* USB transfers */
	struct libusb_transfer *xfer[NUM_XFERS];
	int xfer_len;
	int active;             /* transfers in flight */
	int stop_req;
	hu_thread_t event_thread;
	volatile int event_run;

	/* simulation */
	hu_thread_t sim_thread;
	uint64_t sim_pairs;     /* byte pairs generated so far, incl. flushed */
} g;

static void ensure_init(void)
{
	if (g.initialized)
		return;
	mutex_init(&g.lock);
	g.rate = 1e6;
	g.rate_code = 1;
	g.gain[0] = g.gain[1] = 1;
	g.initialized = 1;
}

static int fail(int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g.err, sizeof(g.err), fmt, ap);
	va_end(ap);
	return code;
}

/* Called from the USB/simulation thread with g.lock held. */
static void stream_fail(int usb_error, const char *what)
{
	if (g.state != -1)
		snprintf(g.stream_err, sizeof(g.stream_err), "%s", what);
	if (usb_error)
		g.last_usb_error = usb_error;
	g.state = -1;
	g.stop_req = 1;
}

/* ------------------------------------------------------------------------
 * Ring buffer
 * ------------------------------------------------------------------------ */

/* Stores n byte pairs; when the ring is full the rest is dropped and a gap
 * is recorded at the write position. With g.lock held. */
static void ring_push_pairs(const unsigned char *p, uint64_t n)
{
	uint64_t space = g.cap - (g.wr - g.rd);
	uint64_t take = n < space ? n : space;
	uint64_t done = 0;

	/* Once the gap list is full nothing more is stored, so the newest gap
	 * stays at the write position and can keep growing. */
	if (g.gap_count == MAX_GAPS)
		take = 0;

	while (done < take) {
		uint64_t pos = (g.wr + done) % g.cap;
		uint64_t len = take - done;

		if (len > g.cap - pos)
			len = g.cap - pos;
		memcpy(g.ring + 2 * pos, p + 2 * done, (size_t)(2 * len));
		done += len;
	}
	g.wr += take;
	g.delivered += n;

	if (take < n) {
		uint64_t lost = n - take;
		struct gap *last = g.gap_count ?
			&g.gaps[(g.gap_head + g.gap_count - 1) % MAX_GAPS] : NULL;

		g.dropped += lost;
		if (last && last->at == g.wr) {
			last->n += lost;
		} else {
			struct gap *ng = &g.gaps[(g.gap_head + g.gap_count) % MAX_GAPS];

			ng->at = g.wr;
			ng->n = lost;
			g.gap_count++;
			g.gap_events++;
		}
	}
}

/* Raw bytes from the scope. With g.lock held. */
static void ring_push_bytes(const unsigned char *p, size_t n)
{
	g.bytes_in += n;
	if (g.flush_left) {
		size_t k = n < g.flush_left ? n : (size_t)g.flush_left;

		g.flush_left -= k;
		p += k;
		n -= k;
	}
	if (n && g.have_carry) {
		unsigned char pair[2] = { g.carry, p[0] };

		ring_push_pairs(pair, 1);
		g.have_carry = 0;
		p++;
		n--;
	}
	ring_push_pairs(p, n / 2);
	if (n & 1) {
		g.carry = p[n - 1];
		g.have_carry = 1;
	}
}

/* Moves the read position past gaps that start there. With g.lock held. */
static void skip_gaps(void)
{
	while (g.gap_count && g.gaps[g.gap_head].at == g.rd) {
		g.skipped += g.gaps[g.gap_head].n;
		g.gap_head = (g.gap_head + 1) % MAX_GAPS;
		g.gap_count--;
	}
}

/* Copies up to max contiguous samples. With g.lock held. */
static int ring_read(unsigned char *ch1, unsigned char *ch2, int max, double *first)
{
	uint64_t avail, n, done = 0;

	skip_gaps();
	avail = g.wr - g.rd;
	if (g.gap_count && g.gaps[g.gap_head].at - g.rd < avail)
		avail = g.gaps[g.gap_head].at - g.rd;
	n = avail < (uint64_t)max ? avail : (uint64_t)max;
	if (first)
		*first = (double)(g.rd + g.skipped);

	while (done < n) {
		uint64_t pos = (g.rd + done) % g.cap;
		uint64_t len = n - done, k;
		const unsigned char *src;

		if (len > g.cap - pos)
			len = g.cap - pos;
		src = g.ring + 2 * pos;
		for (k = 0; k < len; k++) {
			ch1[done + k] = src[2 * k];
			ch2[done + k] = src[2 * k + 1];
		}
		done += len;
	}
	g.rd += n;
	skip_gaps();
	return (int)n;
}

static void ring_reset(void)
{
	g.wr = g.rd = g.skipped = 0;
	g.delivered = g.dropped = g.gap_events = g.bytes_in = 0;
	g.flush_left = FLUSH_BYTES;
	g.have_carry = 0;
	g.gap_head = g.gap_count = 0;
	g.sim_pairs = 0;
}

/* ------------------------------------------------------------------------
 * Firmware
 * ------------------------------------------------------------------------ */
#ifdef _WIN32
/* Full path of name in the firmware folder, as UTF-16 for _wfopen. */
static void firmware_path(const char *name, wchar_t *out, size_t outlen)
{
	wchar_t wname[128];
	HMODULE hm = NULL;
	size_t len;

	out[0] = 0;
	if (g.fwdir[0]) {
		/* MATLAB passes text in the ANSI code page. */
		if (!MultiByteToWideChar(CP_ACP, 0, g.fwdir, -1, out, (int)outlen))
			out[0] = 0;
	} else if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)(void *)&firmware_path, &hm) &&
			GetModuleFileNameW(hm, out, (DWORD)outlen) < outlen) {
		wchar_t *slash = wcsrchr(out, L'\\');

		if (slash)
			*slash = 0;
	}
	len = wcslen(out);
	if (len && out[len - 1] != L'\\' && out[len - 1] != L'/' && len + 1 < outlen) {
		out[len++] = L'\\';
		out[len] = 0;
	}
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128) &&
			len + wcslen(wname) < outlen)
		wcscat(out, wname);
}

static FILE *firmware_open(const char *name, char *shown, size_t shownlen)
{
	wchar_t path[1200];

	firmware_path(name, path, 1200);
	WideCharToMultiByte(CP_ACP, 0, path, -1, shown, (int)shownlen, NULL, NULL);
	return _wfopen(path, L"rb");
}
#else
static FILE *firmware_open(const char *name, char *shown, size_t shownlen)
{
	char dir[1024] = "";
	Dl_info info;

	if (g.fwdir[0]) {
		snprintf(dir, sizeof(dir), "%s", g.fwdir);
	} else if (dladdr((void *)&firmware_open, &info) && info.dli_fname) {
		char *slash;

		snprintf(dir, sizeof(dir), "%s", info.dli_fname);
		slash = strrchr(dir, '/');
		if (slash)
			*slash = 0;
		else
			snprintf(dir, sizeof(dir), ".");
	}
	snprintf(shown, shownlen, "%s/%s", dir[0] ? dir : ".", name);
	return fopen(shown, "rb");
}
#endif

/* Reads a firmware image (raw FX2 RAM image). Returns its size or < 0. */
static int firmware_load(const char *name, unsigned char *buf)
{
	char shown[1200];
	FILE *f = firmware_open(name, shown, sizeof(shown));
	size_t n;

	if (!f)
		return fail(HU_ERR_FIRMWARE, "Firmware file not found: %s", shown);
	n = fread(buf, 1, FW_MAX_SIZE + 1, f);
	fclose(f);
	if (n == 0 || n > FW_MAX_SIZE)
		return fail(HU_ERR_FIRMWARE, "%s is not an FX2 firmware image (%u bytes)",
			shown, (unsigned)n);
	return (int)n;
}

static int fx2_write(libusb_device_handle *h, uint16_t addr, unsigned char *data, uint16_t len)
{
	return libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		REQ_FW_LOAD, addr, 0, data, len, CTRL_TIMEOUT_MS);
}

/* Uploads the firmware to a scope in its power-on state (as libsigrok's
 * ezusb_upload_firmware). The scope then disconnects and re-enumerates. */
static int fx2_upload(libusb_device *dev, const struct profile *p)
{
	static unsigned char fw[FW_MAX_SIZE + 1];
	libusb_device_handle *h;
	unsigned char cpucs;
	int len, r, cfg = 0, off;

	len = firmware_load(p->firmware, fw);
	if (len < 0)
		return len;

	r = libusb_open(dev, &h);
	if (r < 0) {
		g.last_usb_error = r;
		return fail(HU_ERR_NOT_FOUND,
			"Found a %s (USB %04X:%04X) but cannot open it (%s). On Windows it must "
			"use the WinUSB driver instead of the Hantek driver: run Zadig, select "
			"this device and install WinUSB (see libusb/README.md).",
			p->name, p->vid, p->pid, libusb_error_name(r));
	}
#ifndef _WIN32
	if (libusb_kernel_driver_active(h, 0) == 1)
		libusb_detach_kernel_driver(h, 0);
#endif
	if (libusb_get_configuration(h, &cfg) == 0 && cfg != 1)
		libusb_set_configuration(h, 1);

	cpucs = 1; /* hold the 8051 in reset */
	r = fx2_write(h, FX2_CPUCS, &cpucs, 1);
	for (off = 0; r >= 0 && off < len; off += FW_CHUNK) {
		int chunk = len - off < FW_CHUNK ? len - off : FW_CHUNK;

		r = fx2_write(h, (uint16_t)off, fw + off, (uint16_t)chunk);
	}
	if (r < 0) {
		g.last_usb_error = r;
		libusb_close(h);
		return fail(HU_ERR_FIRMWARE, "Firmware upload to the %s failed: %s",
			p->name, libusb_error_name(r));
	}
	/* Run it. The scope disconnects right after, so an error here is not
	 * fatal; the re-enumeration wait tells whether it worked. */
	cpucs = 0;
	fx2_write(h, FX2_CPUCS, &cpucs, 1);
	libusb_close(h);
	return 0;
}

HU_API int huCheckFirmware(int model)
{
	static unsigned char fw[FW_MAX_SIZE + 1];

	ensure_init();
	if (model != 1 && model != 2)
		return fail(HU_ERR_ARG, "model must be 1 (6022BL) or 2 (6022BE)");
	return firmware_load(model == 1 ? profiles[0].firmware : profiles[2].firmware, fw);
}

/* ------------------------------------------------------------------------
 * Device
 * ------------------------------------------------------------------------ */
static int vendor_write(unsigned char req, unsigned char value)
{
	int r;

	if (g.sim)
		return 0;
	r = libusb_control_transfer(g.h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		req, 0, 0, &value, 1, CTRL_TIMEOUT_MS);
	if (r < 0) {
		g.last_usb_error = r;
		return fail(HU_ERR_USB, "USB request 0x%02X failed: %s", req, libusb_error_name(r));
	}
	return 0;
}

static int configure(void)
{
	int r = vendor_write(REQ_CHANNELS, 2);

	if (r == 0)
		r = vendor_write(REQ_GAIN_CH1, (unsigned char)g.gain[0]);
	if (r == 0)
		r = vendor_write(REQ_GAIN_CH2, (unsigned char)g.gain[1]);
	if (r == 0)
		r = vendor_write(REQ_SAMPLERATE, g.rate_code);
	return r;
}

static int fw_model(const struct libusb_device_descriptor *d)
{
	if (d->idVendor != FW_VID || d->idProduct != FW_PID)
		return 0;
	if (d->bcdDevice == 0x0003)
		return 1;
	if (d->bcdDevice == 0x0001)
		return 2;
	return 0; /* another sigrok scope (DDS120, MDSO) */
}

/* Opens a scope that runs the fx2lafw firmware. Returns 1 if opened,
 * 0 if there is none, < 0 if one was found but could not be opened. */
static int open_running(int want_model)
{
	libusb_device **list;
	ssize_t cnt = libusb_get_device_list(g.ctx, &list), i;
	int result = 0;

	for (i = 0; i < cnt; i++) {
		struct libusb_device_descriptor d;
		int model, r;

		if (libusb_get_device_descriptor(list[i], &d) < 0)
			continue;
		model = fw_model(&d);
		if (!model || (want_model && model != want_model))
			continue;
		r = libusb_open(list[i], &g.h);
		if (r == 0) {
			g.model = model;
			result = 1;
			break;
		}
		g.last_usb_error = r;
		result = fail(HU_ERR_NOT_FOUND,
			"The scope runs the sigrok firmware (USB 1D50:608E) but cannot be opened "
			"(%s). On Windows install the WinUSB driver for this USB ID too with "
			"Zadig, then call huOpen again (see libusb/README.md).",
			libusb_error_name(r));
	}
	if (cnt >= 0)
		libusb_free_device_list(list, 1);
	return result;
}

/* Uploads the firmware to the first scope in its power-on state.
 * Returns its model, 0 if there is none, < 0 on error. */
static int upload_cold(void)
{
	libusb_device **list;
	ssize_t cnt = libusb_get_device_list(g.ctx, &list), i;
	int result = 0;
	size_t k;

	for (i = 0; i < cnt && result == 0; i++) {
		struct libusb_device_descriptor d;

		if (libusb_get_device_descriptor(list[i], &d) < 0)
			continue;
		for (k = 0; k < NUM_PROFILES; k++) {
			if (d.idVendor == profiles[k].vid && d.idProduct == profiles[k].pid) {
				int r = fx2_upload(list[i], &profiles[k]);

				result = r < 0 ? r : profiles[k].model;
				break;
			}
		}
	}
	if (cnt >= 0)
		libusb_free_device_list(list, 1);
	return result;
}

static int open_hw(void)
{
	int r, model;

	r = libusb_init(&g.ctx);
	if (r < 0) {
		g.ctx = NULL;
		g.last_usb_error = r;
		return fail(HU_ERR_USB, "libusb_init failed: %s", libusb_error_name(r));
	}

	r = open_running(0);
	if (r == 0) {
		model = upload_cold();
		if (model < 0) {
			r = model;
		} else if (model == 0) {
			r = fail(HU_ERR_NOT_FOUND,
				"No Hantek 6022BL/BE found (USB IDs 04B4:602A, 04B5:602A, 04B4:6022, "
				"04B5:6022 or 1D50:608E). Check the cable; on Windows the scope must "
				"use the WinUSB driver (Zadig, see libusb/README.md).");
		} else {
			/* Wait for the scope to come back with the new firmware. */
			double deadline = now_s() + RENUM_WAIT_S;

			sleep_ms(300);
			do {
				r = open_running(model);
				if (r == 0)
					r = fail(HU_ERR_NOT_FOUND, "The scope did not come back after "
						"the firmware upload (waited %.0f s).", RENUM_WAIT_S);
				if (r > 0)
					break;
				sleep_ms(200);
			} while (now_s() < deadline);
		}
	}
	if (r < 0) {
		libusb_exit(g.ctx);
		g.ctx = NULL;
		return r;
	}

	r = libusb_claim_interface(g.h, USB_IFACE);
	if (r == 0)
		r = libusb_set_interface_alt_setting(g.h, USB_IFACE, 0); /* bulk mode */
	if (r < 0) {
		g.last_usb_error = r;
		fail(HU_ERR_USB, "Cannot claim the scope's USB interface (%s). Is another "
			"program (e.g. PulseView) using it?", libusb_error_name(r));
		libusb_close(g.h);
		g.h = NULL;
		libusb_exit(g.ctx);
		g.ctx = NULL;
		return HU_ERR_USB;
	}
	return 0;
}

static void close_hw(void)
{
	if (g.h) {
		libusb_release_interface(g.h, USB_IFACE);
		libusb_close(g.h);
		g.h = NULL;
	}
	if (g.ctx) {
		libusb_exit(g.ctx);
		g.ctx = NULL;
	}
}

/* ------------------------------------------------------------------------
 * USB streaming
 * ------------------------------------------------------------------------ */
static THREAD_RET event_loop(void *arg)
{
	(void)arg;
	while (g.event_run) {
		struct timeval tv = { 0, 100000 };

		libusb_handle_events_timeout_completed(g.ctx, &tv, NULL);
	}
	THREAD_RETURN;
}

static void LIBUSB_CALL xfer_done(struct libusb_transfer *t)
{
	int resubmit = 0;

	mutex_lock(&g.lock);
	if (t->actual_length > 0) {
		ring_push_bytes(t->buffer, (size_t)t->actual_length);
		g.t_last_data = now_s();
	}
	switch (t->status) {
	case LIBUSB_TRANSFER_COMPLETED:
		resubmit = !g.stop_req;
		break;
	case LIBUSB_TRANSFER_CANCELLED:
		break;
	case LIBUSB_TRANSFER_NO_DEVICE:
		stream_fail(LIBUSB_ERROR_NO_DEVICE, "The scope was disconnected.");
		break;
	default:
		if (!g.stop_req)
			stream_fail(LIBUSB_ERROR_IO, "USB transfer failed.");
		break;
	}
	if (resubmit) {
		int r = libusb_submit_transfer(t);

		if (r < 0) {
			stream_fail(r, "Could not resubmit a USB transfer.");
			resubmit = 0;
		}
	}
	if (!resubmit)
		g.active--;
	mutex_unlock(&g.lock);
}

static void free_xfers(void)
{
	int i;

	for (i = 0; i < NUM_XFERS; i++) {
		if (g.xfer[i]) {
			free(g.xfer[i]->buffer);
			libusb_free_transfer(g.xfer[i]);
			g.xfer[i] = NULL;
		}
	}
}

static int usb_stop(void)
{
	double deadline;
	int i, active;

	mutex_lock(&g.lock);
	g.stop_req = 1;
	mutex_unlock(&g.lock);

	vendor_write(REQ_START, 0); /* any vendor request stops sampling */
	g.t_stop = now_s();
	for (i = 0; i < NUM_XFERS; i++) {
		if (g.xfer[i])
			libusb_cancel_transfer(g.xfer[i]);
	}
	deadline = now_s() + 3.0;
	do {
		mutex_lock(&g.lock);
		active = g.active;
		mutex_unlock(&g.lock);
		if (active)
			sleep_ms(5);
	} while (active && now_s() < deadline);

	g.event_run = 0;
	libusb_interrupt_event_handler(g.ctx);
	thread_join(g.event_thread);

	if (active) {
		/* Should not happen; never free a transfer that is still queued. */
		for (i = 0; i < NUM_XFERS; i++)
			g.xfer[i] = NULL;
		return fail(HU_ERR_USB, "%d USB transfers did not finish.", active);
	}
	free_xfers();
	return 0;
}

static int usb_start(void)
{
	double bytes_per_s = 2.0 * g.rate;
	int len, i, r;

	/* About 20 ms of data per transfer, a multiple of the 512-byte packet. */
	len = (int)(bytes_per_s * 0.02);
	len = (len + 511) / 512 * 512;
	if (len < XFER_MIN)
		len = XFER_MIN;
	if (len > XFER_MAX)
		len = XFER_MAX;
	g.xfer_len = len;

	r = configure();
	if (r < 0)
		return r;

	for (i = 0; i < NUM_XFERS; i++) {
		unsigned char *buf = malloc((size_t)len);

		g.xfer[i] = libusb_alloc_transfer(0);
		if (!buf || !g.xfer[i]) {
			free(buf);
			free_xfers();
			return fail(HU_ERR_NO_MEMORY, "Out of memory for USB transfers.");
		}
		libusb_fill_bulk_transfer(g.xfer[i], g.h, EP_IN, buf, len, xfer_done, NULL, 0);
	}

	g.stop_req = 0;
	g.active = 0;
	g.event_run = 1;
	if (thread_start(&g.event_thread, event_loop) < 0) {
		g.event_run = 0;
		free_xfers();
		return fail(HU_ERR_NO_MEMORY, "Could not start the USB thread.");
	}

	/* Queue every transfer before sampling starts, so the host is always
	 * ready for the next packet. */
	mutex_lock(&g.lock);
	for (i = 0; i < NUM_XFERS; i++) {
		r = libusb_submit_transfer(g.xfer[i]);
		if (r < 0)
			break;
		g.active++;
	}
	g.t_start = g.t_last_data = now_s();
	mutex_unlock(&g.lock);
	if (r == 0)
		r = vendor_write(REQ_START, 1);
	else
		r = fail(HU_ERR_USB, "Could not queue USB transfers: %s", libusb_error_name(r));
	if (r < 0) {
		usb_stop();
		return r;
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * Simulation: same ring-buffer path as the USB data, in real time
 * ------------------------------------------------------------------------ */
static unsigned char clip(double v)
{
	return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Byte pair number p since the start (the first FLUSH_BYTES/2 are
 * discarded, so stream index = p - FLUSH_BYTES/2). */
static void sim_pair(uint64_t p, unsigned char *out)
{
	int64_t i = (int64_t)p - FLUSH_BYTES / 2;

	if (g.sim == 2) { /* test pattern: a function of the stream index */
		out[0] = (unsigned char)(i * 7 + 3);
		out[1] = (unsigned char)((i >> 8) + (i >> 16) * 31);
	} else {          /* 1 kHz 0..3.3 V square wave, 50 Hz +-1 V sine */
		double t = (double)i / g.rate;
		uint32_t x = (uint32_t)p * 2654435761u;
		double noise = (double)(x >> 30) - 1.5;
		double v1 = fmod(t, 1e-3) < 0.5e-3 ? 3.3 : 0.0;
		double v2 = sin(2 * 3.14159265358979 * 50 * t);

		out[0] = clip(128 + v1 * 25 * g.gain[0] + noise * 0.5);
		out[1] = clip(128 + v2 * 25 * g.gain[1] + noise * 0.5);
	}
}

static THREAD_RET sim_loop(void *arg)
{
	static unsigned char buf[2 * 65536];

	(void)arg;
	for (;;) {
		uint64_t target, n, k;

		mutex_lock(&g.lock);
		if (g.stop_req) {
			mutex_unlock(&g.lock);
			break;
		}
		mutex_unlock(&g.lock);

		target = (uint64_t)((now_s() - g.t_start) * g.rate);
		while (g.sim_pairs < target) {
			n = target - g.sim_pairs;
			if (n > 65536)
				n = 65536;
			for (k = 0; k < n; k++)
				sim_pair(g.sim_pairs + k, buf + 2 * k);
			mutex_lock(&g.lock);
			ring_push_bytes(buf, (size_t)(2 * n));
			g.t_last_data = now_s();
			mutex_unlock(&g.lock);
			g.sim_pairs += n;
		}
		sleep_ms(2);
	}
	THREAD_RETURN;
}

/* ------------------------------------------------------------------------
 * Acquisition control
 * ------------------------------------------------------------------------ */
static int start_acq(uint64_t cap)
{
	int r;

	if (cap < RING_MIN)
		cap = RING_MIN;
	if (cap > RING_MAX)
		cap = RING_MAX;
	if (!g.ring || g.cap != cap) {
		free(g.ring);
		g.ring = malloc((size_t)(2 * cap));
		g.cap = g.ring ? cap : 0;
		if (!g.ring)
			return fail(HU_ERR_NO_MEMORY, "Out of memory for a %.0f-sample buffer.",
				(double)cap);
	}
	mutex_lock(&g.lock);
	ring_reset();
	g.state = 1;
	g.stream_err[0] = 0;
	g.last_usb_error = 0;
	g.stop_req = 0;
	g.t_start = g.t_last_data = now_s();
	g.t_stop = 0;
	mutex_unlock(&g.lock);

	if (g.sim) {
		r = thread_start(&g.sim_thread, sim_loop) < 0 ?
			fail(HU_ERR_NO_MEMORY, "Could not start the simulation thread.") : 0;
	} else {
		r = usb_start();
	}
	if (r < 0) {
		g.state = -1;
		return r;
	}
	g.streaming = 1;
	return 0;
}

static int stop_acq(void)
{
	int r = 0;

	if (!g.streaming)
		return 0;
	if (g.sim) {
		mutex_lock(&g.lock);
		g.stop_req = 1;
		mutex_unlock(&g.lock);
		thread_join(g.sim_thread);
		g.t_stop = now_s();
	} else {
		r = usb_stop();
	}
	g.streaming = 0;
	mutex_lock(&g.lock);
	if (g.state == 1)
		g.state = 0;
	mutex_unlock(&g.lock);
	return r;
}

/* ------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------ */
HU_API int huSetFirmwareDir(const char *dir)
{
	ensure_init();
	if (dir && strlen(dir) >= sizeof(g.fwdir))
		return fail(HU_ERR_ARG, "Firmware folder name too long.");
	snprintf(g.fwdir, sizeof(g.fwdir), "%s", dir ? dir : "");
	return 0;
}

HU_API int huSetSimulation(int mode)
{
	ensure_init();
	if (mode < 0 || mode > 2)
		return fail(HU_ERR_ARG, "Simulation mode must be 0, 1 or 2.");
	if (g.open)
		return fail(HU_ERR_BUSY, "Call huSetSimulation before huOpen.");
	g.sim = mode;
	return 0;
}

HU_API int huOpen(void)
{
	int r;

	ensure_init();
	if (g.open)
		return g.model;
	if (g.sim) {
		g.model = 1;
	} else {
		r = open_hw();
		if (r < 0)
			return r;
		r = configure();
		if (r < 0) {
			close_hw();
			return r;
		}
	}
	g.open = 1;
	g.err[0] = 0;
	return g.model;
}

HU_API void huClose(void)
{
	ensure_init();
	stop_acq();
	if (!g.sim)
		close_hw();
	g.open = 0;
	free(g.ring);
	g.ring = NULL;
	g.cap = 0;
	g.state = 0;
}

HU_API int huSetSampleRate(double rate)
{
	size_t i;

	ensure_init();
	for (i = 0; i < NUM_RATES; i++) {
		if (fabs(rate - samplerates[i].rate) <= samplerates[i].rate * 1e-6)
			break;
	}
	if (i == NUM_RATES)
		return fail(HU_ERR_ARG, "Unsupported sample rate %g: use 48e6, 30e6, 24e6, "
			"16e6, 12e6, 8e6, 4e6, 2e6, 1e6, 500e3, 200e3 or 100e3.", rate);
	if (g.streaming)
		return fail(HU_ERR_BUSY, "Stop streaming before changing the sample rate.");
	g.rate = samplerates[i].rate;
	g.rate_code = samplerates[i].code;
	return g.open ? vendor_write(REQ_SAMPLERATE, g.rate_code) : 0;
}

HU_API double huGetSampleRate(void)
{
	ensure_init();
	return g.rate;
}

HU_API int huSetGain(int channel, int gain)
{
	ensure_init();
	if (channel != 0 && channel != 1)
		return fail(HU_ERR_ARG, "Channel must be 0 (CH1) or 1 (CH2).");
	if (gain != 1 && gain != 2 && gain != 5 && gain != 10)
		return fail(HU_ERR_ARG, "Gain must be 1, 2, 5 or 10.");
	if (g.streaming)
		return fail(HU_ERR_BUSY, "Stop streaming before changing the gain.");
	g.gain[channel] = gain;
	return g.open ? vendor_write(channel ? REQ_GAIN_CH2 : REQ_GAIN_CH1,
		(unsigned char)gain) : 0;
}

HU_API int startStreaming(int bufferSamples)
{
	uint64_t cap;

	ensure_init();
	if (!g.open)
		return fail(HU_ERR_NOT_OPEN, "Call huOpen first.");
	if (g.streaming)
		return fail(HU_ERR_BUSY, "Already streaming.");
	if (bufferSamples < 0)
		return fail(HU_ERR_ARG, "bufferSamples must be >= 0.");
	cap = bufferSamples > 0 ? (uint64_t)bufferSamples :
		(uint64_t)(g.rate * RING_DEFAULT_S);
	if (!bufferSamples && cap > (uint64_t)1 << 27)
		cap = (uint64_t)1 << 27; /* 256 MB */
	return start_acq(cap);
}

HU_API int stopStreaming(void)
{
	ensure_init();
	return stop_acq();
}

/* HU_ERR_STREAM / HU_ERR_TIMEOUT if the stream failed; with g.lock held. */
static int stream_problem(void)
{
	double quiet;

	if (g.state == -1) {
		snprintf(g.err, sizeof(g.err), "%s", g.stream_err);
		return HU_ERR_STREAM;
	}
	if (g.streaming) {
		quiet = now_s() - g.t_last_data;
		if (quiet > NO_DATA_S + 4.0 * g.xfer_len / (2.0 * g.rate)) {
			snprintf(g.err, sizeof(g.err), "No data from the scope for %.1f s.", quiet);
			return HU_ERR_TIMEOUT;
		}
	}
	return 0;
}

HU_API int getStreamData(unsigned char *ch1, unsigned char *ch2, int maxSamples, double *firstIndex)
{
	int n, problem;

	ensure_init();
	if (firstIndex)
		*firstIndex = 0;
	if (!ch1 || !ch2 || maxSamples < 0)
		return fail(HU_ERR_ARG, "Invalid buffers.");
	if (!g.ring)
		return fail(HU_ERR_NOT_OPEN, "Nothing recorded: call startStreaming first.");
	mutex_lock(&g.lock);
	n = ring_read(ch1, ch2, maxSamples, firstIndex);
	problem = n ? 0 : stream_problem();
	mutex_unlock(&g.lock);
	return problem ? problem : n;
}

HU_API int getQueueStatus(double *s)
{
	double elapsed;

	ensure_init();
	if (!s)
		return fail(HU_ERR_ARG, "status must hold %d doubles.", HU_STATUS_LEN);
	mutex_lock(&g.lock);
	elapsed = g.streaming ? now_s() - g.t_start :
		(g.t_stop > 0 ? g.t_stop - g.t_start : 0);
	s[0] = g.state;
	s[1] = (double)(g.wr - g.rd);
	s[2] = (double)g.cap;
	s[3] = (double)g.delivered;
	s[4] = (double)g.dropped;
	s[5] = (double)g.gap_events;
	s[6] = elapsed;
	s[7] = g.rate;
	s[8] = elapsed > 0 ? (double)g.bytes_in / 2.0 / elapsed : 0;
	s[9] = (double)(g.rd + g.skipped);
	s[10] = g.last_usb_error;
	s[11] = g.xfer_len;
	if (g.state == -1)
		snprintf(g.err, sizeof(g.err), "%s", g.stream_err);
	mutex_unlock(&g.lock);
	return 0;
}

HU_API int huReadBlock(unsigned char *ch1, unsigned char *ch2, int nSamples)
{
	double deadline, first;
	int r, n;
	uint64_t have;

	ensure_init();
	if (!ch1 || !ch2 || nSamples <= 0)
		return fail(HU_ERR_ARG, "Invalid buffers or length.");
	if (!g.open)
		return fail(HU_ERR_NOT_OPEN, "Call huOpen first.");
	if (g.streaming)
		return fail(HU_ERR_BUSY, "Stop streaming before huReadBlock.");

	/* A ring of at least nSamples cannot overflow before the first nSamples
	 * are in, so they are contiguous. */
	r = start_acq((uint64_t)nSamples);
	if (r < 0)
		return r;
	deadline = now_s() + nSamples / g.rate + 5.0;
	for (;;) {
		mutex_lock(&g.lock);
		have = g.wr - g.rd;
		r = stream_problem();
		mutex_unlock(&g.lock);
		if (have >= (uint64_t)nSamples || r < 0)
			break;
		if (now_s() > deadline) {
			r = fail(HU_ERR_TIMEOUT, "Timed out waiting for %d samples.", nSamples);
			break;
		}
		sleep_ms(1);
	}
	stop_acq();
	if (r < 0)
		return r;
	mutex_lock(&g.lock);
	n = ring_read(ch1, ch2, nSamples, &first);
	mutex_unlock(&g.lock);
	return n == nSamples ? n : fail(HU_ERR_STREAM, "Got %d of %d samples.", n, nSamples);
}

HU_API const char *huLastError(void)
{
	ensure_init();
	return g.err;
}
