# SR Weaver — system-tray app that weaves stereo 3D for Simulated Reality displays
# (Samsung Odyssey 3D, Acer SpatialLabs, etc.) via the LeiaSR / Dimenco weaver.
#
# Supports both architectures:
#   x64  -> links the LeiaSR 1.36.2 win64 SDK   (target srDirectX::srDirectX)
#   x86  -> links the SR 1.34.10 win32 SDK       (target srDirectX32::srDirectX)
# Both expose the modern IDX11Weaver1 / CreateDX11Weaver API (runtime >= 1.34),
# which supports windowed AND fullscreen weaving.
#
# Configure 32-bit (verified working against the installed 32-bit runtime):
#   cmake -B build/x86 -A Win32
# Configure 64-bit (needs the 64-bit SR runtime DLLs present at run time):
#   cmake -B build/x64 -A x64

cmake_minimum_required(VERSION 3.21)
project(SRLoom CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static-link the MSVC runtime so the shipped exe needs no VC++ redistributable —
# the release is then a single self-contained SRLoom.exe (the SR Platform runtime,
# which the user already has for the display, is the only external dependency).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Locate the Simulated Reality SDK appropriate for the target architecture ---
set(SR_SDK_DIR_X64 "${CMAKE_SOURCE_DIR}/lib/Simulated Reality/LeiaSR-SDK-1.36.2-win64"
    CACHE PATH "64-bit LeiaSR SDK root")
set(SR_SDK_DIR_X86 "${CMAKE_SOURCE_DIR}/lib/Simulated Reality/simulatedreality-1.34.10-win32-Release"
    CACHE PATH "32-bit SR SDK root")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SR_SDK_DIR "${SR_SDK_DIR_X64}")
    set(SR_THIRD_PARTY_LIBS
        "${SR_SDK_DIR}/third_party/GLog/lib/x64/glog.lib"
        "${SR_SDK_DIR}/third_party/OpenCV/lib/x64/opencv_world343.lib"
        "${SR_SDK_DIR}/third_party/Leap/lib/LeapC.lib")
else()
    set(SR_SDK_DIR "${SR_SDK_DIR_X86}")
    set(SR_THIRD_PARTY_LIBS
        "${SR_SDK_DIR}/third_party/GLog/lib/x86/glog.lib"
        "${SR_SDK_DIR}/third_party/OpenCV/lib/x86/opencv_world343.lib")
endif()
message(STATUS "SR Weaver: using SR SDK at ${SR_SDK_DIR}")

# The SDK's own CMake package hard-references runtime DLLs that ship with the
# installed SR Platform (not the SDK folder), so find_package() fails its
# file-existence check. We link the import libraries directly instead — this is
# identical for the win32 and win64 SDKs. Unused import libs add no DLL
# dependency, so over-linking the full set is safe.
file(GLOB SR_IMPORT_LIBS "${SR_SDK_DIR}/lib/*.lib")
if(NOT SR_IMPORT_LIBS)
    message(FATAL_ERROR "No SR import libraries found in ${SR_SDK_DIR}/lib")
endif()

# The SR public headers transitively include third-party headers (asio,
# OpenCV, GLog, Leap, rapidxml). The win32 SDK ships only some of them, so we
# always pull the complete, arch-neutral set from the win64 SDK.
set(SR_INCLUDE_DIRS
    "${SR_SDK_DIR}/include"
    "${SR_SDK_DIR_X64}/third_party/asio/include"
    "${SR_SDK_DIR_X64}/third_party/rapidxml"
    "${SR_SDK_DIR_X64}/third_party/GLog/include"
    "${SR_SDK_DIR_X64}/third_party/Leap/include"
    "${SR_SDK_DIR_X64}/third_party/OpenCV/include"
)

# Dear ImGui (MIT) for the control GUI window — vendored in third_party/imgui.
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)

add_executable(SRLoom WIN32
    src/main.cpp
    src/Renderer.cpp
    src/SRWeaver.cpp
    src/TrayIcon.cpp
    src/Capture.cpp
    src/CaptureDXGI.cpp
    src/Converter.cpp
    src/Detector.cpp
    src/Gui.cpp
    src/AcerSpatialLabs.cpp
    src/Settings.cpp
    src/VideoSource.cpp
    src/UpdateChecker.cpp
    src/UpdateChecker.h
    src/KatangaSource.cpp
    src/KatangaSource.h
    src/LFPReader.cpp
    src/LFPReader.h
    src/LFPRenderer.cpp
    src/LFPRenderer.h
    src/Common.h
    src/Renderer.h
    src/SRWeaver.h
    src/TrayIcon.h
    src/Capture.h
    src/CaptureDXGI.h
    src/Converter.h
    src/Detector.h
    src/Gui.h
    src/AcerSpatialLabs.h
    src/Settings.h
    src/VideoSource.h
    src/resource.h
    src/app.rc
    ${IMGUI_SOURCES}
)

target_include_directories(SRLoom PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SR_INCLUDE_DIRS}
)

target_compile_definitions(SRLoom PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(SRLoom PRIVATE
    ${SR_IMPORT_LIBS}
    ${SR_THIRD_PARTY_LIBS}
    d3d11
    dxgi
    dxguid
    d3dcompiler   # runtime HLSL compile for the conversion shaders
    shcore        # SetProcessDpiAwareness
    shell32       # Shell_NotifyIcon (tray)
    user32
    gdi32
    windowsapp    # Windows.Graphics.Capture (WinRT)
)

# Nothing to copy beside the exe: the UI fonts are embedded as resources (app.rc) and
# the SR runtime DLLs come from the installed SR Platform. The release is just the exe.
