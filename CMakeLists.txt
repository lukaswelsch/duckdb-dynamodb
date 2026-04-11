cmake_minimum_required(VERSION 3.21)

# Set extension name here
set(TARGET_NAME dynamodb)
project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/dynamodb_extension.cpp)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})


set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

target_include_directories(${TARGET_NAME}_extension PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

# Link aws-sdk-cpp
find_package(AWSSDK CONFIG COMPONENTS core dynamodb REQUIRED)

message(STATUS "AWSSDK_LINK_LIBRARIES = ${AWSSDK_LINK_LIBRARIES}")
message(STATUS "AWSSDK_INCLUDE_DIRS = ${AWSSDK_INCLUDE_DIRS}")

target_link_libraries(${EXTENSION_NAME}
        aws-cpp-sdk-core
        aws-cpp-sdk-dynamodb
)

target_link_libraries(${LOADABLE_EXTENSION_NAME}
        aws-cpp-sdk-core
        aws-cpp-sdk-dynamodb
)

install(
  TARGETS ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
