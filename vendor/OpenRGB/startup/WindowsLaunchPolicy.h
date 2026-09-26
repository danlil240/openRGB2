#pragma once

enum class WindowsLaunchAction { Start, ConnectGui, Restart };

inline WindowsLaunchAction GetWindowsLaunchAction(bool other_processes, bool server_running,
                                                 bool gui_running, bool gui_requested)
{
    if(other_processes && server_running && !gui_running && gui_requested)
        return WindowsLaunchAction::ConnectGui;
    return (other_processes || server_running) ? WindowsLaunchAction::Restart : WindowsLaunchAction::Start;
}
