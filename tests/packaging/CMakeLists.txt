# Task 6.2 — package lifecycle: the REAL packaging scripts driven
# against staged fake install trees (update preserving user state,
# manifest-verified rollback, shutdown ordering via an injected ctl
# stub, takeover-refuses-silent). Windows-only by nature — the
# scripts and process fixtures are Win32/PowerShell.

if(WIN32)
    add_test(NAME package_launcher_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/launcher_tests.ps1
            -Launcher ${CMAKE_SOURCE_DIR}/packaging/windows/OpenRGB2.cmd)
    set_tests_properties(package_launcher_test PROPERTIES
        LABELS packaging
        TIMEOUT 30)

    add_test(NAME package_gui_launcher_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/gui_launcher_tests.ps1
            -GuiScript ${CMAKE_SOURCE_DIR}/packaging/windows/start-openrgb2-gui.ps1
            -Vbs ${CMAKE_SOURCE_DIR}/packaging/windows/OpenRGB2.vbs)
    set_tests_properties(package_gui_launcher_test PROPERTIES
        LABELS packaging
        TIMEOUT 30)

    add_test(NAME package_desktop_shortcut_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/desktop_shortcut_tests.ps1
            -Installer ${CMAKE_SOURCE_DIR}/packaging/windows/create-desktop-shortcut.ps1)
    set_tests_properties(package_desktop_shortcut_test PROPERTIES
        LABELS packaging
        TIMEOUT 30)

    add_test(NAME package_lifecycle_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/package_lifecycle_tests.ps1
            -ScriptsDir ${CMAKE_SOURCE_DIR}/packaging/windows)
    set_tests_properties(package_lifecycle_test PROPERTIES
        LABELS packaging
        TIMEOUT 300)
endif()
