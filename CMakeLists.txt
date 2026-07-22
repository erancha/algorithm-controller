cmake_minimum_required(VERSION 3.25)
project(algorithm_controller CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
find_package(GTest CONFIG REQUIRED)
add_library(common src/common/pgm.cpp)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_library(proto_lib src/proto/inspection.proto)
target_link_libraries(proto_lib PUBLIC gRPC::grpc++ protobuf::libprotobuf)
target_include_directories(proto_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
# IMPORT_DIRS anchors the include path at the proto's own directory so protoc emits
# flat inspection.pb.h / inspection.grpc.pb.h under proto_gen instead of nesting them
# under proto_gen/src/proto, matching the #include "inspection.grpc.pb.h" form used
# by dependent targets.
protobuf_generate(TARGET proto_lib LANGUAGE cpp
                  IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/proto
                  PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
protobuf_generate(TARGET proto_lib LANGUAGE grpc
                  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
                  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
                  IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/proto
                  PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)

add_library(membox src/membox/shm_segment.cpp)
target_include_directories(membox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(membox PUBLIC rt)

target_sources(membox PRIVATE src/membox/membox.cpp)
target_link_libraries(membox PUBLIC proto_lib)
add_executable(membox-manager src/membox/manager_main.cpp)
target_link_libraries(membox-manager PRIVATE membox)

add_library(camera_lib src/camera/fixtures.cpp)
target_link_libraries(camera_lib PUBLIC common)
target_link_libraries(camera_lib PUBLIC membox proto_lib)
add_executable(camera src/camera/camera_main.cpp)
target_link_libraries(camera PRIVATE camera_lib)

add_library(algorithm src/compute_node/algorithm/position_score.cpp)
target_include_directories(algorithm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(algorithm PUBLIC membox proto_lib)
add_executable(compute-node src/compute_node/node_main.cpp)
target_link_libraries(compute-node PRIVATE algorithm)

add_executable(controller src/controller/controller_main.cpp)
target_link_libraries(controller PRIVATE membox proto_lib)

add_executable(tool-driver src/driver/driver_main.cpp)
target_link_libraries(tool-driver PRIVATE proto_lib common)

add_subdirectory(tests)
