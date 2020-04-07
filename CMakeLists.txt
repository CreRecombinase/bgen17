cmake_minimum_required (VERSION 3.15)
project (bgen17 CXX)
set(default_build_type "Release")
set(CMAKE_CXX_STANDARD 11)
include(FetchContent)
include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/modules/")
set(BGEN_MAJOR_VERSION 0)
set(BGEN_MINOR_VERSION 1)
set(BGEN_PATCH_VERSION 1)
set(BGEN_VERSION
  ${BGEN_MAJOR_VERSION}.${BGEN_MINOR_VERSION}.${BGEN_PATCH_VERSION})



SET(CMAKE_SKIP_BUILD_RPATH  FALSE)
# when building, don't use the install RPATH already
# (but later on when installing)
SET(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

# add the automatically determined parts of the RPATH
# which point to directories outside the build tree to the install RPATH
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# find_package(PkgConfig)
# find_package(PkgConfig QUIET)
# pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET GLOBAL libzstd)

FetchContent_Declare(
  zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  SOURCE_SUBDIR "build/cmake/"
  )

# FetchContent_Declare(
#   bzip2
#   https://github.com/sergiud/bzip2
#   )


FetchContent_Declare(
  sqlite3
  GIT_REPOSITORY https://github.com/CreRecombinase/sqlite-amalgamation
  )
add_subdirectory(boost-cmake)


FetchContent_GetProperties(zstd)
if(NOT zstd_POPULATED)
  FetchContent_Populate(zstd)
  add_subdirectory("${zstd_SOURCE_DIR}/build/cmake" EXCLUDE_FROM_ALL)
endif()

target_include_directories(libzstd_static PUBLIC $<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib> $<INSTALL_INTERFACE:include>)
set_target_properties(libzstd_static PROPERTIES PUBLIC_HEADER "${zstd_SOURCE_DIR}/lib/zstd.h")

FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
  FetchContent_Populate(sqlite3)
  add_subdirectory(${sqlite3_SOURCE_DIR} ${sqlite3_BINARY_DIR})
endif()



# the RPATH to be used when installing, but only if it's not a system directory
LIST(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${CMAKE_INSTALL_PREFIX}/lib" isSystemDir)
IF("${isSystemDir}" STREQUAL "-1")
   SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
ENDIF("${isSystemDir}" STREQUAL "-1")


find_package(ZLIB REQUIRED)
# find_package(BZip2 REQUIRED)



add_library(sqlitecpp OBJECT src/Connection.cpp src/Error.cpp src/SQLite3Connection.cpp src/SQLite3Statement.cpp src/SQLStatement.cpp include/db/Connection.hpp include/db/Error.hpp include/db/SQLite3Connection.hpp include/db/SQLite3Error.hpp include/db/sqlite3.hpp include/db/SQLite3Statement.hpp include/db/SQLStatement.hpp include/db/Transaction.hpp)

set_target_properties(sqlitecpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(sqlitecpp
  INTERFACE BOOST_THREAD_USES_DATETIME
  INTERFACE SQLITE_ENABLE_COLUMN_METADATA
  INTERFACE SQLITE_ENABLE_STAT4
  INTERFACE SQLITE_MAX_EXPR_DEPTH=10000
  INTERFACE SQLITE_USE_URI=1)
target_link_libraries(sqlitecpp PRIVATE SQLite3)
target_link_libraries(sqlitecpp PRIVATE Boost::boost)
target_include_directories(sqlitecpp PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)


add_library(bgen src/bgen.cpp src/IndexQuery.cpp src/MissingValue.cpp src/View.cpp src/zlib.cpp include/genfile/bgen.hpp include/genfile/IndexQuery.hpp include/genfile/View.hpp)

target_compile_definitions(bgen
  INTERFACE BOOST_THREAD_USES_DATETIME)
target_link_libraries(bgen
  PUBLIC Boost::date_time
  PUBLIC Boost::timer
  PUBLIC Boost::filesystem
  PUBLIC Boost::thread)
set_target_properties(bgen PROPERTIES PUBLIC_HEADER "include/genfile/bgen.hpp;include/genfile/IndexQuery.hpp;include/genfile/View.hpp;include/genfile/MissingValue.hpp;include/genfile/types.hpp;include/genfile/zlib.hpp")
target_link_libraries(bgen PUBLIC ZLIB::ZLIB)
target_link_libraries(bgen PUBLIC libzstd_static)
target_include_directories(bgen PUBLIC $<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib> $<INSTALL_INTERFACE:include>)
target_include_directories(bgen PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(bgen PRIVATE sqlitecpp)

# set(CMAKE_CXX_CLANG_TIDY clang-tidy -checks=-*,readability-*)
add_library(bgenapp OBJECT src/ApplicationContext.cpp src/CmdLineUIContext.cpp src/get_current_time_as_string.cpp src/OptionProcessor.cpp src/progress_bar.cpp src/Timer.cpp src/CmdLineOptionProcessor.cpp src/OptionDefinition.cpp src/OstreamTee.cpp src/string_utils.cpp src/UIContext.cpp include/appcontext/appcontext.hpp include/appcontext/CmdLineOptionProcessor.hpp include/appcontext/get_current_time_as_string.hpp include/appcontext/OptionDefinition.hpp include/appcontext/OstreamTee.hpp include/appcontext/progress_bar.hpp include/appcontext/Timer.hpp include/appcontext/ApplicationContext.hpp include/appcontext/CmdLineUIContext.hpp include/appcontext/null_ostream.hpp include/appcontext/OptionProcessor.hpp include/appcontext/ProgramFlow.hpp include/appcontext/string_utils.hpp include/appcontext/UIContext.hpp)

target_link_libraries(bgenapp
  PUBLIC Boost::boost
  PUBLIC Boost::timer)
target_include_directories(bgenapp PRIVATE include)


add_executable(cat-bgen apps/cat-bgen.cpp)
target_link_libraries(cat-bgen PRIVATE bgenapp PUBLIC bgen)
target_include_directories(cat-bgen PUBLIC include)

add_executable(edit-bgen apps/edit-bgen.cpp)
target_link_libraries(edit-bgen PRIVATE bgenapp PUBLIC bgen)
target_include_directories(edit-bgen PUBLIC include)

add_executable(bgenix apps/bgenix.cpp)
target_link_libraries(bgenix PRIVATE bgenapp PUBLIC bgen)
target_include_directories(bgenix PUBLIC include)

add_subdirectory("${PROJECT_SOURCE_DIR}/example")

set(DEST_DIR "${CMAKE_INSTALL_PREFIX}")
install(TARGETS sqlitecpp
  EXPORT bgen17
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )

install(TARGETS bgen Boost_date_time Boost_timer Boost_filesystem Boost_thread Boost_chrono libzstd_static
  EXPORT bgen17
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )

install(TARGETS bgenix cat-bgen edit-bgen
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDDIR}"
        PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")


install(EXPORT bgen17
  FILE
    bgen.cmake
  NAMESPACE
    bgen::
  DESTINATION
    ${CMAKE_INSTALL_LIBDIR}/cmake/bgen
    )

set (PKGCONFIG_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
configure_file(bgen17.pc.in "${PROJECT_BINARY_DIR}/libbgen17.pc" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/libbgen17.pc" DESTINATION "${PKGCONFIG_INSTALL_DIR}")
