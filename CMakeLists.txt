cmake_minimum_required(VERSION 3.25)

project(QtZpl VERSION 0.2.0 LANGUAGES CXX)

option(QTZPL_BUILD_SHARED "Build QtZpl as a shared library" ON)
option(QTZPL_BUILD_TESTS "Build QtZpl tests" ON)
option(QTZPL_BUILD_EXAMPLES "Build QtZpl examples" ON)
option(QTZPL_BUILD_BENCHMARKS "Build opt-in offline performance benchmarks" OFF)

# CMake 3.30 describes MSVC's latest dialect as CXX23. Keep that metadata
# while explicitly selecting the installed compiler's latest language mode.
if(MSVC)
  set(CMAKE_CXX_STANDARD 23)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>")
else()
  set(_qtzpl_cxx_standards ${CMAKE_CXX_COMPILE_FEATURES})
  list(FILTER _qtzpl_cxx_standards INCLUDE REGEX "^cxx_std_[0-9]+$")
  list(REMOVE_ITEM _qtzpl_cxx_standards cxx_std_98)
  list(SORT _qtzpl_cxx_standards COMPARE NATURAL)
  list(GET _qtzpl_cxx_standards -1 _qtzpl_latest_cxx_standard)
  string(REPLACE "cxx_std_" "" CMAKE_CXX_STANDARD "${_qtzpl_latest_cxx_standard}")
  if(CMAKE_CXX_STANDARD LESS 23)
    message(FATAL_ERROR "QtZpl requires a compiler and CMake with C++23 or newer support.")
  endif()
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui)

# Bounded Z64 inflation uses the same zlib implementation as Qt Core where
# available. System-Qt builds use the system development package instead.
find_package(Qt6ZlibPrivate QUIET)
if(TARGET Qt6::ZlibPrivate)
  set(QTZPL_ZLIB_BUNDLED ON)
  set(QTZPL_ZLIB_TARGET Qt6::ZlibPrivate)
else()
  find_package(ZLIB REQUIRED)
  set(QTZPL_ZLIB_BUNDLED OFF)
  set(QTZPL_ZLIB_TARGET ZLIB::ZLIB)
endif()

# Use Qt's own FreeType when available, keeping Windows free of another DLL.
# Unix Qt packages may instead use their system FreeType.
find_package(Qt6FreetypePrivate QUIET)
find_package(Qt6BundledFreetype QUIET)
if(TARGET Qt6::FreetypePrivate AND TARGET Qt6::BundledFreetype)
  set(QTZPL_FREETYPE_BUNDLED ON)
  set(QTZPL_FREETYPE_TARGETS Qt6::FreetypePrivate Qt6::BundledFreetype)
else()
  find_package(Freetype REQUIRED)
  set(QTZPL_FREETYPE_BUNDLED OFF)
  set(QTZPL_FREETYPE_TARGETS Freetype::Freetype)
endif()

set(QTZPL_LIBRARY_TYPE STATIC)
if(QTZPL_BUILD_SHARED)
  set(QTZPL_LIBRARY_TYPE SHARED)
endif()

add_library(QtZpl ${QTZPL_LIBRARY_TYPE}
  resources/fonts/qtzpl_fonts.qrc
  third_party/qrcodegen/qrcodegen.cpp
  src/barcode_encoders.cpp
  src/linear_encoders.cpp
  src/aztec_encoder.cpp
  src/databar_encoder.cpp
  src/micropdf417_encoder.cpp
  src/maxicode_encoder.cpp
  src/parser.cpp
  src/graphic_decoder.cpp
  src/pdf417_encoder.cpp
  src/renderer.cpp
)
add_library(QtZpl::QtZpl ALIAS QtZpl)

target_include_directories(QtZpl
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(QtZpl PRIVATE third_party/qrcodegen)
target_compile_definitions(QtZpl PRIVATE QTZPL_BUILDING_LIBRARY)
if(NOT QTZPL_BUILD_SHARED)
  target_compile_definitions(QtZpl PUBLIC QTZPL_STATIC)
endif()
target_link_libraries(QtZpl PUBLIC Qt6::Core Qt6::Gui)
target_link_libraries(QtZpl PRIVATE ${QTZPL_FREETYPE_TARGETS})
target_link_libraries(QtZpl PRIVATE ${QTZPL_ZLIB_TARGET})
set_target_properties(QtZpl PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
  EXPORT_NAME QtZpl
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS QtZpl EXPORT QtZplTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES src/databar-ZINT-LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses)
install(DIRECTORY third_party/zxing-linear/ third_party/zxing-aztec/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses/apache-2.0 FILES_MATCHING PATTERN "LICENSE")
install(FILES third_party/zxing-linear/NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses/zxing-linear)
install(FILES third_party/zxing-aztec/NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses/zxing-aztec)
install(FILES third_party/zint-raster/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses/zint-raster)
install(FILES third_party/okapi-maxicode/LICENSE third_party/okapi-maxicode/NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses/okapi-maxicode)
install(FILES third_party/ocrb/LICENSE third_party/ocrb/NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses/ocrb)
install(FILES third_party/font-a-native/LICENSE third_party/font-a-native/NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses/font-a-native)
install(FILES third_party/zxing-datamatrix/LICENSE third_party/zxing-datamatrix/NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/QtZpl/licenses/zxing-datamatrix)
install(EXPORT QtZplTargets
  FILE QtZplTargets.cmake
  NAMESPACE QtZpl::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QtZpl
)
configure_package_config_file(cmake/QtZplConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/QtZplConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QtZpl
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/QtZplConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/QtZplConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/QtZplConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QtZpl
)

if(QTZPL_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 6.11 REQUIRED COMPONENTS Test)
  add_subdirectory(tests)
endif()

if(QTZPL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(QTZPL_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
