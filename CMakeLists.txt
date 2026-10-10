cmake_minimum_required(VERSION 3.16)
project(libcari VERSION 2.1.0 LANGUAGES C)

# libcari - CARI 2.1 protocol library
#   cari_proto.h   socket-free codec layer (always built)
#   cari_flac.h    libFLAC wrapper for compressed baseband (CARI_WITH_FLAC)
#   cari_slave.h   ZMQ slave layer   } CARI_WITH_ZMQ
#   cari_master.h  ZMQ master layer  }

option(CARI_WITH_ZMQ   "Build the ZMQ slave/master layer" ON)
option(CARI_WITH_FLAC  "Build FLAC compression support (libFLAC)" ON)
option(CARI_BUILD_TESTS "Build unit tests" ON)
option(CARI_SANITIZE   "Build with address/undefined sanitizers" OFF)
option(BUILD_SHARED_LIBS "Build shared library" ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)          # clock_nanosleep, strdup etc. via _GNU_SOURCE below

if(CARI_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(CARI_SOURCES
    src/tables.c
    src/frame.c
    src/caps.c
    src/samples.c
    src/flacframe.c
)

set(CARI_HAVE_FLAC 0)
if(CARI_WITH_FLAC)
    pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
    list(APPEND CARI_SOURCES src/flac.c)
    set(CARI_HAVE_FLAC 1)
endif()

set(CARI_HAVE_ZMQ 0)
if(CARI_WITH_ZMQ)
    pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
    list(APPEND CARI_SOURCES src/slave.c src/master.c src/log.c)
    set(CARI_HAVE_ZMQ 1)
endif()

configure_file(cmake/cari_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/cari/cari_config.h @ONLY)

add_library(cari ${CARI_SOURCES})
add_library(cari::cari ALIAS cari)
set_target_properties(cari PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET default)
target_compile_definitions(cari PRIVATE _GNU_SOURCE)
target_compile_options(cari PRIVATE -Wall -Wextra -Wshadow -Wstrict-prototypes)
target_include_directories(cari PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
find_library(MATH_LIB m)
target_link_libraries(cari PRIVATE ${MATH_LIB})
set(CARI_PC_REQUIRES "")
if(CARI_WITH_FLAC)
    target_link_libraries(cari PRIVATE PkgConfig::FLAC)
    string(APPEND CARI_PC_REQUIRES " flac")
endif()
if(CARI_WITH_ZMQ)
    target_link_libraries(cari PUBLIC PkgConfig::ZMQ)
    find_package(Threads REQUIRED)
    target_link_libraries(cari PUBLIC Threads::Threads)
    string(APPEND CARI_PC_REQUIRES " libzmq")
endif()

# ---------------------------------------------------------------- install
install(TARGETS cari EXPORT cariTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
set(CARI_HEADERS include/cari/cari_proto.h)
if(CARI_WITH_FLAC)
    list(APPEND CARI_HEADERS include/cari/cari_flac.h)
endif()
if(CARI_WITH_ZMQ)
    list(APPEND CARI_HEADERS include/cari/cari_slave.h include/cari/cari_master.h include/cari/cari_log.h)
endif()
install(FILES ${CARI_HEADERS} ${CMAKE_CURRENT_BINARY_DIR}/include/cari/cari_config.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cari)

include(CMakePackageConfigHelpers)
install(EXPORT cariTargets NAMESPACE cari:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cari)
configure_package_config_file(cmake/cariConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cariConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cari)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/cariConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cariConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/cariConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cari)
configure_file(cmake/cari.pc.in ${CMAKE_CURRENT_BINARY_DIR}/cari.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cari.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ---------------------------------------------------------------- tests
if(CARI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
