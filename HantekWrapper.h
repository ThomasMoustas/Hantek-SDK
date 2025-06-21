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


#ifdef __cplusplus
}
#endif

#endif // HANTEK_WRAPPER_H
