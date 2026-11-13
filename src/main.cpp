#include "controller.h"
#include "tray.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DgpuTraySwitch");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Passed by the logon task: restore the dGPU state after boot.
    const bool bootMode = cmdLine && wcsstr(cmdLine, L"--boot") != nullptr;

    Controller controller;
    Tray tray;
    if (!tray.Create(instance, &controller) || !controller.Start(tray.hwnd(), bootMode)) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Let an in-flight device change finish so the GPU isn't left half-toggled.
    controller.Shutdown();
    CloseHandle(mutex);
    return 0;
}
