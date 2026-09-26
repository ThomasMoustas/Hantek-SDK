/* Test hooks of fake_libusb.c (a modelled Hantek 6022BL behind the libusb API). */
#ifndef FAKE_LIBUSB_H
#define FAKE_LIBUSB_H
#include <stddef.h>

void fake_plug_cold(void);            /* plug in a scope in its power-on state */
void fake_unplug(void);
void fake_set_open_error(int err);    /* e.g. LIBUSB_ERROR_NOT_SUPPORTED: no WinUSB driver */
const unsigned char *fake_ram(size_t *len); /* firmware image written by the host */
int fake_uploads(void);
int fake_requests(int req);           /* control requests seen, per bRequest */
double fake_lost_samples(void);       /* lost because no transfer was waiting */
void fake_scope_state(int *gain1, int *gain2, int *rate_code, int *channels, int *sampling);
#endif
