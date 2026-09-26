/*
 * Fake libusb for the tests: implements the libusb calls HantekUSB.c makes
 * against a model of a Hantek 6022BL, following sigrok-firmware-fx2lafw
 * include/scope.inc, so that HantekUSB's USB code runs end to end without a
 * scope:
 *  - power-on state 04B4:602A: only the FX2 boot loader (request 0xA0 writes
 *    RAM while CPUCS holds the CPU in reset); releasing the CPU after an
 *    upload makes the device drop off the bus and come back 0.5 s later as
 *    1D50:608E, bcdDevice 0x0003
 *  - firmware: vendor requests 0xE0..0xE6, each of which first stops sampling
 *    and commits the partial packet (a zero-length packet if empty); 0xE3 = 1
 *    starts sampling; samples go through a 4 x 512-byte FIFO to bulk EP 0x86,
 *    and are lost when the FIFO is full because no transfer is waiting
 *  - data: 1024 garbage pairs, then the same test pattern as the simulation
 *    mode of HantekUSB (a function of the stream index)
 */
#define _GNU_SOURCE
#include <libusb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fake_libusb.h"

struct libusb_context { int unused; };
struct libusb_device { uint16_t vid, pid, bcd; };
struct libusb_device_handle { struct libusb_device *dev; int generation; };

enum { ST_COLD, ST_RENUM, ST_RUNNING, ST_GONE };

#define MAX_PENDING 64

static struct libusb_device dev_cold = { 0x04b4, 0x602a, 0x0000 };
static struct libusb_device dev_fw = { 0x1d50, 0x608e, 0x0003 };
static struct libusb_device dev_other = { 0x046d, 0xc077, 0x0100 }; /* a mouse */

struct packet { unsigned char data[512]; int len; };

static struct {
	pthread_mutex_t lock;
	int state, generation;
	double renum_at;
	unsigned char ram[0x4000];
	size_t ram_top;
	int cpucs, uploads, open_error;
	/* firmware */
	int gain[2], rate_code, channels, sampling;
	double rate, t0;
	uint64_t pairs, lost_pairs;
	unsigned char partial[512];
	int partial_len;
	struct packet fifo[4];
	int fifo_count;
	/* host side */
	struct libusb_transfer *pending[MAX_PENDING];
	int cancel[MAX_PENDING], npending;
	struct libusb_transfer *done[MAX_PENDING];
	int ndone;
	int interrupted;
	int requests[256];
} fk = { .lock = PTHREAD_MUTEX_INITIALIZER };

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void firmware_init(void) /* scope.inc init() */
{
	fk.gain[0] = fk.gain[1] = 1;
	fk.rate_code = 1;
	fk.rate = 1e6;
	fk.channels = 2;
	fk.sampling = 0;
	fk.partial_len = fk.fifo_count = 0;
}

static void update_state(void)
{
	if (fk.state == ST_RENUM && now_s() >= fk.renum_at) {
		fk.state = ST_RUNNING;
		fk.generation++;
		firmware_init();
	}
}

/* ---- data path ---- */

static void complete(int i, enum libusb_transfer_status status)
{
	struct libusb_transfer *t = fk.pending[i];

	t->status = status;
	fk.done[fk.ndone++] = t;
	memmove(&fk.pending[i], &fk.pending[i + 1], (fk.npending - i - 1) * sizeof(fk.pending[0]));
	memmove(&fk.cancel[i], &fk.cancel[i + 1], (fk.npending - i - 1) * sizeof(fk.cancel[0]));
	fk.npending--;
}

/* Hands one packet to the oldest waiting transfer; 0 if none is waiting. */
static int deliver(const unsigned char *data, int len)
{
	struct libusb_transfer *t;

	if (fk.npending == 0 || fk.cancel[0])
		return 0;
	t = fk.pending[0];
	memcpy(t->buffer + t->actual_length, data, (size_t)len);
	t->actual_length += len;
	if (len < 512 || t->actual_length + 512 > t->length)
		complete(0, LIBUSB_TRANSFER_COMPLETED); /* short packet or full */
	return 1;
}

