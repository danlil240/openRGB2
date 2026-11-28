# R-9 supervision helpers shared by openrgb2-engine and
# openrgb2-ui. Qt-free AND engine-free: the UI links this
# without carrying the engine/SDK boundary (plan section 4).
#
#   CrashDump      in-process minidumps (dbghelp) into
#                  <install>\state\crash, newest 5 per exe
#   RestartLedger  header-only restart budget + backoff, the
#                  same bookkeeping for the UI's engine
#                  supervisor and the engine's backend
#                  supervisor

add_library(openrgb2_supervise STATIC
    CrashDump.cpp
    CrashDump.h
    RestartLedger.h)
target_include_directories(openrgb2_supervise PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(openrgb2_supervise PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(openrgb2_supervise PRIVATE dbghelp)
endif()
