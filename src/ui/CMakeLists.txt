# openrgb2-ui — the standalone Qt Quick application process
# (plan section 4: the UI is a separate process; closing it
# never stops engine playback, and re-attaching resumes state
# via WELCOME + SUBSCRIBE initial pushes).
#
# All QML and the C++/QML IPC bridge live in the OpenRGB2.UI
# module built by ui/CMakeLists.txt — this file is only the
# entry point.

qt_add_executable(openrgb2-ui WIN32
    main.cpp
    icons.qrc
    ${CMAKE_SOURCE_DIR}/packaging/windows/OpenRGB2.rc)
set_target_properties(openrgb2-ui PROPERTIES AUTORCC ON)

target_link_libraries(openrgb2-ui PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Network)

if(WIN32)
    target_link_libraries(openrgb2-ui PRIVATE shell32 advapi32 dwmapi)
endif()

qt_import_qml_plugins(openrgb2-ui)
