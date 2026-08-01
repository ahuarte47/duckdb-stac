cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME stac)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)
find_package(LibXml2 REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

if(EMSCRIPTEN)
  # _LINKED_LIBS influences only Wasm compilation it's unclear why this is
  # needed, but somehow some global symbols are not properly
  # exported otherwise this solves by basically re-linking (at the moment the
  # Wasm binary is actually produced)
  set(DUCKDB_EXTENSION_STAC_LINKED_LIBS
      "../../vcpkg_installed/wasm32-emscripten/lib/lib*.a")
endif()

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
if(NOT EMSCRIPTEN)
  include_directories(${CMAKE_SOURCE_DIR}/third_party/httplib)
endif()

set(EXTENSION_SOURCES src/stac_extension.cpp)
add_subdirectory(src/stac)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
# Link LibXml2 in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} LibXml2::LibXml2)
target_link_libraries(${LOADABLE_EXTENSION_NAME} LibXml2::LibXml2)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
