# Build-flag configurations behind the CMakePresets.json presets
# (audit P-13 step 5). With both options OFF -- the `default` preset and
# every plain configure -- the flags are exactly the historical ones:
# /W3 on C/C++, nothing else.
#
#   OPENRGB2_WARNINGS_W4  (`w4` preset)
#       /W4 /permissive- on FIRST-PARTY targets only, applied through
#       openrgb2_warnings(<target>) (openrgb2_warnings_for_dirs() runs it
#       over every compiled target of the first-party directories).
#       Vendor sources compiled into such a target (tinyuz, HDiffPatch,
#       the Lian Li controller files, ...) are pinned back to /W3 and
#       the default conformance mode per source file. Headers included
#       with <...> (Qt, nlohmann/json) and headers under vendor/ are
#       treated as external at /external:W0. Warnings stay warnings: no
#       /WX until the /W4 backlog is at zero.
#
#   OPENRGB2_ASAN  (`asan` preset)
#       /fsanitize=address with /Zi on every C/C++ target, incremental
#       linking off (ASan requires it), meant for RelWithDebInfo (no
#       /RTC, which ASan rejects). Every registered test gets the ASan
#       runtime directory (the one cl.exe lives in) prepended to PATH,
#       so a plain `ctest` in that tree finds
#       clang_rt.asan_dynamic-x86_64.dll.

option(OPENRGB2_WARNINGS_W4
    "First-party targets at /W4 /permissive- (vendor sources stay /W3)" OFF)
option(OPENRGB2_ASAN
    "Build with AddressSanitizer (/fsanitize=address); use RelWithDebInfo" OFF)

if(MSVC)
    # One warning level per target: /W4 where openrgb2_warnings() set
    # OPENRGB2_W4 on the target, else today's /W3. Only C/C++ -- the RC
    # compiler does not take /W.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:$<IF:$<BOOL:$<TARGET_PROPERTY:OPENRGB2_W4>>,/W4,/W3>>")
endif()

if(OPENRGB2_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "OPENRGB2_ASAN is wired for MSVC only")
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.28)
        # set_property(TEST ... DIRECTORY) in openrgb2_asan_test_env().
        message(FATAL_ERROR "OPENRGB2_ASAN needs CMake >= 3.28")
    endif()
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address;/Zi>")
    foreach(kind EXE SHARED MODULE)
        foreach(cfg DEBUG RELWITHDEBINFO)
            string(REPLACE "/INCREMENTAL" "/INCREMENTAL:NO"
                CMAKE_${kind}_LINKER_FLAGS_${cfg}
                "${CMAKE_${kind}_LINKER_FLAGS_${cfg}}")
            string(REPLACE "/INCREMENTAL:NO:NO" "/INCREMENTAL:NO"
                CMAKE_${kind}_LINKER_FLAGS_${cfg}
                "${CMAKE_${kind}_LINKER_FLAGS_${cfg}}")
        endforeach()
    endforeach()
    # /RTC* (CMake's Debug default) is incompatible with ASan.
    foreach(lang C CXX)
        string(REGEX REPLACE "/RTC[1csu]+" ""
            CMAKE_${lang}_FLAGS_DEBUG "${CMAKE_${lang}_FLAGS_DEBUG}")
    endforeach()
    get_filename_component(OPENRGB2_ASAN_RUNTIME_DIR
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(NOT EXISTS "${OPENRGB2_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        message(WARNING
            "OPENRGB2_ASAN: clang_rt.asan_dynamic-x86_64.dll not found next to "
            "${CMAKE_CXX_COMPILER} -- tests will not find the ASan runtime")
    endif()
    message(STATUS "OPENRGB2_ASAN=ON: runtime dir ${OPENRGB2_ASAN_RUNTIME_DIR}")
endif()

# Vendor tree root: sources and headers under it are third-party.
set(OPENRGB2_VENDOR_DIR "${CMAKE_SOURCE_DIR}/vendor")

# openrgb2_warnings(<target>)
#   Opts one first-party target into the /W4 configuration. A no-op
#   unless OPENRGB2_WARNINGS_W4 is ON, so the default build is
#   unchanged.
function(openrgb2_warnings target)
    if(NOT MSVC OR NOT OPENRGB2_WARNINGS_W4)
        return()
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
        return()
    endif()
    set_property(TARGET ${target} PROPERTY OPENRGB2_W4 ON)
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/external:anglebrackets;/external:W0;/external:I${OPENRGB2_VENDOR_DIR}>")

    # Vendor sources compiled into this target keep today's level.
    get_target_property(src_dir ${target} SOURCE_DIR)
    get_target_property(sources ${target} SOURCES)
    if(NOT sources)
        return()
    endif()
    foreach(src IN LISTS sources)
        if(src MATCHES "^\\$<")
            continue()                     # generator expression
        endif()
        get_filename_component(abs "${src}" ABSOLUTE BASE_DIR "${src_dir}")
        string(FIND "${abs}" "${OPENRGB2_VENDOR_DIR}/" at)
        if(NOT at EQUAL 0)
            continue()
        endif()
        if(abs MATCHES "\\.c$")
            set(opts "/W3")
        else()
            set(opts "/W3;/permissive")
        endif()
        set_source_files_properties("${abs}" TARGET_DIRECTORY ${target}
            PROPERTIES COMPILE_OPTIONS "${opts}")
    endforeach()
endfunction()

# openrgb2_warnings_for_dirs(<dir>...)
#   Runs openrgb2_warnings() over every compiled target defined in the
#   given source directories and their subdirectories. Called once,
#   after every add_subdirectory(), from the top-level CMakeLists.txt.
function(openrgb2_warnings_for_dirs)
    if(NOT MSVC OR NOT OPENRGB2_WARNINGS_W4)
        return()
    endif()
    set(count 0)
    set(pending ${ARGN})
    while(pending)
        list(POP_FRONT pending dir)
        get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(t IN LISTS targets)
            get_target_property(type ${t} TYPE)
            if(type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
                openrgb2_warnings(${t})
                math(EXPR count "${count} + 1")
            endif()
        endforeach()
        get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        list(APPEND pending ${subdirs})
    endwhile()
    message(STATUS "OPENRGB2_WARNINGS_W4=ON: /W4 /permissive- on ${count} first-party targets")
endfunction()

# openrgb2_asan_test_env(<dir>...)
#   Prepends the ASan runtime directory to PATH for every test
#   registered in the given directories (and below). No-op unless
#   OPENRGB2_ASAN is ON.
function(openrgb2_asan_test_env)
    if(NOT OPENRGB2_ASAN)
        return()
    endif()
    file(TO_NATIVE_PATH "${OPENRGB2_ASAN_RUNTIME_DIR}" native_dir)
    set(pending ${ARGN})
    while(pending)
        list(POP_FRONT pending dir)
        get_property(tests DIRECTORY "${dir}" PROPERTY TESTS)
        foreach(t IN LISTS tests)
            set_property(TEST ${t} DIRECTORY "${dir}" APPEND PROPERTY
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${native_dir}")
        endforeach()
        get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        list(APPEND pending ${subdirs})
    endwhile()
endfunction()