static void pump_fifo(void)
{
	while (fk.fifo_count && deliver(fk.fifo[0].data, fk.fifo[0].len)) {
		memmove(&fk.fifo[0], &fk.fifo[1], (size_t)(fk.fifo_count - 1) * sizeof(fk.fifo[0]));
		fk.fifo_count--;
	}
}

static void commit_packet(const unsigned char *data, int len)
{
	pump_fifo();
	if (fk.fifo_count == 0 && deliver(data, len))
		return;
	if (fk.fifo_count < 4) {
		memcpy(fk.fifo[fk.fifo_count].data, data, (size_t)len);
		fk.fifo[fk.fifo_count++].len = len;
	} else {
		fk.lost_pairs += (uint64_t)len / 2; /* FIFO full: samples lost */
	}
}

static void produce(void)
{
	uint64_t target;

	if (!fk.sampling)
		return;
	target = (uint64_t)((now_s() - fk.t0) * fk.rate);
	while (fk.pairs < target) {
		int64_t i = (int64_t)fk.pairs - 1024;

		fk.partial[fk.partial_len++] = i < 0 ? 0xee : (unsigned char)(i * 7 + 3);
		fk.partial[fk.partial_len++] = i < 0 ? 0xee : (unsigned char)((i >> 8) + (i >> 16) * 31);
		fk.pairs++;
		if (fk.partial_len == 512) {
			commit_packet(fk.partial, 512);
			fk.partial_len = 0;
		}
	}
}

static void stop_sampling(void) /* GPIFABORT + INPKTEND */
{
	produce();
	fk.sampling = 0;
	commit_packet(fk.partial, fk.partial_len); /* zero-length if empty */
	fk.partial_len = 0;
}

static void start_sampling(void)
{
	fk.fifo_count = fk.partial_len = 0; /* clear_fifo() */
	fk.pairs = 0;
	fk.t0 = now_s();
	fk.sampling = 1;
}

static int firmware_request(uint8_t req, unsigned char val)
{
	static const struct { int code; double rate; } rates[] = {
		{ 48, 48e6 }, { 30, 30e6 }, { 24, 24e6 }, { 16, 16e6 }, { 12, 12e6 },
		{ 8, 8e6 }, { 4, 4e6 }, { 2, 2e6 }, { 1, 1e6 }, { 50, 500e3 },
		{ 20, 200e3 }, { 10, 100e3 },
	};
	size_t k;

	if (req < 0xe0 || req > 0xe6)
		return LIBUSB_ERROR_PIPE; /* not handled: STALL */
	stop_sampling();
	switch (req) {
	case 0xe0:
	case 0xe1:
		if (val == 1 || val == 2 || val == 5 || val == 10)
			fk.gain[req - 0xe0] = val;
		break;
	case 0xe2:
		for (k = 0; k < sizeof(rates) / sizeof(rates[0]); k++) {
			if (rates[k].code == val) {
				fk.rate_code = val;
				fk.rate = rates[k].rate;
			}
		}
		break;
	case 0xe3:
		if (val == 1)
			start_sampling();
		break;
	case 0xe4:
		if (val == 1 || val == 2)
			fk.channels = val;
		break;
	}
	return 1;
}

/* ---- libusb API ---- */

int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	*ctx = calloc(1, sizeof(**ctx));
	return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx) { free(ctx); }

