// End-to-end test of HantekWrapper.dll + HantekProxy.exe over the real named
// pipe, with mock_htmarch.c standing in for HTMarch.dll. It starts and kills
// HantekProxy.exe itself. Build and run with run_sdk_tests.sh (Linux + Wine),
// or build the same way with MinGW and run test_sdk.exe on Windows.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../HantekWrapper.h"

static int g_failures = 0;

#define CHECK(cond) do { \
        if (cond) { printf("ok    %s\n", #cond); } \
        else { printf("FAIL  %s  (line %d)\n", #cond, __LINE__); g_failures++; } \
    } while (0)

static const wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\HantekPipe";

static PROCESS_INFORMATION StartProxy() {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    char cmd[] = "HantekProxy.exe";
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL  could not start HantekProxy.exe (%lu)\n", GetLastError());
        g_failures++;
    }
    return pi;
}

static void StopProxy(PROCESS_INFORMATION& pi) {
    if (!pi.hProcess) return;
    TerminateProcess(pi.hProcess, 9);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    pi.hProcess = NULL;
}

// Opens the pipe directly (no wrapper), retrying while the proxy is busy.
static HANDLE RawConnect() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE h = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, NULL, NULL);
            return h;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(PIPE_NAME, 100);
        else Sleep(100);
    }
    return INVALID_HANDLE_VALUE;
}

// Sends one raw request and returns the 2-byte reply (or 12345 on I/O error).
static short RawRequest(HANDLE h, const void* msg, DWORD size) {
    DWORD n = 0;
    short reply = 12345;
    if (!WriteFile(h, msg, size, &n, NULL) || n != size) return 12345;
    if (!ReadFile(h, &reply, sizeof(reply), &n, NULL) || n != sizeof(reply)) return 12345;
    return reply;
}

static bool PatternOk(const std::vector<short>& a, const std::vector<short>& b,
                      unsigned long n, int timeDiv) {
    for (unsigned long i = 0; i < n; ++i) {
        if (a[i] != (short)((i + timeDiv) & 0xff) || b[i] != (short)(255 - (i & 0xff))) return false;
    }
    return true;
}

