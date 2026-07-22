add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE GTest::gtest_main)
add_test(NAME smoke_test COMMAND smoke_test)

add_executable(pgm_test pgm_test.cpp)
target_link_libraries(pgm_test PRIVATE common GTest::gtest_main)
add_test(NAME pgm_test COMMAND pgm_test)

add_executable(proto_test proto_test.cpp)
target_link_libraries(proto_test PRIVATE proto_lib GTest::gtest_main)
add_test(NAME proto_test COMMAND proto_test)

add_executable(free_list_test free_list_test.cpp)
target_link_libraries(free_list_test PRIVATE common GTest::gtest_main)
add_test(NAME free_list_test COMMAND free_list_test)

add_executable(shm_segment_test shm_segment_test.cpp)
target_link_libraries(shm_segment_test PRIVATE membox GTest::gtest_main)
add_test(NAME shm_segment_test COMMAND shm_segment_test)

add_executable(membox_test membox_test.cpp)
target_link_libraries(membox_test PRIVATE membox GTest::gtest_main)
add_test(NAME membox_test COMMAND membox_test)

add_executable(fixtures_test fixtures_test.cpp)
target_link_libraries(fixtures_test PRIVATE camera_lib GTest::gtest_main)
add_test(NAME fixtures_test COMMAND fixtures_test)

add_executable(position_score_test position_score_test.cpp)
target_link_libraries(position_score_test PRIVATE algorithm GTest::gtest_main)
add_test(NAME position_score_test COMMAND position_score_test)

add_executable(camera_service_test camera_service_test.cpp)
target_link_libraries(camera_service_test PRIVATE camera_lib GTest::gtest_main)
add_test(NAME camera_service_test COMMAND camera_service_test)

add_executable(node_service_test node_service_test.cpp)
target_link_libraries(node_service_test PRIVATE algorithm GTest::gtest_main)
add_test(NAME node_service_test COMMAND node_service_test)

add_executable(controller_service_test controller_service_test.cpp)
target_link_libraries(controller_service_test PRIVATE membox proto_lib GTest::gtest_main)
add_test(NAME controller_service_test COMMAND controller_service_test)

add_test(NAME e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e.sh ${CMAKE_BINARY_DIR})
set_tests_properties(e2e PROPERTIES TIMEOUT 120)
