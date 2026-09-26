// Build (32-bit, fully static so no MinGW runtime DLLs are needed next to the exe):
//   i686-w64-mingw32-g++ -O2 -s -static -o HantekProxy.exe HantekProxy.cpp
// See build_sdk.sh. HTMarch.dll is loaded at run time from the folder of this exe.
#include <windows.h>
#include <iostream>
#include <fstream>
#include <new>
#include <string>
#include <cstring>
#include <mutex> // For thread safety in logging
std::mutex logMutex;

// --- Define function pointer types with the CRITICAL __stdcall fix ---
typedef short(__stdcall *LPFN_DSOOPENDEVICE)(unsigned short);
typedef unsigned short(__stdcall *LPFN_DSOCHOOSEDEVICE)(unsigned short, short);
typedef short(__stdcall *LPFN_DSOSETTIMEDIV)(unsigned short, int);
typedef short(__stdcall *LPFN_DSOREADHARDDATA_LA)(unsigned short, short*, short*, unsigned long, int);

// Largest nReadLen HTMarch's dsoReadHardData_LA can serve: it always captures
// 1048576 samples and returns them after skipping the first 1024, so larger
// requests make the DLL read past its own buffer.
const unsigned long MAX_READ_LEN = 1047552;

// HTMarch's sample-rate table has 39 entries (nTimeDIV 0..38).
const int MAX_TIME_DIV = 38;

// Result sent back for a request the proxy refuses (unknown command, wrong
// message size, invalid argument), so the client never waits for a reply
// that is not coming.
const short PROXY_BAD_REQUEST = -2;

// Request sizes per command: 4 (command id) + 2 (device index) + arguments.
const DWORD REQUEST_SIZE[] = {
    6,  // 0 dsoOpenDevice
    8,  // 1 dsoChooseDevice:    + short nType
    10, // 2 dsoSetTimeDIV:      + int nTimeDIV
    14, // 3 dsoReadHardData_LA: + unsigned long nReadLen + int nTimeDIV (ignored)
};
const int NUM_COMMANDS = sizeof(REQUEST_SIZE) / sizeof(REQUEST_SIZE[0]);

void Log(const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[64];
    sprintf_s(timestamp, "[%02d:%02d:%02d.%03d] ",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Console output
    std::cout << timestamp << message << std::endl;

    // File output
    static std::ofstream logfile("HantekProxy.log", std::ios::app);
    if (logfile.is_open()) {
        logfile << timestamp << message << std::endl;
        logfile.flush();
    }
}

// Writes one complete message to the pipe.
static bool WriteMessage(HANDLE pipe, const void* data, DWORD size) {
    DWORD written = 0;
    return WriteFile(pipe, data, size, &written, NULL) && written == size;
}

// Loads the HTMarch.dll that sits next to this exe, so the proxy works
// whatever the current folder is; falls back to the normal DLL search.
static HMODULE LoadHantekDll() {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        char* slash = strrchr(path, '\\');
        if (slash && (size_t)(slash + 1 - path) + sizeof("HTMarch.dll") <= MAX_PATH) {
            strcpy(slash + 1, "HTMarch.dll");
            HMODULE h = LoadLibraryA(path);
            if (h) return h;
        }
    }
    return LoadLibraryA("HTMarch.dll");
}