int main() {
    const unsigned long MAX_LEN = 1047552;
    std::vector<short> a(MAX_LEN + 1), b(MAX_LEN + 1);

    PROCESS_INFORMATION proxy = StartProxy();

    printf("-- basic calls\n");
    CHECK(connectToProxy() == 1);
    CHECK(connectToProxy() == 1); // already connected
    CHECK(dsoOpenDevice(0) == 1);
    CHECK(dsoOpenDevice(1) == 0);
    CHECK(dsoChooseDevice(0, 1) == 1);
    CHECK(dsoSetTimeDIV(0, 13) == 1);
    CHECK(dsoSetTimeDIV(0, 39) == -2); // out of range, rejected by the proxy
    CHECK(dsoSetTimeDIV(0, -1) == -2); // would index before the DLL's table

    printf("-- captures\n");
    // nTimeDIV 13 as last argument: the mock aborts if the proxy forwards it.
    CHECK(dsoReadHardData_LA(0, a.data(), b.data(), 1000, 13) == 1);
    CHECK(PatternOk(a, b, 1000, 13));
    // Largest capture: 2 messages of 2 MB through a 1 MB pipe buffer.
    CHECK(dsoReadHardData_LA(0, a.data(), b.data(), MAX_LEN, 0) == 1);
    CHECK(PatternOk(a, b, MAX_LEN, 13));
    CHECK(dsoReadHardData_LA(0, a.data(), b.data(), MAX_LEN + 1, 0) == -2);
    CHECK(dsoReadHardData_LA(0, a.data(), b.data(), 0, 0) == -2);
    CHECK(dsoReadHardData_LA(0, NULL, b.data(), 10, 0) == -2);
    // A failed capture sends no data; the next request must still line up.
    CHECK(dsoReadHardData_LA(1, a.data(), b.data(), 10, 0) == -1);
    CHECK(dsoOpenDevice(0) == 1);

    printf("-- volt/div and calibration\n");
    CHECK(dsoSetVoltDIV(0, 0, 5) == 1);
    CHECK(dsoSetVoltDIV(0, 1, 7) == 1);
    CHECK(dsoSetVoltDIV(0, 2, 5) == -2);  // no channel 2
    CHECK(dsoSetVoltDIV(0, 0, 8) == -2);
    CHECK(dsoSetVoltDIV(0, 0, -1) == -2); // would index before the DLL's table
    CHECK(dsoSetVoltDIV(1, 0, 5) == 0);   // DLL failure
    short cal[129];
    CHECK(dsoGetCalLevel(0, cal, 32) == 1);
    bool calOk = true;
    for (int i = 0; i < 32; ++i) calOk = calOk && cal[i] == 100 + i;
    CHECK(calOk);
    CHECK(dsoGetCalLevel(0, cal, 128) == 1 && cal[127] == 227);
    CHECK(dsoGetCalLevel(0, cal, 129) == -2);
    CHECK(dsoGetCalLevel(0, cal, 0) == -2);
    CHECK(dsoGetCalLevel(1, cal, 32) == 0); // DLL failure, no data follows...
    CHECK(dsoOpenDevice(0) == 1);           // ...and the pipe is still in step

    printf("-- invalid requests sent straight to the pipe\n");
    disconnectFromProxy();
    HANDLE raw = RawConnect();
    CHECK(raw != INVALID_HANDLE_VALUE);
    if (raw != INVALID_HANDLE_VALUE) {
        char msg[14];
        memset(msg, 0, sizeof(msg));
        *reinterpret_cast<int*>(msg) = 3;
        *reinterpret_cast<unsigned long*>(msg + 6) = 2000000;   // too long
        CHECK(RawRequest(raw, msg, 14) == -2);
        *reinterpret_cast<unsigned long*>(msg + 6) = 16;
        *reinterpret_cast<int*>(msg + 10) = 99;                 // ignored, must not reach the DLL
        short r = RawRequest(raw, msg, 14);
        CHECK(r == 1);
        if (r == 1) {                                           // drain the two data messages
            DWORD n;
            ReadFile(raw, a.data(), 32, &n, NULL);
            ReadFile(raw, b.data(), 32, &n, NULL);
        }
        *reinterpret_cast<int*>(msg) = 5;
        *reinterpret_cast<short*>(msg + 6) = 200;               // more calibration levels than exist
        CHECK(RawRequest(raw, msg, 8) == -2);
        *reinterpret_cast<int*>(msg) = 99;                      // unknown command
        CHECK(RawRequest(raw, msg, 6) == -2);
        CHECK(RawRequest(raw, msg, 3) == -2);                   // shorter than a header
        *reinterpret_cast<int*>(msg) = 0;
        CHECK(RawRequest(raw, msg, 7) == -2);                   // wrong size for command 0
        CHECK(RawRequest(raw, msg, 6) == 1);                    // still in step
        CloseHandle(raw);
    }

    printf("-- proxy restart\n");
    CHECK(connectToProxy() == 1);
    StopProxy(proxy);
    CHECK(dsoOpenDevice(0) == -1);  // pipe broken
    proxy = StartProxy();
    CHECK(connectToProxy() == 1);   // reconnects instead of "already connected"
    CHECK(dsoOpenDevice(0) == 1);

    printf("-- second proxy instance\n");
    PROCESS_INFORMATION second = StartProxy();
    DWORD code = 0;
    if (second.hProcess) {
        WaitForSingleObject(second.hProcess, 10000);
        GetExitCodeProcess(second.hProcess, &code);
    }
    CHECK(code == 1);               // exits instead of killing the first one
    StopProxy(second);
    CHECK(dsoOpenDevice(0) == 1);   // first proxy unaffected

    disconnectFromProxy();
    StopProxy(proxy);

    printf(g_failures ? "\n%d CHECK(S) FAILED\n" : "\nALL SDK TESTS PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
