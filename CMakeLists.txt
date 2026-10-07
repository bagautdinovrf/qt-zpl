cmake_minimum_required(VERSION 3.25)

project(QtZpl VERSION 0.1.0 LANGUAGES CXX)

option(QTZPL_BUILD_SHARED "Build QtZpl as a shared library" ON)
option(QTZPL_BUILD_TESTS "Build QtZpl tests" ON)
option(QTZPL_BUILD_EXAMPLES "Build QtZpl examples" ON)
option(QTZPL_BUILD_BENCHMARKS "Build opt-in offline performance benchmarks" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui)

set(QTZPL_LIBRARY_TYPE STATIC)
if(QTZPL_BUILD_SHARED)
  set(QTZPL_LIBRARY_TYPE SHARED)
endif()

add_library(QtZpl ${QTZPL_LIBRARY_TYPE}
  resources/fonts/qtzpl_fonts.qrc
  third_party/qrcodegen/qrcodegen.cpp
  src/barcode_encoders.cpp
  src/maxicode_encoder.cpp
  src/parser.cpp
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
set_target_properties(QtZpl PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
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
  COMPATIBILITY SameMajorVersion
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
