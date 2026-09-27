# Engine IPC layer (Milestone 3.5): bounded, versioned local named
# pipes (plan section 4) plus a separate read-only diagnostics
# channel. Qt-free like the engine; the only platform dependency is
# the Win32 named-pipe transport — the protocol/session/service core
# is transport-agnostic and tests inject in-process channels.

add_library(openrgb2_ipc STATIC
    IpcProtocol.cpp
    IpcServer.cpp
    NamedPipe.cpp
    EngineService.cpp)

target_include_directories(openrgb2_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

# EngineService adapts engine + SDK-client surfaces; the engine
# library already carries the nlohmann/json include dir.
target_link_libraries(openrgb2_ipc PUBLIC
    openrgb2_engine
    openrgb2_backend_openrgb
    Threads::Threads)

if(WIN32)
    # advapi32: token/SID + SDDL<->descriptor conversion for the
    # restricted pipe DACL.
    target_link_libraries(openrgb2_ipc PUBLIC advapi32)
endif()
