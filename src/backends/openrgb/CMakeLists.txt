# OpenRGB SDK client backend (Milestone 3.2). Qt-free like the
# engine: the vendored backend is the sole hardware owner and this
# library only talks loopback TCP to it — never USB/HID/SMBus.

add_library(openrgb2_backend_openrgb STATIC
    SdkWire.cpp
    TcpSocket.cpp
    OpenRgbClient.cpp)

target_include_directories(openrgb2_backend_openrgb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

# Engine boundary types (DeviceKey/DeviceSnapshot/RgbFrame/
# DeliveryStats/IFrameSink) come from the engine library.
target_link_libraries(openrgb2_backend_openrgb PUBLIC
    openrgb2_engine
    ws2_32)
