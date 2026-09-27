# Minimal engine IPC verification driver (Task 3.9). The real UI is
# Milestone 4 — this is the milestone's CLI surface for load-scene /
# set-mode / status-report verification plus the smoke test's
# save/restore path over the backend SDK.

add_executable(openrgb2-ctl
    engine_ctl.cpp)
target_link_libraries(openrgb2-ctl PRIVATE
    openrgb2_ipc
    openrgb2_backend_openrgb)
