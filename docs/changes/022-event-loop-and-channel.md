# 022 · 引入 EventLoop 与 Channel

> 本记录是**阶段三「并发模型重构」的第一批**。这一批只新增基础层，尚未接入主流程——服务端行为与 `021` 基线采集时完全一致。

## 一、背景与动机

阶段三要把「半同步/半反应堆」换成主从 Reactor（one loop per thread）。`ROADMAP` 已列出要引入的六个抽象，本批落地其中最底层的两个：`EventLoop` 与 `Channel`。

先做这两个而非直接改造主流程，理由是它们**可以独立验证**：事件循环与通道的行为全部是运行时时序，只有真的跑起来才能测，而单独成模块时测试只需要一个 `EventLoop` 实例，不牵扯协议解析、数据库与定时器。

## 二、现状分析

现有实现在这两个职责上存在的问题：

| 问题 | 位置 |
|:--|:--|
| **单例 epoll 实例** | `http_conn::m_epollfd` 是静态成员，`webserver.cpp` 赋值一次，7 处 `modfd`/`addfd`/`removefd` 都用它——与「每线程一个 epoll」直接冲突 |
| **事件注册散落在协议层** | `http_conn` 直接调 `epoll_ctl`，协议状态机与 fd 的关注事件耦合在一起 |
| **两份重复的 addfd** | `Utils::addfd`（`timer/lst_timer.cpp`）与自由函数 `addfd`（`http/http_conn.cpp`），逻辑一致但各写一遍 |
| **无跨线程唤醒机制** | 主线程阻塞在 `epoll_wait`，工作线程无法主动让它做任何事——这正是 Reactor 模式下主线程改用「轮询标志位」的原因（`webserver.cpp` 的 `while (true)` 忙等） |

## 三、设计方案

### 归属约定是结构性的

`EventLoop` 的注释里写明了约定：**所有事件处理、连接状态与超时定时器都只在本线程内访问**，跨线程只能经 `run_in_loop` / `queue_in_loop` 投递任务。这条约定成立之后，「同一连接上读写串行」不再需要 `EPOLLONESHOT` 与忙等这类附加机制——它是推论，不是约束。

`run_in_loop` 在本线程直接执行、否则入队；`queue_in_loop` 只入队。两者都在入队后写 eventfd 唤醒。

### Channel 只标识描述符，不拥有它

关闭时机的判断各有归属（连接归它自己、监听归 Acceptor），`Channel` 只保证「关闭之前先把自己从事件表里摘掉」——`EPOLL_CTL_DEL` 必须先于 `close`，否则描述符被内核复用后，一条迟到的删除会作用到新连接上。

关注事件只在**一个方向上**（要么等可读、要么等可写），与原先 `modfd` 的语义一致。反复请求同一关注事件时**先比较再更新**：协议处理在「请求尚未收全」时每次事件都会要求关注可读，每次都发一次 `EPOLL_CTL_MOD` 会多出与请求数等量的系统调用。

### 读先于关闭的分派顺序

`handle_event` 的顺序是「读 → 写 → 关闭」。这是 `020` 那轮验证的直接结论：`EPOLLRDHUP` 只表示对端关闭了写端，接收缓冲区里可能还有最后一个请求，而它通常与 `EPOLLIN` 一起返回。关闭回调放在最后，是因为前面的回调可能已经把连接关掉，而「此刻是否还有未完成的工作」这个判断权在持有者手里。

### `tie`：把生命周期保护放在分派点

回调里可能触发连接关闭，而关闭意味着持有者的最后一个 `shared_ptr` 被释放。若不做处理，`handle_event` 返回后访问的就是已析构的对象。

`Channel::tie(weak_ptr)` 绑定持有者的弱引用，`handle_event` 期间提升为强引用。这样保护只写在**一处**（分派点），而不是要求每个回调都记得先 `shared_from_this()`。类型取 `void` 是为了不依赖持有者的具体类型。

### 唤醒用 eventfd 而不是自管道

现在是 `SIGALRM` + `socketpair`（两个 fd、信号异步）。改用 eventfd：单描述符、固定 8 字节计数语义，读写都比管道简单。这一批只用于跨线程唤醒；`timerfd` 与 `signalfd` 在 3.2 引入。

## 四、实现要点

