# Minimal engine IPC verification driver (Task 3.9). The real UI is
# Milestone 4 — this is the milestone's CLI surface for load-scene /
# set-mode / status-report verification plus the smoke test's
# save/restore path over the backend SDK.

# The session + the one-session composite workspace flows (audit
# I-10) — a library so tests/engine/engine_ctl_test drives the same
# code over a real pipe.
add_library(openrgb2_ctl STATIC
    CtlSession.cpp)
target_include_directories(openrgb2_ctl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openrgb2_ctl PUBLIC
    openrgb2_ipc)

add_executable(openrgb2-ctl
    engine_ctl.cpp)
target_link_libraries(openrgb2-ctl PRIVATE
    openrgb2_ctl
    openrgb2_ipc
    openrgb2_backend_openrgb)
