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
# C is enabled in addition to C++ because third_party/npclient/npclient.c is
# a pure C file built as a free-standing DLL (NPClient64) -- it must not pull
# in any C++ runtime so games loading it don't end up with a second MSVC CRT
# in their process. Listing both languages here tells CMake to enable the C
# toolchain so it can pick a linker language for the C-only DLL target.
project(SRLoom CXX C)

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

# NPClient.dll (x64) -- the consumer-side DLL TrackIR-aware games load to read
# head-pose data. Source vendored from OpenTrack's contrib/npclient (MIT,
# originally from the Linuxtrack project). Built as a free-standing Windows
# DLL; the built artifact is copied into assets/npclient/ so the resource
# compiler can embed it as RCDATA in SRLoom.exe. SR Loom extracts the bytes
# back to %LOCALAPPDATA%\SRLoom\NPClient\ on first launch and points the
# NaturalPoint registry key at that path, so TrackIR games "just work"
# without users installing OpenTrack themselves.
#
# x86 NPClient.dll is not built here -- adding it requires a second CMake
# invocation (cross-arch builds aren't single-project under MSVC) and the
# 64-bit DLL covers every modern TrackIR-aware sim/racing title we care
# about for v1.7. x86 can be slotted in later for legacy 32-bit games
# (FSX, P3D v4, X-Plane 9/10, rFactor 2 32-bit) without changing this layout.
add_library(NPClient64 SHARED
    third_party/npclient/npclient.c
)
set_target_properties(NPClient64 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "NPClient64"
    SUFFIX ".dll"
)
# DLL is C99; suppress the C++-style warnings MSVC throws at it.
target_compile_definitions(NPClient64 PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

# Drop the built DLL into a known source-tree path so the resource
# compiler's IDR_NPCLIENT64 RCDATA line can locate it. copy_if_different
# means no spurious rebuilds when the bytes are unchanged.
set(NPCLIENT_EMBED_DIR "${CMAKE_SOURCE_DIR}/assets/npclient")
file(MAKE_DIRECTORY "${NPCLIENT_EMBED_DIR}")
add_custom_command(TARGET NPClient64 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:NPClient64>
        "${NPCLIENT_EMBED_DIR}/NPClient64.dll"
    VERBATIM
)

# Converter shaders, compiled at BUILD time: fxc (from the Windows SDK) turns
# each entry point into a header of bytecode (g_Converter_<entry>) that the exe
# embeds. Compiling them when SR Loom started took ~17 s the first time after
# every change (the main shader alone is that slow to optimise).
find_program(FXC_EXE fxc
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    REQUIRED)
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")
set(CONVERTER_HLSL "${CMAKE_SOURCE_DIR}/src/shaders/Converter.hlsl")
set(CONVERTER_ENTRIES
    VSMain:vs_5_0 PSMain:ps_5_0 PSAnaDisp:ps_5_0 PSAnaDesc:ps_5_0 PSAnaDescCoarse:ps_5_0 PSDown:ps_5_0 PSAnaRefine:ps_5_0
    PSAnaFill:ps_5_0 PSAnaSmooth:ps_5_0 PSAnaPair:ps_5_0 PSAnaCompose:ps_5_0 PSAnaPair:ps_5_0:Plain PSAnaCompose:ps_5_0:Plain PSFmtHalfSBS:ps_5_0 PSFmtFullSBS:ps_5_0 PSFmtTAB:ps_5_0 PSFmtRow:ps_5_0 PSFmtColumn:ps_5_0 PSFmtChecker:ps_5_0 PSFmtFramePack:ps_5_0 PSFmtAnaglyph:ps_5_0 PSFmtQuilt:ps_5_0 CSFmtAnaglyph:cs_5_0 CSFmtChecker:cs_5_0 CSFmtColumn:cs_5_0 CSFmtRow:cs_5_0 PSQuiltH:ps_5_0 PSAnaBoxCheck:ps_5_0 PSAnaBoxRows:ps_5_0 PSAnaBoxMap:ps_5_0 PSChange:ps_5_0 PSChangeGrow:ps_5_0 PSChangeGrow2:ps_5_0 PSDownBox:ps_5_0 PSDownFlat:ps_5_0 PSFlatShrink:ps_5_0 PSReach:ps_5_0 PSChangeStatRows:ps_5_0 PSChangeStat:ps_5_0 PSScrollCost:ps_5_0 PSScrollSum:ps_5_0 PSScrollPick:ps_5_0 PSChangeScrollQ:ps_5_0 PSChangeScroll:ps_5_0 PSAnaBoxShiftCost:ps_5_0 PSAnaBoxShift:ps_5_0)
set(CONVERTER_SHADER_HEADERS)
foreach(E ${CONVERTER_ENTRIES})
    string(REPLACE ":" ";" P "${E}")
    list(GET P 0 ENTRY)
    list(GET P 1 PROFILE)
    # (A third field: the same entry point compiled again with that name added
    # and SRC_PLAIN defined -- for a source that needs no decoding, Converter.hlsl.)
    list(LENGTH P PN)
    set(VARIANT "")
    set(DEFS)
    if(PN GREATER 2)
        list(GET P 2 VARIANT)
        set(DEFS /D SRC_PLAIN=1)
    endif()
    set(OUT "${SHADER_OUT_DIR}/Converter_${ENTRY}${VARIANT}.h")
    add_custom_command(OUTPUT "${OUT}"
        COMMAND "${FXC_EXE}" /nologo /T ${PROFILE} /E ${ENTRY} /O3 ${DEFS} /Vn g_Converter_${ENTRY}${VARIANT} /Fh "${OUT}" "${CONVERTER_HLSL}"
        DEPENDS "${CONVERTER_HLSL}"
        COMMENT "fxc: Converter ${ENTRY}${VARIANT}"
        VERBATIM)
    list(APPEND CONVERTER_SHADER_HEADERS "${OUT}")
endforeach()
# (Listed in the project for editing only -- Visual Studio must not run its own
# shader compile on it.)
set_source_files_properties("${CONVERTER_HLSL}" PROPERTIES VS_TOOL_OVERRIDE "None")

add_executable(SRLoom WIN32
    ${CONVERTER_SHADER_HEADERS}
    src/shaders/Converter.hlsl
    src/main.cpp
    src/Renderer.cpp
    src/Present12.cpp
    src/SRWeaver.cpp
    src/TrayIcon.cpp
    src/Capture.cpp
    src/CaptureDXGI.cpp
    src/Converter.cpp
    src/RegionWeave.cpp
    src/RegionWeave.h
    src/ScreenAnalysis.cpp
    src/ScreenAnalysis.h
    src/GpuTrack.cpp
    src/GpuTrack.h
    src/Detector.cpp
    src/Gui.cpp
    src/AcerSpatialLabs.cpp
    src/Settings.cpp
    src/Profiles.cpp
    src/Profiles.h
    src/MediaProfiles.cpp
    src/MediaProfiles.h
    src/VideoSource.cpp
    src/UpdateChecker.cpp
    src/UpdateChecker.h
    src/KatangaSource.cpp
    src/KatangaSource.h
    src/LFPReader.cpp
    src/LFPReader.h
    src/LFPRenderer.cpp
    src/LFPRenderer.h
    src/OpenTrackBridge.cpp
    src/OpenTrackBridge.h
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

# Release PDB, kept locally (not shipped, *.pdb is gitignored). The crash
# handler logs "SRLoom.exe +0x<RVA>"; keep the PDB that matches each
# released exe so a user's log can be resolved to a function/line.
# /DEBUG turns /OPT:REF + /OPT:ICF off by default -- re-enable them so the
# exe stays as lean as before (it only gains a small PDB-path record).
if(MSVC)
    target_compile_options(SRLoom PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(SRLoom PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

target_include_directories(SRLoom PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${SHADER_OUT_DIR}          # (the build-time compiled shader headers)
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
    d3d12
    dxgi
    dxguid
    d3dcompiler   # runtime HLSL compile for the smaller shaders (the Converter's are built ahead)
    shcore        # SetProcessDpiAwareness
    shell32       # Shell_NotifyIcon (tray)
    user32
    gdi32
    windowsapp    # Windows.Graphics.Capture (WinRT)
)

# SRLoom embeds NPClient64.dll as RCDATA, which only resolves at RC compile
# time if the DLL is already on disk. Building NPClient64 first guarantees
# the POST_BUILD copy to assets/npclient/ has run by the time SRLoom starts.
add_dependencies(SRLoom NPClient64)

# Nothing to copy beside the exe: the UI fonts are embedded as resources (app.rc) and
# the SR runtime DLLs come from the installed SR Platform. The release is just the exe.

# Offline check of the anaglyph "Recovered Colour" conversion (not shipped;
# built only on request: cmake --build ... --target anatest). anatest runs the
# current converter; anatest_head the one in build/anatest/head (e.g. a
# previous commit's Converter.cpp/.h) to compare against.
add_executable(anatest EXCLUDE_FROM_ALL tools/anatest/anatest.cpp src/Converter.cpp src/ScreenAnalysis.cpp ${CONVERTER_SHADER_HEADERS})
target_include_directories(anatest PRIVATE ${CMAKE_SOURCE_DIR}/src ${SHADER_OUT_DIR})
target_compile_definitions(anatest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(anatest PRIVATE d3d11 dxgi d3dcompiler user32 windowscodecs ole32)
if(EXISTS "${CMAKE_SOURCE_DIR}/build/anatest/head/Converter.cpp")
    add_executable(anatest_head EXCLUDE_FROM_ALL tools/anatest/anatest.cpp "${CMAKE_SOURCE_DIR}/build/anatest/head/Converter.cpp")
    target_include_directories(anatest_head PRIVATE "${CMAKE_SOURCE_DIR}/build/anatest/head" ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(anatest_head PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS ANATEST_HEAD)
    target_link_libraries(anatest_head PRIVATE d3d11 dxgi windowscodecs ole32 d3dcompiler)
endif()

# captest: zero-copy capture against the real Windows capture (its own test
# window + the monitor it's on; prints numbers only). Built only on request:
# cmake --build ... --target captest. Output in its own folder so its log
# lines don't land in SR Loom's srweaver.log.
add_executable(captest EXCLUDE_FROM_ALL tools/captest/captest.cpp src/Capture.cpp src/Converter.cpp ${CONVERTER_SHADER_HEADERS})
target_include_directories(captest PRIVATE ${CMAKE_SOURCE_DIR}/src ${SHADER_OUT_DIR})
target_compile_definitions(captest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(captest PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 windowsapp)
set_target_properties(captest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/captest)

# dx12test: checks the D3D11 -> D3D12 texture sharing the DX12 weaver option is
# built on (tools/dx12test). Not part of the app; built only when asked for:
# cmake --build ... --target dx12test.
add_executable(dx12test EXCLUDE_FROM_ALL tools/dx12test/dx12test.cpp)
target_compile_definitions(dx12test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(dx12test PRIVATE d3d11 d3d12 dxgi)
set_target_properties(dx12test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/dx12test)

# dx12bench: times Recovered Colour's compose kernel under Direct3D 11 and
# Direct3D 12 (fxc, dxc, 16-bit floats). Not part of the app:
# cmake --build ... --target dx12bench.
add_executable(dx12bench EXCLUDE_FROM_ALL tools/dx12bench/dx12bench.cpp)
target_compile_definitions(dx12bench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(dx12bench PRIVATE d3d11 d3d12 dxgi d3dcompiler)
set_target_properties(dx12bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/dx12bench)

# lensprobe: what the SR SDK's weaving library reports for the lens (numbers
# only; no window). cmake --build ... --target lensprobe.
add_executable(lensprobe EXCLUDE_FROM_ALL tools/lensprobe/lensprobe.cpp)
target_include_directories(lensprobe PRIVATE "${SR_SDK_DIR}/include")
target_compile_definitions(lensprobe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(lensprobe PRIVATE ${SR_IMPORT_LIBS} ${SR_THIRD_PARTY_LIBS})
set_target_properties(lensprobe PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release)
