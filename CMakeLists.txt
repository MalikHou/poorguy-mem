cmake_minimum_required(VERSION 3.20)
project(poorguy_mem VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PGMEM_WITH_ELOQSTORE "Build with EloqStore-backed storage adapter (legacy toggle)" ON)
option(PGMEM_WITH_ONNX "Build with ONNX Runtime semantic embedder" OFF)
set(PGMEM_STORE_BACKEND "" CACHE STRING "Default storage backend: eloqstore|inmemory|custom")
set_property(CACHE PGMEM_STORE_BACKEND PROPERTY STRINGS eloqstore inmemory custom)
set(PGMEM_CUSTOM_STORE_SOURCES "" CACHE STRING "Semicolon-separated source list providing CreateCustomStoreAdapter")

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
   AND NOT PGMEM_EFFECTIVE_BACKEND STREQUAL "inmemory"
   AND NOT PGMEM_EFFECTIVE_BACKEND STREQUAL "custom")
    message(FATAL_ERROR "Unsupported backend '${PGMEM_EFFECTIVE_BACKEND}'. Use eloqstore|inmemory|custom.")
endif()

message(STATUS "PGMEM default backend: ${PGMEM_EFFECTIVE_BACKEND}")

include(GNUInstallDirs)

set(PGMEM_COMMON_SOURCES
    src/util/time.cpp
    src/util/text.cpp
    src/util/json.cpp
    src/net/http_common.cpp
    src/net/http_server.cpp
    src/net/http_client.cpp
    src/store/store_factory.cpp
    src/store/in_memory_store_adapter.cpp
    src/store/eloqstore_adapter.cpp
    src/core/metrics.cpp
    src/core/retriever.cpp
    src/core/summary.cpp
    src/core/sync.cpp
    src/core/memory_engine.cpp
    src/mcp/mcp_dispatcher.cpp
    src/install/workspace_config.cpp
)

if(PGMEM_EFFECTIVE_BACKEND STREQUAL "custom")
    if(PGMEM_CUSTOM_STORE_SOURCES STREQUAL "")
        message(FATAL_ERROR "PGMEM_EFFECTIVE_BACKEND=custom requires -DPGMEM_CUSTOM_STORE_SOURCES=<files>")
    endif()
    list(APPEND PGMEM_COMMON_SOURCES ${PGMEM_CUSTOM_STORE_SOURCES})
endif()

add_library(pgmem_core STATIC ${PGMEM_COMMON_SOURCES})
target_include_directories(pgmem_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(pgmem_core PUBLIC pthread)

target_compile_definitions(pgmem_core
    PUBLIC
        PGMEM_VERSION=\"${PROJECT_VERSION}\"
        PGMEM_DEFAULT_STORE_BACKEND=\"${PGMEM_EFFECTIVE_BACKEND}\"
)

if(PGMEM_EFFECTIVE_BACKEND STREQUAL "custom")
    target_compile_definitions(pgmem_core PUBLIC PGMEM_HAS_CUSTOM_STORE=1)
endif()

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

add_executable(pgmem-syncd src/apps/pgmem_syncd_main.cpp)
target_link_libraries(pgmem-syncd PRIVATE pgmem_core)

add_executable(pgmem-install src/apps/pgmem_install_main.cpp)
target_link_libraries(pgmem-install PRIVATE pgmem_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(pgmem_unit_tests
        tests/unit_main.cpp
        tests/test_text.cpp
        tests/test_memory_engine.cpp
        tests/test_sync_lww.cpp
        tests/test_store_factory.cpp
        tests/test_workspace_config.cpp
    )
    target_link_libraries(pgmem_unit_tests PRIVATE pgmem_core)
    add_test(NAME pgmem_unit_tests COMMAND pgmem_unit_tests)
endif()

install(TARGETS pgmemd pgmem-syncd pgmem-install
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
