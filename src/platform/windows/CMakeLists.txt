# Windows-specific platform pieces (Milestone 3, Task 3.7). The
# display/system power monitor is Qt-free Win32: a dedicated
# thread hosting a message-only window with the power-setting +
# suspend/resume notification registrations. Linked only into
# the engine executable — the offline test suites never load it.

# Task 4.6 adds the reactive capture sources — Qt-free ports
# of the preserved plugin's inputs (WASAPI loopback audio,
# GDI screen sampler, WH_KEYBOARD_LL key hook). The offline
# suites never load them: tests inject fake IReactiveSource
# implementations instead of capturing the real desktop.
add_library(openrgb2_platform_windows STATIC
    WinPowerMonitor.cpp
    WinReactiveInputs.cpp
    WinAudioLoopback.cpp
    WinScreenSampler.cpp
    WinKeyHook.cpp)
target_include_directories(openrgb2_platform_windows PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openrgb2_platform_windows PUBLIC
    openrgb2_engine)
# RegisterPowerSettingNotification / RegisterSuspendResumeNotification;
# WASAPI loopback (COM); GDI StretchBlt screen grab.
target_link_libraries(openrgb2_platform_windows PRIVATE
    PowrProf
    user32
    gdi32
    ole32)
