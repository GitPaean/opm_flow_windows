# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
cmake_minimum_required(VERSION 3.21)
project(flow-gui VERSION 0.7.3 LANGUAGES CXX)

# C++20: the opm-common headers used by the Results tab require it (and Qt 6
# is fine with it everywhere).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 6 Widgets (+ Charts for the Results tab, optional). On Windows use the
# official prebuilt Qt (see README); on Linux/macOS the distro/homebrew Qt.
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Qt6 QUIET COMPONENTS Charts OpenGLWidgets)
qt_standard_project_setup()

set(FLOWGUI_SOURCES main.cpp FlowGuiWindow.cpp FlowGuiWindow.h
                    DeckEditor.cpp DeckEditor.h CasePath.h
                    GuiPaths.cpp GuiPaths.h
                    FlowLayout.cpp FlowLayout.h)

# The Windows application icon. A .rc listed here is compiled and linked by
# CMake's RC support, which MinGW has and MSVC has; nothing else is needed for
# Explorer and the taskbar to pick the icon up. Guarded because the file names
# a .ico, which only the Windows resource compiler understands.
if(WIN32)
    enable_language(RC)
    list(APPEND FLOWGUI_SOURCES flow-gui.rc)
endif()

# --- optional summary plotting: Qt Charts + opm-common's EclIO::ESmry -------
# Auto-detected from this harness' layout (../install-mpi + vcpkg); can be
# pointed elsewhere with -DFLOWGUI_OPM_PREFIX=<opm install prefix> and
# -DFLOWGUI_FMT_LIB=<fmt library>.
set(FLOWGUI_OPM_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../install-mpi"
    CACHE PATH "opm-common install prefix providing EclIO::ESmry")

if(WIN32)
    set(_opmlib "${FLOWGUI_OPM_PREFIX}/lib/opmcommon.lib")
    set(_fmt_default "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/installed/x64-windows/lib/fmt.lib")
else()
    set(_opmlib "${FLOWGUI_OPM_PREFIX}/lib/libopmcommon.a")
    set(_fmt_default "fmt")
endif()
set(FLOWGUI_FMT_LIB "${_fmt_default}" CACHE STRING "fmt library for opmcommon")

set(FLOWGUI_HAVE_SUMMARY OFF)
if(Qt6Charts_FOUND AND EXISTS "${_opmlib}"
   AND EXISTS "${FLOWGUI_OPM_PREFIX}/include/opm/io/eclipse/ESmry.hpp")
    set(FLOWGUI_HAVE_SUMMARY ON)
    # RestartCompare rides along with the summary option: it needs opm-common
    # for ERst and Qt Charts to draw the divergence, which is exactly the pair
    # this option already establishes.
    list(APPEND FLOWGUI_SOURCES SummaryPlotWidget.cpp SummaryPlotWidget.h
                                RestartCompare.cpp RestartCompare.h)
endif()
message(STATUS "flow-gui summary plotting: ${FLOWGUI_HAVE_SUMMARY}")

# 3D results viewer: needs opm-common (EGrid/EInit/ERst) + Qt OpenGLWidgets
# (part of qtbase on every platform).
set(FLOWGUI_HAVE_3D OFF)
if(Qt6OpenGLWidgets_FOUND AND EXISTS "${_opmlib}"
   AND EXISTS "${FLOWGUI_OPM_PREFIX}/include/opm/io/eclipse/EGrid.hpp")
    set(FLOWGUI_HAVE_3D ON)
    list(APPEND FLOWGUI_SOURCES Viewer3D.cpp Viewer3D.h)
endif()
message(STATUS "flow-gui 3D viewer: ${FLOWGUI_HAVE_3D}")

qt_add_executable(flow-gui WIN32 MACOSX_BUNDLE ${FLOWGUI_SOURCES})
target_compile_definitions(flow-gui PRIVATE FLOWGUI_VERSION="${PROJECT_VERSION}")
target_link_libraries(flow-gui PRIVATE Qt6::Widgets)

# The application icon, compiled into the binary so it is there whether
# flow-gui runs from a build tree, an unpacked release or an installed prefix.
# Every size is drawn for the size it is used at rather than scaled down from
# one large image, which is what keeps the 16px taskbar icon from turning to
# mush; QIcon picks the nearest and Qt does the rest.
#
# qt_add_resources rather than a .qrc listed in the sources: AUTORCC is not one
# of the things qt_standard_project_setup() turns on, so a .qrc there is passed
# through as a source CMake has no rule for and silently never compiled. BASE
# strips the directory, so the paths are :/icons/flow-gui-<n>.png.
qt_add_resources(flow-gui "appicon"
    PREFIX "/icons"
    BASE   "icons"
    FILES
        icons/flow-gui-16.png
        icons/flow-gui-24.png
        icons/flow-gui-32.png
        icons/flow-gui-48.png
        icons/flow-gui-64.png
        icons/flow-gui-128.png
        icons/flow-gui-256.png)

