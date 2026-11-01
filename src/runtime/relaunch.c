/* Starts this program again with the same options, netplay ones replaced
   by `extra` (the Online menu: into a session, or out of one), and waits for
   it: play.ps1 waits for this process, and the new one writes to the same
   log. Returns its exit code. (Its own file: windows.h stays out of main.c.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int relaunch_self(int argc, char** argv, const char* extra) {
    static char cmd[8192];
    size_t n = 0;
    for (int i = 0; i < argc && n < sizeof cmd - 600; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--net-host")) {
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') i++;
            continue;
        }
        if (!strcmp(a, "--net-join") || !strcmp(a, "--net-delay")) { i++; continue; }
        n += (size_t)snprintf(cmd + n, sizeof cmd - n, "%s\"%s\"", n ? " " : "", a);
    }
    if (extra && extra[0]) n += (size_t)snprintf(cmd + n, sizeof cmd - n, " %s", extra);
    printf("netplay: restarting: %s\n", cmd);
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "netplay: couldn't restart the game (error %lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    return system(cmd) == 0 ? 0 : 1;
#endif
}