| 文件 | 内容 |
|:--|:--|
| `net/channel.h` / `.cpp` | `Channel`：关注事件管理（`enable_reading` / `enable_writing` / `disable_*` / `remove`）、回调设置、`tie`、`handle_event` 分派 |
| `net/event_loop.h` / `.cpp` | `EventLoop`：epoll + eventfd、`loop` / `quit` / `run_in_loop` / `queue_in_loop`、`update_channel` / `remove_channel`、待执行任务队列 |
| `tests/test_event_loop.cpp` | 8 个用例 |
| `CMakeLists.txt` | `NOVASERVER_SOURCES` 加入两个 `.cpp`（维持现有构建结构，未抽库） |
| `tests/CMakeLists.txt` | 新增 `event_loop_tests` 目标，并设 `TIMEOUT 30` |

两处细节：

- **`loop()` 不重置退出标志**：循环启动之前调用 `quit()` 是合法的，重置会把那次请求丢掉
- **`do_pending_functors` 先在锁内整块换出、再在锁外执行**：任务本身可能再次 `queue_in_loop`，持锁执行会自锁

## 五、验证

### 单元测试（8 个用例）

| 用例 | 覆盖点 |
|:--|:--|
| `RunInLoopExecutesImmediatelyOnOwnerThread` | 未调用 `loop()` 也算本线程（归属按线程判定，不按循环是否在跑） |
| `QueueInLoopFromOtherThreadWakesUpLoop` | 跨线程投递 + eventfd 唤醒 |
| `QuitFromOtherThreadStopsLoop` | 跨线程 `quit` 能终止阻塞中的循环 |
| `ChannelDispatchesReadEvent` | 读事件分派（真实管道 + 另一线程写入） |
| `ChannelDispatchesWriteEvent` | 写事件分派 |
| `EventIsDroppedAfterOwnerDestroyed` | `tie` 的持有者先销毁时，事件被安静丢弃 |
| `OwnerSurvivesCallbackThatReleasesIt` | 回调里释放最后一个强引用，对象仍活到回调返回 |
| `EnableReadingDoesNotRepeatEpollCtl` | 重复请求同一关注事件不改变状态 |

### 负验证：确认测试不是恰好通过

把 `wakeup()` 临时改成空函数，`QueueInLoopFromOtherThreadWakesUpLoop` 与 `QuitFromOtherThreadStopsLoop` **立即挂住**——说明这两个用例确实在验证唤醒，而不是因为时序巧合总是通过。

但「挂住」不是好的失败模式：CI 里会一直卡着而不是明确失败。因此给该测试目标加了 `TIMEOUT 30`，把这种情况变成一次可读的失败。**这是加超时的原因，不是随手设的保险。**

### TSan

```
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DCMAKE_PREFIX_PATH=/tmp/gtest-local/usr
cmake --build build-tsan --target event_loop_tests
setarch -R ./build-tsan/tests/event_loop_tests
```

8/8 通过，**无任何数据竞争报告**。

`setarch -R` 是必需的：Ubuntu 22.04 之后的高熵 ASLR 会让 TSan 启动即报 `FATAL: ThreadSanitizer: unexpected memory mapping`。另一种解法是 `sysctl vm.mmap_rnd_bits=28`，但那是全局设置且需要 root，测试里用 `setarch -R` 更合适。

### 回归

| 项 | 结果 |
|:--|:--|
| `ctest` | 6/6 通过（新增 `event_loop_tests`） |
| Release 与 TSan 双构建 | 0 错误 0 警告 |
| `clang-format --dry-run --Werror` | 无差异 |
| 服务端行为 | 未改变（这一批未接入主流程） |

## 六、遗留

1. **`Channel` 的析构不摘除事件表项**：摘除需要访问 `EventLoop`，而析构未必早于它。当前依赖使用方在销毁前自行调用 `remove()`。3.3 接入 `TcpConnection` 时会看到这条约定的实际约束力，届时若容易出错，应改为更强的机制
2. **`epoll_wait` 用无限超时**：加入 `TimerQueue`（3.2）后仍然正确（定时器到时会通过 timerfd 唤醒），但「循环完全依赖事件驱动」这一点在 3.2 需要重新确认
3. **`update_channel` 的失败只打日志**：`epoll_ctl` 失败没有向上传递，调用方无从得知。当前认为这是不可恢复的内部错误，接入主流程时若有可恢复的情形需要重新考虑
