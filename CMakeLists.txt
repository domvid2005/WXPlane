cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, matches vcpkg x64-windows-static
if(CMAKE_HOST_APPLE)   # before project() and the dependencies, so OpenJPEG and libaec are built universal too
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()
project(WXPlane LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- X-Plane SDK 4.2.0 (needed for XPLMWeather.h) ---------------------------------------------------
# Download "XPSDK420.zip" from https://developer.x-plane.com/sdk/plugin-sdk-downloads/ and unzip it.
# Pass the unzipped folder: cmake -DXPLANE_SDK=/path/to/SDK ...   (it contains CHeaders/ and Libraries/)
set(XPLANE_SDK "" CACHE PATH "Folder containing CHeaders/ and Libraries/ of the X-Plane SDK (4.2.0 or newer)")
if(NOT XPLANE_SDK OR NOT EXISTS "${XPLANE_SDK}/CHeaders/XPLM/XPLMWeather.h")
  message(FATAL_ERROR "Set -DXPLANE_SDK=<SDK folder>. It must be SDK 4.2.0+ (CHeaders/XPLM/XPLMWeather.h missing).")
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# --- GRIB2 decoders for the turbulence data, built from source and linked statically ----------------------------
# OpenJPEG decodes JPEG2000-packed GRIB2 (NOAA GTGN, template 5.40); libaec decodes CCSDS-packed GRIB2 (ECMWF, 5.42).
include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)          # the static CRT setting above also applies to the dependencies
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_CODEC OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openjpeg GIT_REPOSITORY https://github.com/uclouvain/openjpeg.git GIT_TAG v2.5.3 GIT_SHALLOW TRUE)
FetchContent_Declare(libaec GIT_REPOSITORY https://github.com/MathisRosenhauer/libaec.git GIT_TAG v1.1.3 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openjpeg libaec)
add_library(wx_codecs INTERFACE)
target_link_libraries(wx_codecs INTERFACE openjp2 aec_static)
target_include_directories(wx_codecs INTERFACE
  "${openjpeg_SOURCE_DIR}/src/lib/openjp2" "${openjpeg_BINARY_DIR}/src/lib/openjp2")
target_compile_definitions(wx_codecs INTERFACE OPJ_STATIC)

set(WX_CORE_SOURCES
  src/json_mini.cpp src/metar.cpp src/sources.cpp src/wxmodel.cpp src/windsaloft.cpp src/config.cpp src/viewer.cpp
  src/inflate.cpp src/png.cpp src/grib2.cpp src/radar.cpp src/windshear.cpp src/gribx.cpp src/turbulence.cpp src/hazards.cpp)

add_library(WXPlane MODULE
  ${WX_CORE_SOURCES} src/http.cpp src/fetcher.cpp src/turbfetch.cpp src/wxgl.cpp src/plugin.cpp)

target_include_directories(WXPlane PRIVATE "${XPLANE_SDK}/CHeaders/XPLM")
target_compile_definitions(WXPlane PRIVATE
  XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1 XPLM410=1 XPLM420=1)
target_link_libraries(WXPlane PRIVATE CURL::libcurl Threads::Threads wx_codecs ${CMAKE_DL_LIBS})

# --- Output: <build>/package/WXPlane/<lin_x64|mac_x64|win_x64>/WXPlane.xpl ----------------------
if(WIN32)
  set(WX_PLATFORM_DIR win_x64)
  target_compile_definitions(WXPlane PRIVATE IBM=1 NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
  target_link_libraries(WXPlane PRIVATE "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib")
elseif(APPLE)
  set(WX_PLATFORM_DIR mac_x64)
  target_compile_definitions(WXPlane PRIVATE APL=1)
  target_link_options(WXPlane PRIVATE -undefined dynamic_lookup)
else()
  set(WX_PLATFORM_DIR lin_x64)
  target_compile_definitions(WXPlane PRIVATE LIN=1)
  # XPLM symbols are resolved by X-Plane when the plugin loads, so undefined references are expected.
  target_link_options(WXPlane PRIVATE -static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL)
endif()

set(WX_OUT "${CMAKE_BINARY_DIR}/package/WXPlane/${WX_PLATFORM_DIR}")
# Third-party notices next to the platform folders (package/WXPlane/LICENSES.txt).
add_custom_command(TARGET WXPlane POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_SOURCE_DIR}/LICENSES.txt" "${CMAKE_BINARY_DIR}/package/WXPlane/LICENSES.txt")
set_target_properties(WXPlane PROPERTIES
  PREFIX "" OUTPUT_NAME "WXPlane" SUFFIX ".xpl"
  LIBRARY_OUTPUT_DIRECTORY "${WX_OUT}" RUNTIME_OUTPUT_DIRECTORY "${WX_OUT}")
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${cfg} CFG)
  set_target_properties(WXPlane PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${CFG} "${WX_OUT}" RUNTIME_OUTPUT_DIRECTORY_${CFG} "${WX_OUT}")
endforeach()

# --- Unit tests (no X-Plane needed): ctest -------------------------------------------------------------
option(WX_BUILD_TESTS "Build the parser/model unit tests" ON)
if(WX_BUILD_TESTS)
  enable_testing()
  add_executable(test_wx tests/test_wx.cpp ${WX_CORE_SOURCES})
  target_link_libraries(test_wx PRIVATE wx_codecs)
  target_compile_definitions(test_wx PRIVATE WX_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
  add_test(NAME parser_and_model COMMAND test_wx)
endif()