const char *LIBUSB_CALL libusb_error_name(int code)
{
	switch (code) {
	case LIBUSB_ERROR_NO_DEVICE: return "LIBUSB_ERROR_NO_DEVICE";
	case LIBUSB_ERROR_NOT_SUPPORTED: return "LIBUSB_ERROR_NOT_SUPPORTED";
	case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE";
	case LIBUSB_ERROR_NOT_FOUND: return "LIBUSB_ERROR_NOT_FOUND";
	default: return "LIBUSB_ERROR_OTHER";
	}
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
	libusb_device **l = calloc(3, sizeof(*l));
	ssize_t n = 0;

	(void)ctx;
	pthread_mutex_lock(&fk.lock);
	update_state();
	l[n++] = &dev_other;
	if (fk.state == ST_COLD)
		l[n++] = &dev_cold;
	else if (fk.state == ST_RUNNING)
		l[n++] = &dev_fw;
	pthread_mutex_unlock(&fk.lock);
	*list = l;
	return n;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref)
{
	(void)unref;
	free(list);
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	d->idVendor = dev->vid;
	d->idProduct = dev->pid;
	d->bcdDevice = dev->bcd;
	return 0;
}

static int present(libusb_device *dev)
{
	return (dev == &dev_cold && fk.state == ST_COLD) ||
		(dev == &dev_fw && fk.state == ST_RUNNING) || dev == &dev_other;
}

int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **h)
{
	int r = 0;

	pthread_mutex_lock(&fk.lock);
	update_state();
	if (fk.open_error)
		r = fk.open_error;
	else if (!present(dev))
		r = LIBUSB_ERROR_NO_DEVICE;
	if (r == 0) {
		*h = calloc(1, sizeof(**h));
		(*h)->dev = dev;
		(*h)->generation = fk.generation;
	}
	pthread_mutex_unlock(&fk.lock);
	return r;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *h) { free(h); }

static int stale(libusb_device_handle *h)
{
	return h->generation != fk.generation || fk.state == ST_GONE || !present(h->dev);
}

int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int LIBUSB_CALL libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int LIBUSB_CALL libusb_get_configuration(libusb_device_handle *h, int *c) { (void)h; *c = 1; return 0; }
int LIBUSB_CALL libusb_set_configuration(libusb_device_handle *h, int c) { (void)h; (void)c; return 0; }

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *h, int i)
{
	int r;

	pthread_mutex_lock(&fk.lock);
	r = stale(h) ? LIBUSB_ERROR_NO_DEVICE : (i == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND);
	pthread_mutex_unlock(&fk.lock);
	return r;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }

int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int alt)
{
	(void)h;
	return i == 0 && alt == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND; /* HantekUSB uses bulk only */
}

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t req,
		uint16_t value, uint16_t index, unsigned char *data, uint16_t len, unsigned int timeout)
{
	int r = len;

	(void)index;
	(void)timeout;
	pthread_mutex_lock(&fk.lock);
	update_state();
	fk.requests[req]++;
	if (stale(h)) {
		r = LIBUSB_ERROR_NO_DEVICE;
	} else if (type != (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)) {
		r = LIBUSB_ERROR_PIPE;
	} else if (h->dev == &dev_cold) {
		if (req != 0xa0) {
			r = LIBUSB_ERROR_PIPE; /* no firmware: only the boot loader answers */
		} else if (value == 0xe600 && len == 1) {
			if (fk.cpucs == 1 && data[0] == 0 && fk.ram_top > 0) {
				fk.uploads++;
				fk.state = ST_RENUM; /* drops off the bus, comes back later */
				fk.generation++;
				fk.renum_at = now_s() + 0.5;
			}
			fk.cpucs = data[0];
		} else if (fk.cpucs != 1 || (size_t)value + len > sizeof(fk.ram)) {
			r = LIBUSB_ERROR_PIPE;
		} else {
			memcpy(fk.ram + value, data, len);
			if ((size_t)value + len > fk.ram_top)
				fk.ram_top = (size_t)value + len;
		}
	} else if (h->dev == &dev_fw) {
		r = len == 1 ? firmware_request(req, data[0]) : LIBUSB_ERROR_PIPE;
	} else {
		r = LIBUSB_ERROR_PIPE;
	}
	pthread_mutex_unlock(&fk.lock);
	return r;
}

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso)
{
	return calloc(1, sizeof(struct libusb_transfer) +
		(size_t)iso * sizeof(struct libusb_iso_packet_descriptor));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *t) { free(t); }

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *t)
{
	int r = 0;

	pthread_mutex_lock(&fk.lock);
	update_state();
	if (t->endpoint != 0x86 || t->type != LIBUSB_TRANSFER_TYPE_BULK) {
		r = LIBUSB_ERROR_INVALID_PARAM;
	} else if (stale(t->dev_handle)) {
		r = LIBUSB_ERROR_NO_DEVICE;
	} else if (fk.npending == MAX_PENDING) {
		r = LIBUSB_ERROR_BUSY;
	} else {
		t->actual_length = 0;
		fk.cancel[fk.npending] = 0;
		fk.pending[fk.npending++] = t;
		produce();
		pump_fifo();
	}
	pthread_mutex_unlock(&fk.lock);
	return r;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *t)
{
	int i, r = LIBUSB_ERROR_NOT_FOUND;

	pthread_mutex_lock(&fk.lock);
	for (i = 0; i < fk.npending; i++) {
		if (fk.pending[i] == t) {
			fk.cancel[i] = 1;
			r = 0;
		}
	}
	pthread_mutex_unlock(&fk.lock);
	return r;
}

