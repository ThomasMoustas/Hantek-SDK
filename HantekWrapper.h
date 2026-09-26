#ifndef HANTEK_WRAPPER_H
#define HANTEK_WRAPPER_H

#define HANTEKWRAPPER_API __declspec(dllexport)

#ifdef __cplusplus
extern "C" {
#endif

// --- Connection Management ---
HANTEKWRAPPER_API int __stdcall connectToProxy();
HANTEKWRAPPER_API void __stdcall disconnectFromProxy();

// --- Device Functions ---
HANTEKWRAPPER_API short __stdcall dsoOpenDevice(unsigned short DeviceIndex);
HANTEKWRAPPER_API unsigned short __stdcall dsoChooseDevice(unsigned short DeviceIndex, short nType);
HANTEKWRAPPER_API short __stdcall dsoSetTimeDIV(unsigned short DeviceIndex, int nTimeDIV);
HANTEKWRAPPER_API short __stdcall dsoReadHardData_LA(unsigned short DeviceIndex, short* pData1, short* pData2, unsigned long nReadLen, int nTimeDIV);

// nCH: 0 = CH1, 1 = CH2. nVoltDIV: 0..7 = 20 mV, 50 mV, 100 mV, 200 mV, 500 mV, 1 V, 2 V, 5 V per div
// (hardware gain 10, 10, 10, 5, 2, 1, 1, 1).
HANTEKWRAPPER_API short __stdcall dsoSetVoltDIV(unsigned short DeviceIndex, int nCH, int nVoltDIV);

// Factory zero levels from the scope's EEPROM (raw ADC counts), nLen 1..128.
// level[16*hs + 2*nVoltDIV + ch]: hs = 1 at 48 MSa/s (nTimeDIV 0..10), else 0; ch = 0 (CH1) or 1 (CH2).
HANTEKWRAPPER_API short __stdcall dsoGetCalLevel(unsigned short DeviceIndex, short* level, short nLen);


#ifdef __cplusplus
}
#endif

#endif // HANTEK_WRAPPER_H
