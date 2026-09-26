// g++ -m32 -static -o HantekProxy.exe HantekProxy.cpp -L. -lHTMarch -lws2_32
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sddl.h>  // For security descriptors
#include <mutex> // For thread safety in logging
std::mutex logMutex;

// --- Define function pointer types with the CRITICAL __stdcall fix ---
typedef short(__stdcall *LPFN_DSOOPENDEVICE)(unsigned short);
typedef unsigned short(__stdcall *LPFN_DSOCHOOSEDEVICE)(unsigned short, short);
typedef short(__stdcall *LPFN_DSOSETTIMEDIV)(unsigned short, int);
typedef short(__stdcall *LPFN_DSOREADHARDDATA_LA)(unsigned short, short*, short*, unsigned long, int);
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
int main() {
    std::cout << "Starting Simplified Hantek Proxy..." << std::endl;

    // Create security attributes for pipe
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES) };
    SECURITY_DESCRIPTOR sd;

    // Create a NULL DACL for full access
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
        Log("InitializeSecurityDescriptor failed: " + std::to_string(GetLastError()));
        return 1;
    }
    
    if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE)) {
        Log("SetSecurityDescriptorDacl failed: " + std::to_string(GetLastError()));
        return 1;
    }
    
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    // Load the 32-bit Hantek DLL
    HMODULE hDso = LoadLibraryA("HTMarch.dll");
    if (!hDso) {
        std::cout << "Fatal Error: Could not load HTMarch.dll." << std::endl;
        return 1;
    }
    std::cout << "HTMarch.dll loaded." << std::endl;

    // Get function addresses from the DLL
    LPFN_DSOOPENDEVICE dsoOpenDevice = (LPFN_DSOOPENDEVICE)GetProcAddress(hDso, "dsoOpenDevice");
    LPFN_DSOCHOOSEDEVICE dsoChooseDevice = (LPFN_DSOCHOOSEDEVICE)GetProcAddress(hDso, "dsoChooseDevice");
    LPFN_DSOSETTIMEDIV dsoSetTimeDIV = (LPFN_DSOSETTIMEDIV)GetProcAddress(hDso, "dsoSetTimeDIV");
    LPFN_DSOREADHARDDATA_LA dsoReadHardData_LA = (LPFN_DSOREADHARDDATA_LA)GetProcAddress(hDso, "dsoReadHardData_LA");

    if (!dsoOpenDevice || !dsoChooseDevice || !dsoSetTimeDIV || !dsoReadHardData_LA) {
        std::cout << "Fatal Error: Could not get function addresses from DLL." << std::endl;
        FreeLibrary(hDso);
        return 1;
    }
    std::cout << "Function pointers acquired." << std::endl;

    // Check for existing proxy instances
    if (FindWindowA(NULL, "HantekProxy")) {
        Log("Existing proxy window found. Terminating...");
        SendMessage(FindWindowA(NULL, "HantekProxy"), WM_CLOSE, 0, 0);
        Sleep(500);
    }

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
        Log("CreateNamedPipe failed: " + std::to_string(err));
        
        if (err == ERROR_PIPE_BUSY) {
            Log("Another proxy instance may be running. Killing existing instances...");
            system("taskkill /F /IM HantekProxy.exe > nul 2>&1");
        }
    }

    Log("Pipe successfully created: \\\\.\\pipe\\HantekPipe");
    
    // Main loop: wait for a connection, handle it, then wait for the next
    while (true) {
        std::cout << "Waiting for client connection..." << std::endl;
        if (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::cout << "Client connected." << std::endl;
            
            // Loop to handle commands from the connected client
            while (true) {
                char buffer[1024];
                DWORD bytesRead;
                if (!ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, NULL) || bytesRead == 0) {
                    break; // Client disconnected or error, break inner loop
                }

                int commandId = *reinterpret_cast<int*>(buffer);
                unsigned short deviceIndex = *reinterpret_cast<unsigned short*>(buffer + 4);
                DWORD written;

                switch (commandId) {
                    case 0: { // dsoOpenDevice
                        short result = dsoOpenDevice(deviceIndex);
                        WriteFile(pipe, &result, sizeof(result), &written, NULL);
                        break;
                    }
                    case 1: { // dsoChooseDevice
                        short nType = *reinterpret_cast<short*>(buffer + 6);
                        unsigned short result = dsoChooseDevice(deviceIndex, nType);
                        WriteFile(pipe, &result, sizeof(result), &written, NULL);
                        break;
                    }
                    case 2: { // dsoSetTimeDIV
                        int timeDiv = *reinterpret_cast<int*>(buffer + 6);
                        short result = dsoSetTimeDIV(deviceIndex, timeDiv);
                        WriteFile(pipe, &result, sizeof(result), &written, NULL);
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
                        
                        short* pData1 = new short[nReadLen];
                        short* pData2 = new short[nReadLen];
                        
                        short result = dsoReadHardData_LA(deviceIndex, pData1, pData2, nReadLen, 0);
                        
                        WriteFile(pipe, &result, sizeof(result), &written, NULL);
                        if (result != -1) {
                            WriteFile(pipe, pData1, nReadLen * sizeof(short), &written, NULL);
                            WriteFile(pipe, pData2, nReadLen * sizeof(short), &written, NULL);
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
    }

    // Cleanup (though the loop above is infinite)
    CloseHandle(pipe);
    FreeLibrary(hDso);
    return 0;
}