# 单元测试
#
# 测试目标只链接被测模块自身所需的源文件，不链接整个服务器，
# 以便隔离验证单个模块的行为。

# 与被测目标保持同一套动态检查开关。
# 日志缓冲区越界一类缺陷不会改变可观测的输出，只有在 Sanitizer 下运行
# 单元测试才能被发现，因此这里必须把开关一并传递到测试目标。
if(ENABLE_ASAN)
    set(NOVASERVER_SANITIZER -fsanitize=address -fno-omit-frame-pointer)
elseif(ENABLE_TSAN)
    set(NOVASERVER_SANITIZER -fsanitize=thread)
else()
    set(NOVASERVER_SANITIZER)
endif()

function(add_unit_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE
        GTest::gtest_main
        Threads::Threads
    )
    target_compile_options(${name} PRIVATE -Wall -Wextra ${NOVASERVER_SANITIZER})
    if(NOVASERVER_SANITIZER)
        target_link_options(${name} PRIVATE ${NOVASERVER_SANITIZER})
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_unit_test(unit_tests
    test_block_queue.cpp
    test_locker.cpp
)

add_unit_test(log_tests
    test_log.cpp
    ${CMAKE_SOURCE_DIR}/log/log.cpp
)

# URL 编解码与路径规范化为纯函数，无需链接额外源文件
add_unit_test(url_codec_tests
    test_url_codec.cpp
)

add_unit_test(config_tests
    test_config.cpp
    ${CMAKE_SOURCE_DIR}/config.cpp
)

add_unit_test(password_hash_tests
    test_password_hash.cpp
    ${CMAKE_SOURCE_DIR}/auth/password_hash.cpp
)

# 事件循环与通道：行为只在运行时成立，因此这两个模块的测试要真的跑起循环
add_unit_test(event_loop_tests
    test_event_loop.cpp
    ${CMAKE_SOURCE_DIR}/net/event_loop.cpp
    ${CMAKE_SOURCE_DIR}/net/channel.cpp
    ${CMAKE_SOURCE_DIR}/net/timer_queue.cpp
)

# 这批测试依赖跨线程唤醒，而唤醒一旦失效的表现是「循环永远等不到事件」——
# 即测试挂住而不是断言失败。加超时把这种情况变成一次明确的失败
set_tests_properties(event_loop_tests PROPERTIES TIMEOUT 30)

# 统一事件源：定时器来自 timerfd、信号来自 signalfd，同样只能在运行时验证
add_unit_test(timer_queue_tests
    test_timer_queue.cpp
    ${CMAKE_SOURCE_DIR}/net/event_loop.cpp
    ${CMAKE_SOURCE_DIR}/net/channel.cpp
    ${CMAKE_SOURCE_DIR}/net/timer_queue.cpp
    ${CMAKE_SOURCE_DIR}/net/signal_watcher.cpp
)
set_tests_properties(timer_queue_tests PROPERTIES TIMEOUT 30)
target_link_libraries(password_hash_tests PRIVATE OpenSSL::Crypto)
