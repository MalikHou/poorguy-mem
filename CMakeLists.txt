cmake_minimum_required(VERSION 3.20)
project(poorguy_mem VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PGMEM_WITH_ELOQSTORE "Build with EloqStore-backed storage adapter (legacy toggle)" ON)
option(PGMEM_WITH_ONNX "Build with ONNX Runtime semantic embedder" OFF)
set(PGMEM_STORE_BACKEND "" CACHE STRING "Default storage backend: eloqstore|inmemory")
set_property(CACHE PGMEM_STORE_BACKEND PROPERTY STRINGS eloqstore inmemory)

if(PGMEM_STORE_BACKEND)
    string(TOLOWER "${PGMEM_STORE_BACKEND}" PGMEM_EFFECTIVE_BACKEND)
else()
    if(PGMEM_WITH_ELOQSTORE)
        set(PGMEM_EFFECTIVE_BACKEND "eloqstore")
    else()
        set(PGMEM_EFFECTIVE_BACKEND "inmemory")
    endif()
endif()

if(NOT PGMEM_EFFECTIVE_BACKEND STREQUAL "eloqstore"
   AND NOT PGMEM_EFFECTIVE_BACKEND STREQUAL "inmemory")
    message(FATAL_ERROR "Unsupported backend '${PGMEM_EFFECTIVE_BACKEND}'. Use eloqstore|inmemory.")
endif()

message(STATUS "PGMEM default backend: ${PGMEM_EFFECTIVE_BACKEND}")

include(GNUInstallDirs)

find_package(Protobuf REQUIRED)
if(NOT Protobuf_PROTOC_EXECUTABLE)
    message(FATAL_ERROR "protoc executable is required")
endif()

find_path(PGMEM_BRPC_INCLUDE_DIR NAMES brpc/server.h)
find_library(PGMEM_BRPC_LIBRARY NAMES brpc)
if(NOT PGMEM_BRPC_INCLUDE_DIR OR NOT PGMEM_BRPC_LIBRARY)
    message(FATAL_ERROR "brpc headers or library not found")
endif()

find_path(PGMEM_GFLAGS_INCLUDE_DIR NAMES gflags/gflags.h)
find_library(PGMEM_GFLAGS_LIBRARY NAMES gflags libgflags)
if(NOT PGMEM_GFLAGS_INCLUDE_DIR OR NOT PGMEM_GFLAGS_LIBRARY)
    message(FATAL_ERROR "gflags headers or library not found")
endif()

set(PGMEM_PROTO_DIR ${PROJECT_SOURCE_DIR}/proto)
set(PGMEM_PROTO_OUT_DIR ${PROJECT_BINARY_DIR}/generated)
set(PGMEM_HTTP_ROUTER_PROTO ${PGMEM_PROTO_DIR}/http_router.proto)
set(PGMEM_HTTP_ROUTER_PROTO_SRC ${PGMEM_PROTO_OUT_DIR}/http_router.pb.cc)
set(PGMEM_HTTP_ROUTER_PROTO_HDR ${PGMEM_PROTO_OUT_DIR}/http_router.pb.h)

add_custom_command(
    OUTPUT ${PGMEM_HTTP_ROUTER_PROTO_SRC} ${PGMEM_HTTP_ROUTER_PROTO_HDR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PGMEM_PROTO_OUT_DIR}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --cpp_out=${PGMEM_PROTO_OUT_DIR}
            --proto_path=${PGMEM_PROTO_DIR}
            ${PGMEM_HTTP_ROUTER_PROTO}
    DEPENDS ${PGMEM_HTTP_ROUTER_PROTO}
    VERBATIM
)

add_custom_target(pgmem_proto_generated
    DEPENDS ${PGMEM_HTTP_ROUTER_PROTO_SRC} ${PGMEM_HTTP_ROUTER_PROTO_HDR}
)

set(PGMEM_COMMON_SOURCES
    src/util/time.cpp
    src/util/text.cpp
    src/util/json.cpp
    ${PGMEM_HTTP_ROUTER_PROTO_SRC}
    src/net/http_common.cpp
    src/net/brpc_runtime.cpp
    src/net/http_server.cpp
    src/net/http_client.cpp
    src/store/store_factory.cpp
    src/store/io_uring_probe.cpp
    src/store/in_memory_store_adapter.cpp
    src/store/eloqstore_adapter.cpp
    src/core/metrics.cpp
    src/core/retriever.cpp
    src/core/summary.cpp
    src/core/memory_engine.cpp
    src/mcp/mcp_dispatcher.cpp
    src/install/workspace_config.cpp
)

add_library(pgmem_core STATIC ${PGMEM_COMMON_SOURCES})
add_dependencies(pgmem_core pgmem_proto_generated)
target_include_directories(pgmem_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PGMEM_PROTO_OUT_DIR}
        ${PGMEM_BRPC_INCLUDE_DIR}
        ${PGMEM_GFLAGS_INCLUDE_DIR}
        ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(pgmem_core
    PUBLIC
        pthread
        ${PGMEM_BRPC_LIBRARY}
        ${PGMEM_GFLAGS_LIBRARY}
        ${Protobuf_LIBRARIES}
)

target_compile_definitions(pgmem_core
    PUBLIC
        PGMEM_VERSION=\"${PROJECT_VERSION}\"
        PGMEM_DEFAULT_STORE_BACKEND=\"${PGMEM_EFFECTIVE_BACKEND}\"
)

if(PGMEM_EFFECTIVE_BACKEND STREQUAL "eloqstore")
    if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/eloqstore/CMakeLists.txt")
        set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(WITH_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(WITH_DB_STRESS OFF CACHE BOOL "" FORCE)
        set(WITH_BENCHMARK OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/eloqstore EXCLUDE_FROM_ALL)
        target_compile_definitions(pgmem_core PUBLIC PGMEM_WITH_ELOQSTORE=1)
        target_link_libraries(pgmem_core PUBLIC eloqstore)
    else()
        message(WARNING "Backend is eloqstore but third_party/eloqstore is missing")
    endif()
endif()

if(PGMEM_WITH_ONNX)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime)
    if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
        target_compile_definitions(pgmem_core PUBLIC PGMEM_WITH_ONNX=1)
        target_include_directories(pgmem_core PUBLIC ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(pgmem_core PUBLIC ${ONNXRUNTIME_LIBRARY})
    else()
        message(WARNING "PGMEM_WITH_ONNX=ON but ONNX Runtime was not found; using lexical + hash semantic fallback")
    endif()
endif()

add_executable(pgmemd src/apps/pgmemd_main.cpp)
target_link_libraries(pgmemd PRIVATE pgmem_core)

add_executable(pgmem-install src/apps/pgmem_install_main.cpp)
target_link_libraries(pgmem-install PRIVATE pgmem_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(pgmem_unit_tests
        tests/unit_main.cpp
        tests/test_text.cpp
        tests/test_memory_engine.cpp
        tests/test_mcp_dispatcher.cpp
        tests/test_brpc_runtime.cpp
        tests/test_store_factory.cpp
        tests/test_workspace_config.cpp
    )
    target_link_libraries(pgmem_unit_tests PRIVATE pgmem_core)
    add_test(NAME pgmem_unit_tests COMMAND pgmem_unit_tests)
endif()

install(TARGETS pgmemd pgmem-install
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
