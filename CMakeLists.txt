# ============================================================================
# parallel-containers-bench — научный бенчмарк-фреймворк для параллельных
# контейнеров (lock-free hash maps, fine-grained locking).
#
# Ссылки:
#   [1] Cooper et al., "Benchmarking Cloud Serving Systems with YCSB", SoCC 2010
#   [2] libcds — https://github.com/khizmax/libcds
#   [3] Concurrency Kit — https://github.com/concurrencykit/ck
#   [4] sol2 — https://github.com/ThePhD/sol2
# ============================================================================
cmake_minimum_required(VERSION 3.22)
project(parallel_containers_bench
    VERSION   0.1.0
    LANGUAGES CXX C
)

# ---------- compiler requirements ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- aggressive optimisation flags (научный бенчмарк — O3 обязателен) ----------
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -march=native -flto -DNDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------- dependencies ----------

# 1. libcds  (lock-free контейнеры: SplitListMap, FeldmanHashMap)
#    Предполагается установка через vcpkg или system-wide.
#find_package(LibCDS QUIET)
#if(NOT LibCDS_FOUND)
#    # Fallback: ищем заголовки и библиотеку напрямую
#    find_path(LIBCDS_INCLUDE_DIR cds/init.h
#        HINTS
#        /usr/local/include
#        ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
#        $ENV{LIBCDS_ROOT}/include
#    )
#    find_library(LIBCDS_LIBRARY NAMES cds cds-s
#        HINTS
#        /usr/local/lib64
#        /usr/local/lib
#        ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
#        $ENV{LIBCDS_ROOT}/lib
#    )
#    if(LIBCDS_INCLUDE_DIR AND LIBCDS_LIBRARY)
#        message(STATUS "libcds found: ${LIBCDS_LIBRARY}")
#    else()
#        message(WARNING "libcds NOT found — CDS adapters will be stubs.")
#        set(LIBCDS_INCLUDE_DIR "")
#        set(LIBCDS_LIBRARY "")
#        add_compile_definitions(NO_LIBCDS)
#    endif()
#endif()

#find_path(CK_INCLUDE_DIR ck_ht.h
#    HINTS
#    /usr/local/include
#    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
#    $ENV{CK_ROOT}/include
#)
#find_library(CK_LIBRARY NAMES ck
#    HINTS
#    /usr/local/lib
#    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
#    $ENV{CK_ROOT}/lib
#)

#if(CK_INCLUDE_DIR AND CK_LIBRARY)
#    message(STATUS "Concurrency Kit found: ${CK_LIBRARY}")
#else()
#    message(WARNING "Concurrency Kit NOT found — CK_HT adapter will be a stub.")
#    set(CK_INCLUDE_DIR "")
#    set(CK_LIBRARY "")
#    add_compile_definitions(NO_CK)
#endif()

# 3. sol2 + Lua (конфигурация через Lua)
find_package(Lua REQUIRED)             # системный Lua 5.3/5.4
# Поиск sol2 (header-only)
find_path(SOL2_INCLUDE_DIR sol/sol.hpp
    HINTS
    /usr/local/include
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
    $ENV{SOL2_ROOT}/include
)

if(NOT SOL2_INCLUDE_DIR)
    message(FATAL_ERROR "sol2 NOT found. Install via vcpkg: vcpkg install sol2 or from source: https://github.com/ThePhD/sol2")
else()
    message(STATUS "sol2 found: ${SOL2_INCLUDE_DIR}")
endif()



# 4. fmt + spdlog (логирование)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)

# ---------- main executable ----------
add_executable(bench main.cpp)

target_include_directories(bench PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LUA_INCLUDE_DIR}
    #${LIBCDS_INCLUDE_DIR}
    #${CK_INCLUDE_DIR}
    ${SOL2_INCLUDE_DIR}
)

target_link_libraries(bench PRIVATE
    ${LUA_LIBRARIES}
    #${LIBCDS_LIBRARY}
    #${CK_LIBRARY}
    fmt::fmt
    spdlog::spdlog
    pthread
)

# Для perf_event_open — нужна линковка с -lrt (таймеры POSIX) — опционально
target_link_libraries(bench PRIVATE rt)
