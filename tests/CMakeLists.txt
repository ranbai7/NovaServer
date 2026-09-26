# 单元测试
#
# 测试目标只链接被测模块自身所需的源文件，不链接整个服务器，
# 以便隔离验证单个模块的行为。

add_executable(unit_tests
    test_block_queue.cpp
    test_locker.cpp
)

target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    Threads::Threads
)

target_compile_options(unit_tests PRIVATE -Wall -Wextra)

add_test(NAME unit_tests COMMAND unit_tests)