void LIBUSB_CALL libusb_interrupt_event_handler(libusb_context *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&fk.lock);
	fk.interrupted = 1;
	pthread_mutex_unlock(&fk.lock);
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *ctx,
		struct timeval *tv, int *completed)
{
	double end = now_s() + tv->tv_sec + tv->tv_usec * 1e-6;
	struct libusb_transfer *done[MAX_PENDING];
	struct timespec nap = { 0, 200000 };

	(void)ctx;
	(void)completed;
	for (;;) {
		int i, n, interrupted;

		pthread_mutex_lock(&fk.lock);
		update_state();
		produce();
		pump_fifo();
		for (i = 0; i < fk.npending; i++) {
			if (fk.cancel[i])
				complete(i--, LIBUSB_TRANSFER_CANCELLED);
		}
		n = fk.ndone;
		memcpy(done, fk.done, (size_t)n * sizeof(done[0]));
		fk.ndone = 0;
		interrupted = fk.interrupted;
		fk.interrupted = 0;
		pthread_mutex_unlock(&fk.lock);

		for (i = 0; i < n; i++)
			done[i]->callback(done[i]);
		if (n || interrupted || now_s() >= end)
			return 0;
		nanosleep(&nap, NULL);
	}
}

/* ---- test hooks ---- */

void fake_plug_cold(void)
{
	pthread_mutex_lock(&fk.lock);
	fk.state = ST_COLD;
	fk.generation++;
	fk.cpucs = 0;
	fk.ram_top = 0;
	fk.open_error = 0;
	fk.lost_pairs = 0;
	pthread_mutex_unlock(&fk.lock);
}

void fake_unplug(void)
{
	pthread_mutex_lock(&fk.lock);
	fk.state = ST_GONE;
	fk.generation++;
	fk.sampling = 0;
	while (fk.npending)
		complete(0, LIBUSB_TRANSFER_NO_DEVICE);
	pthread_mutex_unlock(&fk.lock);
}

void fake_set_open_error(int err)
{
	pthread_mutex_lock(&fk.lock);
	fk.open_error = err;
	pthread_mutex_unlock(&fk.lock);
}

const unsigned char *fake_ram(size_t *len)
{
	*len = fk.ram_top;
	return fk.ram;
}

int fake_uploads(void) { return fk.uploads; }
int fake_requests(int req) { return fk.requests[req]; }
double fake_lost_samples(void) { return (double)fk.lost_pairs; }

void fake_scope_state(int *gain1, int *gain2, int *rate_code, int *channels, int *sampling)
{
	pthread_mutex_lock(&fk.lock);
	*gain1 = fk.gain[0];
	*gain2 = fk.gain[1];
	*rate_code = fk.rate_code;
	*channels = fk.channels;
	*sampling = fk.sampling;
	pthread_mutex_unlock(&fk.lock);
}
