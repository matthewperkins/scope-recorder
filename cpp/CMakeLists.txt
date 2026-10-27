cmake_minimum_required(VERSION 3.28)
project(scope_recorder LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
# Build for the GPU in this machine unless told otherwise (e.g. -DCMAKE_CUDA_ARCHITECTURES=75 for a T400).
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES native)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
find_package(CUDAToolkit REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)

# IC4 SDK, unpacked from The Imaging Source's .deb packages (see third_party/ic4).
add_library(ic4core SHARED IMPORTED)
set_target_properties(ic4core PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/third_party/ic4/lib/libic4core.so
    IMPORTED_NO_SONAME OFF
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/third_party/ic4/include)

add_executable(scope_recorder
    src/main.cpp
    src/MainWindow.cpp
    src/Camera.cpp
    src/GpuFrames.cu
    src/LiveView.cpp
    src/Recorder.cpp
    src/CameraSettingsDialog.cpp
)

target_link_libraries(scope_recorder PRIVATE
    Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets
    CUDA::cudart
    PkgConfig::FFMPEG
    ic4core
)

target_compile_options(scope_recorder PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)

# Find libic4core.so next to the source tree without installing it system-wide.
set_target_properties(scope_recorder PROPERTIES
    BUILD_RPATH ${CMAKE_SOURCE_DIR}/third_party/ic4/lib)

# Hardware test for the settings dialog (needs the camera; not part of the app).
add_executable(settings_dialog_test EXCLUDE_FROM_ALL tests/settings_dialog_test.cpp src/CameraSettingsDialog.cpp)
target_include_directories(settings_dialog_test PRIVATE src)
target_link_libraries(settings_dialog_test PRIVATE Qt6::Widgets ic4core)
set_target_properties(settings_dialog_test PROPERTIES BUILD_RPATH ${CMAKE_SOURCE_DIR}/third_party/ic4/lib)

# Hardware test of the "Select camera…" / reconnect path (needs the camera; not part of the app).
add_executable(camera_reopen_test EXCLUDE_FROM_ALL tests/camera_reopen_test.cpp src/Camera.cpp src/GpuFrames.cu)
target_include_directories(camera_reopen_test PRIVATE src)
target_link_libraries(camera_reopen_test PRIVATE Qt6::Core Qt6::Gui CUDA::cudart ic4core)
set_target_properties(camera_reopen_test PROPERTIES BUILD_RPATH ${CMAKE_SOURCE_DIR}/third_party/ic4/lib)
