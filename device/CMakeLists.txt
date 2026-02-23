cmake_minimum_required(VERSION 3.16)
project(edge_agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === HAL Camera Backend ===
set(HAL_CAMERA_BACKEND "FILE" CACHE STRING "Camera HAL backend: FILE or OPENCV")
set_property(CACHE HAL_CAMERA_BACKEND PROPERTY STRINGS FILE OPENCV)

if(HAL_CAMERA_BACKEND STREQUAL "OPENCV")
    set(HAL_SOURCE hal/opencv_frame_source.cpp)
    add_compile_definitions(HAL_USE_OPENCV)
elseif(HAL_CAMERA_BACKEND STREQUAL "FILE")
    set(HAL_SOURCE hal/file_frame_source.cpp)
    add_compile_definitions(HAL_USE_FILE)
else()
    message(FATAL_ERROR "Unknown HAL_CAMERA_BACKEND: ${HAL_CAMERA_BACKEND} (expected FILE or OPENCV)")
endif()
message(STATUS "HAL Camera Backend: ${HAL_CAMERA_BACKEND}")

# === TFLite (from source) ===
set(TFLITE_SOURCE_DIR "/tf" CACHE PATH "TensorFlow source directory")
if(NOT EXISTS "${TFLITE_SOURCE_DIR}/tensorflow/lite/CMakeLists.txt")
    message(FATAL_ERROR "TFLITE_SOURCE_DIR invalid: ${TFLITE_SOURCE_DIR}")
endif()
add_subdirectory(
    "${TFLITE_SOURCE_DIR}/tensorflow/lite"
    "${CMAKE_CURRENT_BINARY_DIR}/tflite"
    EXCLUDE_FROM_ALL
)

# === OpenCV ===
set(OPENCV_COMPONENTS core imgproc imgcodecs)
if(HAL_CAMERA_BACKEND STREQUAL "OPENCV")
    list(APPEND OPENCV_COMPONENTS videoio)
endif()
find_package(OpenCV REQUIRED COMPONENTS ${OPENCV_COMPONENTS})

# === gRPC + Protobuf ===
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
pkg_check_modules(GRPC REQUIRED grpc++ grpc)

# Find grpc_cpp_plugin
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
endif()

# === Generate proto stubs ===
set(PROTO_DIR "${CMAKE_SOURCE_DIR}/../proto")
set(PROTO_FILE "${PROTO_DIR}/edge_ai.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

add_custom_command(
    OUTPUT
        "${PROTO_GEN_DIR}/edge_ai.pb.cc"
        "${PROTO_GEN_DIR}/edge_ai.pb.h"
        "${PROTO_GEN_DIR}/edge_ai.grpc.pb.cc"
        "${PROTO_GEN_DIR}/edge_ai.grpc.pb.h"
    COMMAND protoc
        -I "${PROTO_DIR}"
        --cpp_out="${PROTO_GEN_DIR}"
        --grpc_out="${PROTO_GEN_DIR}"
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating gRPC C++ stubs"
)

# === Edge Agent ===
add_executable(edge_agent
    src/main.cpp
    src/preprocess.cpp
    src/inference.cpp
    src/postprocess.cpp
    src/grpc_client.cpp
    src/pipeline.cpp
    ${HAL_SOURCE}
    ${PROTO_GEN_DIR}/edge_ai.pb.cc
    ${PROTO_GEN_DIR}/edge_ai.grpc.pb.cc
)

target_include_directories(edge_agent PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${PROTO_GEN_DIR}
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
)

target_link_libraries(edge_agent PRIVATE
    tensorflow-lite
    ${OpenCV_LIBS}
    ${PROTOBUF_LIBRARIES}
    ${GRPC_LIBRARIES}
    pthread
)
