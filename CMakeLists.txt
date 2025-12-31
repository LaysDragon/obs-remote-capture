cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# ========== 設定 C++20 標準 (用於 designated initializers) ==========
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# ========== 源文件 ==========
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/plugin-main.c
  src/remote-server.cpp
  src/remote-client-source.cpp
  src/capture-preview-source.cpp
  # src/audio-stream.cpp  # 暫時禁用，使用了已棄用的 API
)

# ========== Windows 網路庫 ==========
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32)
endif()

# ========== TurboJPEG (可選) ==========
find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h)
find_library(TURBOJPEG_LIBRARY turbojpeg)

if(TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIBRARY)
  message(STATUS "Found TurboJPEG: ${TURBOJPEG_LIBRARY}")
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${TURBOJPEG_INCLUDE_DIR})
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${TURBOJPEG_LIBRARY})
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_TURBOJPEG)
else()
  message(WARNING "TurboJPEG not found - video encoding will be disabled")
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