if(FLOWGUI_HAVE_3D)
    target_compile_definitions(flow-gui PRIVATE FLOWGUI_HAVE_3D)
    target_link_libraries(flow-gui PRIVATE Qt6::OpenGLWidgets)
endif()

if(FLOWGUI_HAVE_SUMMARY OR FLOWGUI_HAVE_3D)
    if(FLOWGUI_HAVE_SUMMARY)
        target_compile_definitions(flow-gui PRIVATE FLOWGUI_HAVE_SUMMARY)
        target_link_libraries(flow-gui PRIVATE Qt6::Charts)
    endif()
    target_include_directories(flow-gui PRIVATE "${FLOWGUI_OPM_PREFIX}/include")
    target_link_libraries(flow-gui PRIVATE "${_opmlib}" "${FLOWGUI_FMT_LIB}")
    if(MSVC)
        # opm-common headers need the conformance switches; the static lib was
        # built with /openmp:llvm and (for the harness build) MS-MPI.
        target_compile_options(flow-gui PRIVATE /permissive- /Zc:__cplusplus /bigobj)
        target_compile_definitions(flow-gui PRIVATE NOMINMAX _USE_MATH_DEFINES)
        if(DEFINED ENV{VCToolsInstallDir})
            target_link_libraries(flow-gui PRIVATE "$ENV{VCToolsInstallDir}lib/x64/libomp.lib")
        endif()
        if(DEFINED ENV{MSMPI_LIB64})
            target_link_libraries(flow-gui PRIVATE "$ENV{MSMPI_LIB64}msmpi.lib")
        endif()
        # vcpkg include dir (fmt headers referenced by some opm-common headers)
        target_include_directories(flow-gui PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/installed/x64-windows/include")
        # runtime DLLs the summary linkage needs next to the exe
        set(_fmtdll "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/installed/x64-windows/bin/fmt.dll")
        # libomp140 release DLL: full VS puts it under <redist>/x64/..., the
        # Build Tools under <redist>/debug_nonredist/x64/... (quirk).
        file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _redist)
        set(_ompdll "")
        foreach(_cand
                "${_redist}/x64/Microsoft.VC143.OpenMP.LLVM/libomp140.x86_64.dll"
                "${_redist}/debug_nonredist/x64/Microsoft.VC143.OpenMP.LLVM/libomp140.x86_64.dll")
            if(NOT _ompdll AND EXISTS "${_cand}")
                set(_ompdll "${_cand}")
            endif()
        endforeach()
        message(STATUS "flow-gui runtime DLLs: fmt=${_fmtdll} omp=${_ompdll}")
        foreach(_dll IN LISTS _fmtdll _ompdll)
            if(EXISTS "${_dll}")
                add_custom_command(TARGET flow-gui POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_dll}" "$<TARGET_FILE_DIR:flow-gui>")
            endif()
        endforeach()
    else()
        find_package(OpenMP QUIET)
        if(OpenMP_CXX_FOUND)
            target_link_libraries(flow-gui PRIVATE OpenMP::OpenMP_CXX)
        endif()
        find_package(MPI QUIET COMPONENTS C)
        if(MPI_C_FOUND)
            target_link_libraries(flow-gui PRIVATE MPI::MPI_C)
        endif()
    endif()
endif()

# A WIN32 (non-console) app: keep main() as the entry point with MSVC.
if(MSVC)
    set_target_properties(flow-gui PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

install(TARGETS flow-gui
        RUNTIME DESTINATION bin
        BUNDLE  DESTINATION .)

# Desktop integration, on the platforms that have this notion of it. The icon
# compiled into the binary covers the window itself; these two are what the
# shell reads BEFORE the program runs - the launcher, the dock and the app
# grid - and on Wayland they are the only thing it will read at all.
#
# The icons go in one per size under hicolor rather than as a single large
# file: that is the directory the theme looks in for a given size, and it is
# what lets a 22px panel and a 256px grid each get artwork drawn for them.
if(UNIX AND NOT APPLE)
    install(FILES flow-gui.desktop
            DESTINATION share/applications)
    foreach(_sz 16 24 32 48 64 128 256)
        install(FILES "icons/flow-gui-${_sz}.png"
                DESTINATION "share/icons/hicolor/${_sz}x${_sz}/apps"
                RENAME flow-gui.png)
    endforeach()
endif()
