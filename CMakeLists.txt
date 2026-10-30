cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, matches vcpkg x64-windows-static
project(WeatherFix LANGUAGES CXX)

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

set(WX_CORE_SOURCES
  src/json_mini.cpp src/metar.cpp src/sources.cpp src/wxmodel.cpp src/config.cpp)

add_library(WeatherFix MODULE
  ${WX_CORE_SOURCES} src/http.cpp src/fetcher.cpp src/plugin.cpp)

target_include_directories(WeatherFix PRIVATE "${XPLANE_SDK}/CHeaders/XPLM")
target_compile_definitions(WeatherFix PRIVATE
  XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1 XPLM410=1 XPLM420=1)
target_link_libraries(WeatherFix PRIVATE CURL::libcurl Threads::Threads)

# --- Output: <build>/package/WeatherFix/<lin_x64|mac_x64|win_x64>/WeatherFix.xpl ----------------------
if(WIN32)
  set(WX_PLATFORM_DIR win_x64)
  target_compile_definitions(WeatherFix PRIVATE IBM=1 NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
  target_link_libraries(WeatherFix PRIVATE "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib")
elseif(APPLE)
  set(WX_PLATFORM_DIR mac_x64)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
  target_compile_definitions(WeatherFix PRIVATE APL=1)
  target_link_options(WeatherFix PRIVATE -undefined dynamic_lookup)
else()
  set(WX_PLATFORM_DIR lin_x64)
  target_compile_definitions(WeatherFix PRIVATE LIN=1)
  # XPLM symbols are resolved by X-Plane when the plugin loads, so undefined references are expected.
  target_link_options(WeatherFix PRIVATE -static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL)
endif()

set(WX_OUT "${CMAKE_BINARY_DIR}/package/WeatherFix/${WX_PLATFORM_DIR}")
set_target_properties(WeatherFix PROPERTIES
  PREFIX "" OUTPUT_NAME "WeatherFix" SUFFIX ".xpl"
  LIBRARY_OUTPUT_DIRECTORY "${WX_OUT}" RUNTIME_OUTPUT_DIRECTORY "${WX_OUT}")
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${cfg} CFG)
  set_target_properties(WeatherFix PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${CFG} "${WX_OUT}" RUNTIME_OUTPUT_DIRECTORY_${CFG} "${WX_OUT}")
endforeach()

# --- Unit tests (no X-Plane needed): ctest -------------------------------------------------------------
option(WX_BUILD_TESTS "Build the parser/model unit tests" ON)
if(WX_BUILD_TESTS)
  enable_testing()
  add_executable(test_wx tests/test_wx.cpp ${WX_CORE_SOURCES})
  add_test(NAME parser_and_model COMMAND test_wx)
endif()
