/*
 * Stand-in for the Hantek SDK (HTMarch.dll), used only by the SDK-path tests.
 * Same exports and return conventions as the real DLL (1 = success, 0 or -1 =
 * failure), synthetic data instead of a scope. Arguments that make the real
 * DLL read out of bounds abort() here, so a test fails loudly if the proxy
 * ever forwards one.
 */
#include <stdlib.h>

#define EXPORT __declspec(dllexport)
#define MAX_READ_LEN 1047552

static int g_time_div = 0;

EXPORT short __stdcall dsoOpenDevice(unsigned short DeviceIndex)
{
	return DeviceIndex == 0 ? 1 : 0;
}

EXPORT unsigned short __stdcall dsoChooseDevice(unsigned short DeviceIndex, short nType)
{
	(void)nType;
	return DeviceIndex == 0 ? 1 : 0;
}

EXPORT short __stdcall dsoSetTimeDIV(unsigned short DeviceIndex, int nTimeDIV)
{
	if (nTimeDIV < 0)
		abort(); /* real DLL: indexes before its sample-rate table */
	if (DeviceIndex != 0 || nTimeDIV >= 39)
		return 0;
	g_time_div = nTimeDIV;
	return 1;
}

EXPORT short __stdcall dsoReadHardData_LA(unsigned short DeviceIndex, short *pData1,
		short *pData2, unsigned long nReadLen, int nSizeIndex)
{
	unsigned long i;

	if (nReadLen > MAX_READ_LEN)
		abort(); /* real DLL: copies past its capture buffer */
	if (nSizeIndex < 0 || nSizeIndex >= 8)
		abort(); /* real DLL: indexes past its 8-entry size table */
	if (DeviceIndex != 0)
		return -1;
	for (i = 0; i < nReadLen; i++) {
		pData1[i] = (short)((i + g_time_div) & 0xff);
		pData2[i] = (short)(255 - (i & 0xff));
	}
	return 1;
}

EXPORT short __stdcall dsoSetVoltDIV(unsigned short DeviceIndex, int nCH, int nVoltDIV)
{
	(void)nCH;
	if (nVoltDIV < 0)
		abort(); /* real DLL: indexes before its gain table */
	if (DeviceIndex != 0 || nVoltDIV >= 8)
		return 0;
	return 1;
}

EXPORT short __stdcall dsoGetCalLevel(unsigned short DeviceIndex, short *level, short nLen)
{
	short i;

	if (nLen > 128)
		abort(); /* real DLL: copies past its 128-byte buffer */
	if (DeviceIndex != 0)
		return 0;
	for (i = 0; i < nLen; i++)
		level[i] = (short)(100 + i);
	return 1;
}
