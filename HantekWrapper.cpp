// Build (64-bit, for MATLAB):
//   x86_64-w64-mingw32-g++ -O2 -s -static -shared -o HantekWrapper.dll HantekWrapper.cpp HantekWrapper.def
// See build_sdk.sh.


#include "HantekWrapper.h"
#include <windows.h>

// Global pipe handle for the persistent connection
HANDLE g_pipe = INVALID_HANDLE_VALUE;

static const wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\HantekPipe";

// Same limit as in HantekProxy.exe: the largest capture HTMarch.dll can return.
static const unsigned long MAX_READ_LEN = 1047552;

// Drops the connection. Called after any pipe error: at that point requests
// and replies may be out of step, and keeping a dead handle would make
// connectToProxy() report "already connected" forever.
static void ResetPipe() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

// Connect to the proxy's named pipe. Retries for about 5 s, so it also works
// right after HantekProxy.exe has been started.
HANTEKWRAPPER_API int __stdcall connectToProxy() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        DWORD available = 0;
        if (PeekNamedPipe(g_pipe, NULL, 0, NULL, &available, NULL)) {
            return 1; // Already connected
        }
        ResetPipe(); // The proxy went away; connect again below
    }
    for (int attempt = 0; attempt < 50 && g_pipe == INVALID_HANDLE_VALUE; ++attempt) {
        g_pipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(PIPE_NAME, 100); // Another client is still connected
            } else {
                Sleep(100); // Proxy not started yet
            }
        }
    }
    if (g_pipe == INVALID_HANDLE_VALUE) {
        return 0; // Failure
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(g_pipe, &mode, NULL, NULL)) {
        ResetPipe();
        return 0;
    }
    return 1; // Success
}

// Disconnect from the pipe
HANTEKWRAPPER_API void __stdcall disconnectFromProxy() {
    ResetPipe();
}

// Writes one complete message.
static bool WriteMessage(const void* data, DWORD size) {
    DWORD bytesWritten = 0;
    return WriteFile(g_pipe, data, size, &bytesWritten, NULL) && bytesWritten == size;
}

// Reads one message that must be exactly `size` bytes long (a longer message
// makes ReadFile fail with ERROR_MORE_DATA).
static bool ReadMessage(void* buffer, DWORD size) {
    DWORD bytesRead = 0;
    return ReadFile(g_pipe, buffer, size, &bytesRead, NULL) && bytesRead == size;
}

// Internal function to send a command and get a simple response
static bool SendAndReceive(const void* sendBuffer, DWORD sendSize, void* responseBuffer, DWORD responseSize) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    if (WriteMessage(sendBuffer, sendSize) && ReadMessage(responseBuffer, responseSize)) return true;
    ResetPipe();
    return false;
}

// --- Wrapper Implementations ---
// Results: whatever HTMarch.dll returns (1 = success, 0 or -1 = failure),
// -1 (0 for dsoChooseDevice) if the proxy could not be reached, and -2 if the
// proxy rejected the request as invalid.

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
    if (!pData1 || !pData2 || nReadLen == 0 || nReadLen > MAX_READ_LEN) return -2;

    // 1. Send command and parameters, read back the result code
    char header[14]; // 4 (cmd), 2 (index), 4 (len), 4 (timeDiv)
    *reinterpret_cast<int*>(header) = 3; // Command 3
    *reinterpret_cast<unsigned short*>(header + 4) = DeviceIndex;
    *reinterpret_cast<unsigned long*>(header + 6) = nReadLen;
    *reinterpret_cast<int*>(header + 10) = nTimeDIV; // ignored by the proxy
    short responseCode = -1;
    if (!SendAndReceive(header, sizeof(header), &responseCode, sizeof(responseCode))) return -1;

    // 2. If the capture succeeded, read the data for both channels
    if (responseCode > 0) {
        DWORD dataSize = nReadLen * sizeof(short);
        if (!ReadMessage(pData1, dataSize) || !ReadMessage(pData2, dataSize)) {
            ResetPipe();
            return -1;
        }
    }
    return responseCode;
}
