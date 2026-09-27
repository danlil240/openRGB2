# Task 6.2 — package lifecycle: the REAL packaging scripts driven
# against staged fake install trees (update preserving user state,
# manifest-verified rollback, shutdown ordering via an injected ctl
# stub, takeover-refuses-silent). Windows-only by nature — the
# scripts and process fixtures are Win32/PowerShell.

if(WIN32)
    add_test(NAME package_lifecycle_test
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/package_lifecycle_tests.ps1
            -ScriptsDir ${CMAKE_SOURCE_DIR}/packaging/windows)
    set_tests_properties(package_lifecycle_test PROPERTIES
        LABELS packaging
        TIMEOUT 300)
endif()
