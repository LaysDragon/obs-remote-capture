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

# 暫時繞過 vcpkg 的路徑限制以找到本地 OBS 庫
set(_orig_find_root_path_mode ${CMAKE_FIND_ROOT_PATH_MODE_PACKAGE})
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

# 恢復原始設定
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ${_orig_find_root_path_mode})

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
  src/remote-client-source.cpp
  src/capture-preview-source.cpp
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

# ========== gRPC 與 Protobuf (可選) ==========
option(ENABLE_GRPC "Use gRPC for network communication" OFF)

if(ENABLE_GRPC)
  find_package(gRPC CONFIG)
  find_package(protobuf CONFIG)
  
  if(gRPC_FOUND AND protobuf_FOUND)
    message(STATUS "Found gRPC: using gRPC for network communication")
    
    # 獲取 protoc 和 grpc_cpp_plugin 路徑
    # 靜態 triplet 不包含工具，需要從 x64-windows 版本獲取
    find_program(PROTOC_EXECUTABLE protoc 
      HINTS 
        "${VCPKG_INSTALLED_DIR}/x64-windows/tools/protobuf"
        "${_VCPKG_INSTALLED_DIR}/x64-windows/tools/protobuf"
      REQUIRED
    )
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin
      HINTS
        "${VCPKG_INSTALLED_DIR}/x64-windows/tools/grpc"
        "${_VCPKG_INSTALLED_DIR}/x64-windows/tools/grpc"
      REQUIRED
    )
    
    # Proto 文件
    set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/remote_capture.proto")
    set(PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY ${PROTO_OUT_DIR})
    
    # 生成 protobuf 和 gRPC 代碼
    set(PROTO_SRCS "${PROTO_OUT_DIR}/remote_capture.pb.cc")
    set(PROTO_HDRS "${PROTO_OUT_DIR}/remote_capture.pb.h")
    set(GRPC_SRCS "${PROTO_OUT_DIR}/remote_capture.grpc.pb.cc")
    set(GRPC_HDRS "${PROTO_OUT_DIR}/remote_capture.grpc.pb.h")
    
    add_custom_command(
      OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
      COMMAND ${PROTOC_EXECUTABLE}
        --proto_path="${CMAKE_CURRENT_SOURCE_DIR}/proto"
        --cpp_out="${PROTO_OUT_DIR}"
        --grpc_out="${PROTO_OUT_DIR}"
        --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN}"
        "${PROTO_FILE}"
      DEPENDS ${PROTO_FILE}
      COMMENT "Generating gRPC/Protobuf code..."
    )
    
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
      ${PROTO_SRCS} ${PROTO_HDRS}
      ${GRPC_SRCS} ${GRPC_HDRS}
      src/grpc_server.cpp
      src/grpc_client.cpp
    )
    
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${PROTO_OUT_DIR})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE 
      gRPC::grpc++
      protobuf::libprotobuf
    )
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_GRPC)
    
    # 禁用生成代碼的警告 (protobuf 生成的代碼有不可避免的 size_t 轉換警告)
    if(MSVC)
      set_source_files_properties(
        ${PROTO_SRCS} ${GRPC_SRCS}
        PROPERTIES COMPILE_FLAGS "/wd4267 /wd4244"
      )
    endif()
  else()
    message(WARNING "gRPC not found - using fallback socket implementation")
  endif()
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

