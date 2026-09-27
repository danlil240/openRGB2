# Engine IPC layer (Milestone 3.5): bounded, versioned local named
# pipes (plan section 4) plus a separate read-only diagnostics
# channel. Qt-free like the engine; the only platform dependency is
# the Win32 named-pipe transport — the protocol/session/service core
# is transport-agnostic and tests inject in-process channels.

# Codec + transport only: the wire contract (IpcProtocol) and the
# CLIENT half of the named-pipe transport (NamedPipeChannel +
# ConnectNamedPipeChannel). IPC CLIENTS — e.g. the openrgb2-ui Qt
# process — link this target so the server machinery (IpcServer,
# NamedPipeListener, EngineService) and its engine/backend
# dependency chain NEVER enter their link interface (plan section 4:
# the UI speaks pipe traffic only).
add_library(openrgb2_ipc_wire STATIC
    IpcProtocol.cpp
    NamedPipeClient.cpp)
target_include_directories(openrgb2_ipc_wire PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/json)
target_link_libraries(openrgb2_ipc_wire PUBLIC Threads::Threads)

add_library(openrgb2_ipc STATIC
    IpcServer.cpp
    NamedPipe.cpp
    EngineService.cpp)

target_include_directories(openrgb2_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

# EngineService adapts engine + SDK-client surfaces; the wire target
# carries the codec + client transport it reuses.
target_link_libraries(openrgb2_ipc PUBLIC
    openrgb2_ipc_wire
    openrgb2_engine
    openrgb2_backend_openrgb
    Threads::Threads)

if(WIN32)
    # advapi32: token/SID + SDDL<->descriptor conversion for the
    # restricted pipe DACL (server side only).
    target_link_libraries(openrgb2_ipc PUBLIC advapi32)
endif()
