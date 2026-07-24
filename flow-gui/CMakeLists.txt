cmake_minimum_required(VERSION 3.21)
project(flow-gui CXX)

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
                    DeckEditor.cpp DeckEditor.h)

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
    list(APPEND FLOWGUI_SOURCES SummaryPlotWidget.cpp SummaryPlotWidget.h)
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
target_link_libraries(flow-gui PRIVATE Qt6::Widgets)

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
