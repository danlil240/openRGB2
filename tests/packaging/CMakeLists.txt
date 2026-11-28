# Task 6.2 — package lifecycle: the REAL packaging scripts driven
# against staged fake install trees (update preserving user state,
# manifest-verified rollback, shutdown ordering via an injected ctl
# stub, takeover-refuses-silent). Windows-only by nature — the
# scripts and process fixtures are Win32/PowerShell.

if(WIN32)
    add_test(NAME package_startup_task_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/startup_task_tests.ps1
            -Script ${CMAKE_SOURCE_DIR}/packaging/windows/startup-task.ps1)
    set_tests_properties(package_startup_task_test PROPERTIES
        LABELS packaging
        TIMEOUT 30)

    # Audit P-14: the staged manifest's backend pin comes from
    # vendor/provenance/MANIFEST.md, not a string literal.
    add_test(NAME package_stage_pin_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/stage_release_pin_tests.ps1
            -StageScript ${CMAKE_SOURCE_DIR}/packaging/windows/stage-release.ps1
            -RepoRoot ${CMAKE_SOURCE_DIR})
    set_tests_properties(package_stage_pin_test PROPERTIES
        LABELS packaging
        TIMEOUT 30)

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

    # Audit P-2: the REAL stage-release.ps1 against fake inputs and a
    # fake windeployqt -- re-staging keeps a run-in-place package's
    # state\ and profiles\local\, and never wipes a non-package dir.
    add_executable(fake_windeployqt fake_windeployqt.cpp)
    target_compile_features(fake_windeployqt PRIVATE cxx_std_17)
    add_test(NAME package_stage_release_state_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/stage_release_state_tests.ps1
            -StageScript ${CMAKE_SOURCE_DIR}/packaging/windows/stage-release.ps1
            -FakeWindeployqt $<TARGET_FILE:fake_windeployqt>)
    set_tests_properties(package_stage_release_state_test PROPERTIES
        LABELS packaging
        TIMEOUT 300)

    add_test(NAME package_lifecycle_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/package_lifecycle_tests.ps1
            -ScriptsDir ${CMAKE_SOURCE_DIR}/packaging/windows)
    set_tests_properties(package_lifecycle_test PROPERTIES
        LABELS packaging
        TIMEOUT 300)

    # Audit P-14: stage-release.ps1 -VerifyOnly hashes the staged
    # tree against its manifest.json (changed / missing / unlisted).
    add_test(NAME package_stage_verify_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/stage_release_verify_tests.ps1
            -StageScript ${CMAKE_SOURCE_DIR}/packaging/windows/stage-release.ps1)
    set_tests_properties(package_stage_verify_test PROPERTIES
        LABELS packaging
        TIMEOUT 120)

    # Audit P-13: the REAL stage-release.ps1 end to end -- this build
    # tree's exes, the build's Qt kit (real windeployqt) and the
    # pinned backend into a temp -OutDir; manifest, layout,
    # windeployqt output and -VerifyOnly. Needs the UI and a built
    # backend (OPENRGB2_BACKEND_DIR or vendor\OpenRGB); without the
    # backend, or in a Debug build, it exits 77 = skipped.
    if(OPENRGB2_BUILD_UI)
        set(_stage_qt_dir "${QT6_INSTALL_PREFIX}")
        if(NOT _stage_qt_dir)
            get_filename_component(_stage_qt_dir "${Qt6_DIR}/../../.." ABSOLUTE)
        endif()
        add_test(NAME package_stage_release_test
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/stage_release_real_tests.ps1
                -StageScript ${CMAKE_SOURCE_DIR}/packaging/windows/stage-release.ps1
                -BuildDir ${CMAKE_BINARY_DIR}
                -Configuration $<CONFIG>
                -QtDir ${_stage_qt_dir})
        set_tests_properties(package_stage_release_test PROPERTIES
            LABELS packaging
            SKIP_RETURN_CODE 77
            TIMEOUT 900)
    endif()
endif()
