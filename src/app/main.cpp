#include "app/main_window.hpp"

#include <windows.h>

// Purpose: Enter the native Windows GUI process.
// Inputs: `instance` is the process HINSTANCE and `show_command` is the startup show mode; unused Win32 parameters are ignored.
// Outputs: Returns the main window message-loop exit code.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    superzip::app::MainWindow window;
    return window.run(instance, show_command);
}
