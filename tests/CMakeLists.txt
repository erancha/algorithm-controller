# Each test carries a ctest label naming its layer, so scripts/test.sh can run
# layers selectively (ctest -L): "unit" tests exercise one component in-process,
# "service" tests bring up in-process gRPC servers, and "e2e" launches the full
# five-process pipeline.
function(add_layer_test name layer)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN} GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS ${layer})
endfunction()

add_layer_test(smoke_test unit)
add_layer_test(pgm_test unit common)
add_layer_test(proto_test unit proto_lib)
add_layer_test(free_list_test unit common)
add_layer_test(shm_segment_test unit membox)
add_layer_test(membox_test unit membox)
add_layer_test(fixtures_test unit camera_lib)
add_layer_test(position_score_test unit algorithm)

add_layer_test(camera_service_test service camera_lib)
add_layer_test(node_service_test service algorithm)
add_layer_test(controller_service_test service membox proto_lib)

add_test(NAME e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e.sh ${CMAKE_BINARY_DIR})
set_tests_properties(e2e PROPERTIES TIMEOUT 120 LABELS e2e)
