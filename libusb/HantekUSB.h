/*
 * HantekUSB - native 64-bit access to the Hantek 6022BL / 6022BE for MATLAB,
 * without the 32-bit Hantek SDK: libusb + the open-source sigrok fx2lafw
 * firmware, with gapless streaming into a ring buffer.
 *
 * All functions return >= 0 on success and a negative HU_ERR_* code on
 * failure; huLastError() describes the last failure. Samples are raw ADC
 * counts 0..255 (about 128 = 0 V); volts = (raw - zero) / (25 * gain).
 *
 * Only one scope at a time; call the functions from one thread (MATLAB).
 */
#ifndef HANTEKUSB_H
#define HANTEKUSB_H

#ifdef _WIN32
#define HU_API __declspec(dllexport)
#else
#define HU_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HU_ERR_ARG        -1  /* invalid argument */
#define HU_ERR_NOT_OPEN   -2  /* huOpen() first */
#define HU_ERR_NOT_FOUND  -3  /* no 6022BL/BE found, or it has no WinUSB driver */
#define HU_ERR_FIRMWARE   -4  /* firmware file missing or upload failed */
#define HU_ERR_USB        -5  /* USB error */
#define HU_ERR_BUSY       -6  /* not allowed while streaming */
#define HU_ERR_TIMEOUT    -7  /* no data from the scope */
#define HU_ERR_NO_MEMORY  -8
#define HU_ERR_STREAM     -9  /* streaming stopped by a USB error */

#define HU_STATUS_LEN 12      /* number of doubles written by getQueueStatus */

/* Folder with fx2lafw-hantek-6022bl.fw / -6022be.fw (NULL or "" = the folder
 * of this library). Call before huOpen(). */
HU_API int huSetFirmwareDir(const char *dir);

/* Checks that the firmware file for model 1 (6022BL) or 2 (6022BE) is found
 * and valid; returns its size. */
HU_API int huCheckFirmware(int model);

/* 0 = real scope (default), 1 = simulated signals, 2 = simulated test
 * pattern. Lets the MATLAB code run without hardware. Call before huOpen(). */
HU_API int huSetSimulation(int mode);

/* Finds the scope, uploads the firmware if needed and waits for it to
 * restart. Returns 1 for a 6022BL, 2 for a 6022BE. */
HU_API int huOpen(void);
HU_API void huClose(void);

/* Sample rate in Sa/s: 48e6, 30e6, 24e6, 16e6, 12e6, 8e6, 4e6, 2e6, 1e6,
 * 500e3, 200e3 or 100e3 (default 1e6). Both channels are always sampled. */
HU_API int huSetSampleRate(double rate);
HU_API double huGetSampleRate(void);

/* Hardware gain 1, 2, 5 or 10 (default 1) for channel 0 (CH1) or 1 (CH2):
 * input range about +-5.12 V / gain. */
HU_API int huSetGain(int channel, int gain);

/* One block of nSamples contiguous samples per channel. Blocks until done.
 * Returns nSamples. */
HU_API int huReadBlock(unsigned char *ch1, unsigned char *ch2, int nSamples);

/* Continuous acquisition into a ring buffer of bufferSamples samples per
 * channel (0 = about 10 s worth). Read it out with getStreamData() before it
 * fills; otherwise the newest samples are dropped and the gap is reported. */
HU_API int startStreaming(int bufferSamples);
HU_API int stopStreaming(void);

/* Copies up to maxSamples contiguous samples per channel and returns how many
 * (0 if nothing is waiting). *firstIndex receives the stream index of the
 * first one: sample k was taken at (firstIndex + k) / rate seconds after the
 * start. Data after a gap comes in the next call, with an index that jumps.
 * Still works after stopStreaming() for the samples left in the buffer. */
HU_API int getStreamData(unsigned char *ch1, unsigned char *ch2, int maxSamples, double *firstIndex);

/* Fills status[0..HU_STATUS_LEN-1]:
 *  0 state: 0 idle, 1 streaming, -1 stopped by an error (see huLastError)
 *  1 samples waiting in the ring buffer      2 ring buffer capacity
 *  3 samples delivered by the scope (after the start-up flush)
 *  4 samples dropped because the ring buffer was full   5 number of such gaps
 *  6 seconds of acquisition (host clock)     7 configured sample rate
 *  8 measured sample rate: clearly below [7] means USB could not keep up
 *    and the scope lost data (at the start the value is a bit low)
 *  9 stream index of the next sample getStreamData will return
 * 10 last libusb error code (0 = none)      11 bytes per USB transfer */
HU_API int getQueueStatus(double *status);

/* Text of the last error. */
HU_API const char *huLastError(void);

#ifdef __cplusplus
}
#endif

#endif /* HANTEKUSB_H */
