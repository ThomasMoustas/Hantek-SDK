// $ g++ -m64 -shared -DBUILDING_HANTEK_DLL -o HantekWrapper.dll HantekWrapper.cpp     -Wl,--add-stdcall-alias     -Wl,--enable-auto-import     -lws2_32


#include "HantekWrapper.h"
#include <windows.h>
#include <iostream>

// Global pipe handle for the persistent connection
HANDLE g_pipe = INVALID_HANDLE_VALUE;

// Connect to the proxy's named pipe
HANTEKWRAPPER_API int __stdcall connectToProxy() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        return 1; // Already connected
    }
    g_pipe = CreateFileW(L"\\\\.\\pipe\\HantekPipe", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        return 0; // Failure
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(g_pipe, &mode, NULL, NULL);
    return 1; // Success
}

// Disconnect from the pipe
HANTEKWRAPPER_API void __stdcall disconnectFromProxy() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

// Internal function to send a command and get a simple response
bool SendAndReceive(void* sendBuffer, size_t sendSize, void* responseBuffer, size_t responseSize) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    DWORD bytesWritten, bytesRead;
    if (!WriteFile(g_pipe, sendBuffer, sendSize, &bytesWritten, NULL)) return false;
    if (!ReadFile(g_pipe, responseBuffer, responseSize, &bytesRead, NULL)) return false;
    return (bytesWritten == sendSize && bytesRead == responseSize);
}

// --- Wrapper Implementations ---

HANTEKWRAPPER_API short __stdcall dsoOpenDevice(unsigned short DeviceIndex) {
    char buffer[6]; // 4 bytes for command ID, 2 for device index
    *reinterpret_cast<int*>(buffer) = 0; // Command 0
    *reinterpret_cast<unsigned short*>(buffer + 4) = DeviceIndex;
    short response = -1;
    if (!SendAndReceive(buffer, sizeof(buffer), &response, sizeof(response))) return -1;
    return response;
}

HANTEKWRAPPER_API unsigned short __stdcall dsoChooseDevice(unsigned short DeviceIndex, short nType) {
    char buffer[8]; // 4 (cmd), 2 (index), 2 (type)
    *reinterpret_cast<int*>(buffer) = 1; // Command 1
    *reinterpret_cast<unsigned short*>(buffer + 4) = DeviceIndex;
    *reinterpret_cast<short*>(buffer + 6) = nType;
    unsigned short response = 0;
    if (!SendAndReceive(buffer, sizeof(buffer), &response, sizeof(response))) return 0;
    return response;
}

HANTEKWRAPPER_API short __stdcall dsoSetTimeDIV(unsigned short DeviceIndex, int nTimeDIV) {
    char buffer[10]; // 4 (cmd), 2 (index), 4 (timeDiv)
    *reinterpret_cast<int*>(buffer) = 2; // Command 2
    *reinterpret_cast<unsigned short*>(buffer + 4) = DeviceIndex;
    *reinterpret_cast<int*>(buffer + 6) = nTimeDIV;
    short response = -1;
    if (!SendAndReceive(buffer, sizeof(buffer), &response, sizeof(response))) return -1;
    return response;
}

HANTEKWRAPPER_API short __stdcall dsoReadHardData_LA(unsigned short DeviceIndex, short* pData1, short* pData2, unsigned long nReadLen, int nTimeDIV) {
    if (g_pipe == INVALID_HANDLE_VALUE) return -1;

    // 1. Send command and parameters
    char header[14]; // 4 (cmd), 2 (index), 4 (len), 4 (timeDiv)
    *reinterpret_cast<int*>(header) = 3; // Command 3
    *reinterpret_cast<unsigned short*>(header + 4) = DeviceIndex;
    *reinterpret_cast<unsigned long*>(header + 6) = nReadLen;
    *reinterpret_cast<int*>(header + 10) = nTimeDIV;
    
    DWORD bytesWritten;
    if (!WriteFile(g_pipe, header, sizeof(header), &bytesWritten, NULL)) return -1;

    // 2. Read back the result code
    short responseCode = -1;
    DWORD bytesRead;
    if (!ReadFile(g_pipe, &responseCode, sizeof(responseCode), &bytesRead, NULL)) return -1;
    
    // 3. If successful, read the data for both channels
    if (responseCode != -1) {
        DWORD dataSize = nReadLen * sizeof(short);
        if (!ReadFile(g_pipe, pData1, dataSize, &bytesRead, NULL) || bytesRead != dataSize) return -1;
        if (!ReadFile(g_pipe, pData2, dataSize, &bytesRead, NULL) || bytesRead != dataSize) return -1;
    }
    return responseCode;
}
