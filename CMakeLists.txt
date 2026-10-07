cmake_minimum_required(VERSION 3.24)
project(camfx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

# ONNX Runtime: a prebuilt release unpacked into third_party/ (see scripts/fetch_deps.sh)
file(GLOB ORT_CANDIDATES "${CMAKE_SOURCE_DIR}/third_party/onnxruntime-linux-x64-*")
list(SORT ORT_CANDIDATES)
list(REVERSE ORT_CANDIDATES)
list(GET ORT_CANDIDATES 0 ORT_DEFAULT_ROOT)
set(ONNXRUNTIME_ROOT "${ORT_DEFAULT_ROOT}" CACHE PATH "Root of an unpacked ONNX Runtime release")
if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
  message(FATAL_ERROR "ONNX Runtime not found. Run scripts/fetch_deps.sh or set ONNXRUNTIME_ROOT.")
endif()
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so"
  INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include")

add_executable(camfx
  src/main.cpp
  src/Params.cpp
  src/Lut3D.cpp
  src/Segmenter.cpp
  src/Effects.cpp
  src/FastOps.cpp
  src/Pipeline.cpp
  src/LoopbackMonitor.cpp
  src/Cameras.cpp
  src/PresetStore.cpp
  src/SettingsWindow.cpp
  src/PreviewWidget.cpp
  src/TrayApp.cpp
  src/Bench.cpp
)
target_compile_options(camfx PRIVATE -Wall -Wextra -O3 -march=native)
target_compile_definitions(camfx PRIVATE
  CAMFX_SOURCE_MODEL_DIR="${CMAKE_SOURCE_DIR}/models")
target_link_libraries(camfx PRIVATE
  Qt6::Widgets Qt6::Network ${OpenCV_LIBS} PkgConfig::GST onnxruntime pthread)

# Find libonnxruntime next to the binary when installed, or in the source tree when run from the build dir
set_target_properties(camfx PROPERTIES
  BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib"
  INSTALL_RPATH "$ORIGIN/../lib/camfx")

include(GNUInstallDirs)
install(TARGETS camfx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${ONNXRUNTIME_ROOT}/lib/" DESTINATION ${CMAKE_INSTALL_LIBDIR}/camfx
  FILES_MATCHING PATTERN "libonnxruntime*.so*"
  PATTERN "*tensorrt*" EXCLUDE PATTERN "cmake" EXCLUDE PATTERN "pkgconfig" EXCLUDE)
install(FILES models/selfie_multiclass_256x256.onnx
  DESTINATION ${CMAKE_INSTALL_DATADIR}/camfx/models)
install(DIRECTORY luts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/camfx/luts
  FILES_MATCHING PATTERN "*.cube")

# Micro-benchmark of the image kernels (not built by default)
add_executable(fastops-bench EXCLUDE_FROM_ALL tools/fastops_bench.cpp src/FastOps.cpp)
target_include_directories(fastops-bench PRIVATE src)
target_compile_options(fastops-bench PRIVATE -O3 -march=native)
target_link_libraries(fastops-bench PRIVATE ${OpenCV_LIBS})

# Start menu entry and icon
configure_file(data/camfx.desktop.in ${CMAKE_BINARY_DIR}/camfx.desktop @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/camfx.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/camfx.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
