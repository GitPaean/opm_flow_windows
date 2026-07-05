cmake_minimum_required(VERSION 3.21)
project(flow-gui-qt CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 6 Widgets. On Windows the harness' vcpkg provides it
# (vcpkg install qtbase --triplet x64-windows); on Linux/macOS use the distro
# package (qt6-base-dev / brew install qt) or an official Qt installation and
# point CMAKE_PREFIX_PATH at it if needed.
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(flow-gui-qt WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(flow-gui-qt PRIVATE Qt6::Widgets)

# A WIN32 (non-console) app: keep main() as the entry point with MSVC.
if(MSVC)
    set_target_properties(flow-gui-qt PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

install(TARGETS flow-gui-qt
        RUNTIME DESTINATION bin
        BUNDLE  DESTINATION .)
