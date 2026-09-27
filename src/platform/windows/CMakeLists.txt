# Windows-specific platform pieces (Milestone 3, Task 3.7). The
# display/system power monitor is Qt-free Win32: a dedicated
# thread hosting a message-only window with the power-setting +
# suspend/resume notification registrations. Linked only into
# the engine executable — the offline test suites never load it.

add_library(openrgb2_platform_windows STATIC
    WinPowerMonitor.cpp)
target_include_directories(openrgb2_platform_windows PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openrgb2_platform_windows PUBLIC
    openrgb2_engine)
# RegisterPowerSettingNotification / RegisterSuspendResumeNotification.
target_link_libraries(openrgb2_platform_windows PRIVATE
    PowrProf
    user32)
