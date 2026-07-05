cmake_minimum_required(VERSION 3.24)
project(flow-gui CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FLTK: use an installed copy when available (e.g. libfltk-dev on Linux),
# otherwise fetch and build the pinned release from source. The from-source
# path is fully self-contained on both Windows (GDI) and Linux (X11 dev
# headers required: libx11-dev libxext-dev libxft-dev libxinerama-dev).
find_package(FLTK CONFIG QUIET)
if(NOT FLTK_FOUND)
    include(FetchContent)
    set(FLTK_BUILD_TEST     OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_FLUID    OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_GL       OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_FORMS    OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(fltk
        URL https://github.com/fltk/fltk/archive/refs/tags/release-1.4.3.tar.gz
    )
    FetchContent_MakeAvailable(fltk)
endif()

add_executable(flow-gui WIN32 main.cpp)

if(TARGET fltk::fltk)          # CONFIG package or FetchContent (FLTK >= 1.4)
    target_link_libraries(flow-gui PRIVATE fltk::fltk)
elseif(TARGET fltk)            # older exported target name (FLTK 1.3 CONFIG)
    target_link_libraries(flow-gui PRIVATE fltk)
else()                         # classic FindFLTK module variables
    target_include_directories(flow-gui PRIVATE ${FLTK_INCLUDE_DIR})
    target_link_libraries(flow-gui PRIVATE ${FLTK_LIBRARIES})
endif()

# A GUI (WIN32) app has no console; keep main() as the entry point.
if(MSVC)
    set_target_properties(flow-gui PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

install(TARGETS flow-gui RUNTIME DESTINATION bin)