int main() {
    std::cout << "Starting Simplified Hantek Proxy..." << std::endl;

    // Load the 32-bit Hantek DLL
    HMODULE hDso = LoadHantekDll();
    if (!hDso) {
        Log("Fatal Error: Could not load HTMarch.dll (error " + std::to_string(GetLastError()) + ").");
        return 1;
    }
    std::cout << "HTMarch.dll loaded." << std::endl;

    // Get function addresses from the DLL
    LPFN_DSOOPENDEVICE dsoOpenDevice = (LPFN_DSOOPENDEVICE)GetProcAddress(hDso, "dsoOpenDevice");
    LPFN_DSOCHOOSEDEVICE dsoChooseDevice = (LPFN_DSOCHOOSEDEVICE)GetProcAddress(hDso, "dsoChooseDevice");
    LPFN_DSOSETTIMEDIV dsoSetTimeDIV = (LPFN_DSOSETTIMEDIV)GetProcAddress(hDso, "dsoSetTimeDIV");
    LPFN_DSOREADHARDDATA_LA dsoReadHardData_LA = (LPFN_DSOREADHARDDATA_LA)GetProcAddress(hDso, "dsoReadHardData_LA");

    if (!dsoOpenDevice || !dsoChooseDevice || !dsoSetTimeDIV || !dsoReadHardData_LA) {
        Log("Fatal Error: Could not get function addresses from HTMarch.dll.");
        FreeLibrary(hDso);
        return 1;
    }
    std::cout << "Function pointers acquired." << std::endl;

    // Default security descriptor (no NULL DACL): read/write access only for the
    // creating user, administrators and SYSTEM; remote clients are rejected.
    HANDLE pipe = CreateNamedPipe(
        TEXT("\\\\.\\pipe\\HantekPipe"),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,                  // Max instances
        1024 * 1024,        // Out buffer
        1024 * 64,          // In buffer
        30000,              // 30s timeout
        NULL                // Security attributes
    );

    if (pipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            // FILE_FLAG_FIRST_PIPE_INSTANCE: the pipe already exists.
            Log("CreateNamedPipe failed: another HantekProxy.exe is already running. Exiting.");
        } else {
            Log("CreateNamedPipe failed: " + std::to_string(err) + ". Exiting.");
        }
        FreeLibrary(hDso);
        return 1;
    }

    Log("Pipe successfully created: \\\\.\\pipe\\HantekPipe");

    // Main loop: wait for a connection, handle it, then wait for the next
    while (true) {
        std::cout << "Waiting for client connection..." << std::endl;
        if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            Log("ConnectNamedPipe failed: " + std::to_string(GetLastError()));
            DisconnectNamedPipe(pipe);
            Sleep(200); // don't spin if the pipe is in a bad state
            continue;
        }
        std::cout << "Client connected." << std::endl;

        // Loop to handle commands from the connected client
        bool ok = true;
        while (ok) {
            char buffer[1024];
            DWORD bytesRead = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, NULL) || bytesRead == 0) {
                break; // Client disconnected, error, or a message longer than any request
            }

            int commandId = bytesRead >= 4 ? *reinterpret_cast<int*>(buffer) : -1;
            if (commandId < 0 || commandId >= NUM_COMMANDS || bytesRead != REQUEST_SIZE[commandId]) {
                Log("Rejected request: command " + std::to_string(commandId) +
                    ", " + std::to_string(bytesRead) + " bytes");
                short result = PROXY_BAD_REQUEST;
                ok = WriteMessage(pipe, &result, sizeof(result));
                continue;
            }
            unsigned short deviceIndex = *reinterpret_cast<unsigned short*>(buffer + 4);

            switch (commandId) {
                case 0: { // dsoOpenDevice
                    short result = dsoOpenDevice(deviceIndex);
                    ok = WriteMessage(pipe, &result, sizeof(result));
                    break;
                }
                case 1: { // dsoChooseDevice
                    short nType = *reinterpret_cast<short*>(buffer + 6);
                    unsigned short result = dsoChooseDevice(deviceIndex, nType);
                    ok = WriteMessage(pipe, &result, sizeof(result));
                    break;
                }
                case 2: { // dsoSetTimeDIV
                    int timeDiv = *reinterpret_cast<int*>(buffer + 6);
                    // HTMarch checks only nTimeDIV < 39 (signed), so a negative value
                    // would index before its sample-rate table.
                    short result = PROXY_BAD_REQUEST;
                    if (timeDiv >= 0 && timeDiv <= MAX_TIME_DIV) {
                        result = dsoSetTimeDIV(deviceIndex, timeDiv);
                    } else {
                        Log("Rejected dsoSetTimeDIV: nTimeDIV " + std::to_string(timeDiv) +
                            " (allowed 0.." + std::to_string(MAX_TIME_DIV) + ")");
                    }
                    ok = WriteMessage(pipe, &result, sizeof(result));
                    break;
                }
                case 3: { // dsoReadHardData_LA
                    unsigned long nReadLen = *reinterpret_cast<unsigned long*>(buffer + 6);
                    // buffer + 10 holds the client's nTimeDIV, which is deliberately not
                    // forwarded: the last argument of HTMarch's dsoReadHardData_LA is an
                    // index into an internal 8-entry table of capture sizes, and values
                    // >= 8 read past that table (garbage data or a crash). All 8 entries
                    // are equal, so 0 is always correct; the sample rate itself is set
                    // with dsoSetTimeDIV (command 2).
                    if (nReadLen == 0 || nReadLen > MAX_READ_LEN) {
                        Log("Rejected dsoReadHardData_LA: nReadLen " + std::to_string(nReadLen) +
                            " (allowed 1.." + std::to_string(MAX_READ_LEN) + ")");
                        short result = PROXY_BAD_REQUEST;
                        ok = WriteMessage(pipe, &result, sizeof(result));
                        break;
                    }

                    short* pData1 = new (std::nothrow) short[nReadLen];
                    short* pData2 = new (std::nothrow) short[nReadLen];

                    short result = -1;
                    if (pData1 && pData2) {
                        result = dsoReadHardData_LA(deviceIndex, pData1, pData2, nReadLen, 0);
                    }

                    // The sample data follows only a successful capture (result 1).
                    ok = WriteMessage(pipe, &result, sizeof(result));
                    if (ok && result > 0) {
                        ok = WriteMessage(pipe, pData1, nReadLen * sizeof(short)) &&
                             WriteMessage(pipe, pData2, nReadLen * sizeof(short));
                    }

                    delete[] pData1;
                    delete[] pData2;
                    break;
                }
            }
        }
        std::cout << "Client disconnected." << std::endl;
        DisconnectNamedPipe(pipe);
    }

    // Cleanup (though the loop above is infinite)
    CloseHandle(pipe);
    FreeLibrary(hDso);
    return 0;
}
