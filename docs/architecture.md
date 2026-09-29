# 系统架构

本文档描述 NovaServer 当前的并发模型与模块结构。旧架构（半同步/半反应堆）的描述保留在 [ROADMAP.md](ROADMAP.md) 第三节，用于说明各项改动的动机。

## 一、并发模型

采用**主从 Reactor（one loop per thread）**：

- **主线程**只负责 accept。它持有自己的 `EventLoop`，其上注册监听套接字与信号接管
- **子 Reactor 线程**各持有一个独立的 `EventLoop`（含独立的 epoll 实例），承担已建立连接的读写与协议处理
- 新连接按**轮转策略**分发给子线程，一个连接自建立至销毁都归属同一个线程

核心约束是：**连接的文件描述符、协议解析状态与其超时定时器全部归属所属线程**。跨线程只传递一次连接对象本身（构造时投递）。这条约束成立之后，「同一连接上的读写串行」是结构推论，不需要 `EPOLLONESHOT` 之类的附加机制。

```
                    ┌───────────────────────────┐
   监听套接字 ──────▶│  主线程 EventLoop          │
                    │  · Acceptor（监听）        │
                    │  · SignalWatcher（退出信号）│
                    └───────────┬───────────────┘
                                │ run_in_loop（按轮转投递）
              ┌─────────────────┼─────────────────┐
              ▼                 ▼                 ▼
     ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
     │ 子线程 EventLoop│ │ 子线程 EventLoop│ │ 子线程 EventLoop│
     │ · epoll        │ │ · epoll        │ │ · epoll        │
     │ · TimerQueue   │ │ · TimerQueue   │ │ · TimerQueue   │
     │ · TcpConnection│ │ · TcpConnection│ │ · TcpConnection│
     └────────────────┘ └────────────────┘ └────────────────┘
```

`-t 0` 是合法的退化配置：不创建子线程，全部连接归主循环，用于与多线程分发做对照。

## 二、模块结构

| 模块 | 文件 | 职责 |
|:--|:--|:--|
| `EventLoop` | `net/event_loop.{h,cpp}` | 单线程事件循环：epoll 实例、eventfd 唤醒、跨线程待执行任务队列、连接注册表 |
| `Channel` | `net/channel.{h,cpp}` | 文件描述符与关注事件的绑定，维护 `epoll_ctl` 的 ADD/MOD/DEL 状态 |
| `Acceptor` | `net/acceptor.{h,cpp}` | 监听套接字、接受新连接、描述符耗尽时的兜底 |
| `TcpConnection` | `net/tcp_connection.{h,cpp}` | 连接生命周期、读写处理、持有协议对象与空闲定时器 |
| `EventLoopThreadPool` | `net/event_loop_thread_pool.{h,cpp}` | 子 Reactor 线程池与轮转分配 |
| `TimerQueue` | `net/timer_queue.{h,cpp}` | 基于 timerfd 的定时器队列 |
| `SignalWatcher` | `net/signal_watcher.{h,cpp}` | 以 signalfd 接管退出信号，屏蔽信号处理函数 |
| `WebServer` | `webserver.{h,cpp}` | 服务器主类：装配上述组件，承担 `on_new_connection` 与 `on_connection_closed` |
| `http_conn` | `http/http_conn.{h,cpp}` | HTTP 状态机解析与响应组装（不感知事件循环） |
| `Config` | `config.{h,cpp}` | 三层配置来源：内置默认值 < 配置文件 < 命令行 |

## 三、事件源的统一

除 epoll 之外，进程里所有需要等待的外部输入都收敛为**文件描述符**：

| 需求 | 手段 | 取代 |
|:--|:--|:--|
| 事件通知 | `epoll` | — |
| 空闲连接回收 | `timerfd` | `SIGALRM` + socketpair |
| 跨线程唤醒 | `eventfd` | 自管道 |
| 进程退出信号 | `signalfd` | `signal()` 注册处理函数 |

这样做的收益是：**没有信号处理函数**，所有回调都在正常的线程上下文与栈上执行，不必受异步信号安全函数的限制；定时精度也不再受 `SIGALRM` 的秒级粒度约束。

退出路径中有一处顺序约束：**`block_signals` 必须在创建任何线程之前调用**。信号掩码是线程属性，子线程继承创建时的掩码；先建线程再屏蔽，那些线程不会被屏蔽，`SIGTERM` 可能被它们按默认行为处理掉（进程直接退出，而非走优雅退出）。

## 四、连接的生命周期

### 建立

`Acceptor::handle_read` 在主线程接受连接后，通过回调把 `connfd` 与对端地址交给 `WebServer::on_new_connection`；后者用 `EventLoopThreadPool::next_loop()` 取到目标循环，再以 `run_in_loop` 把「构造连接」这一步投递过去。**跨线程传递的只有描述符与地址这一次**，连接的构造、注册与启动都发生在它归属的线程内。

### 归属

`TcpConnection` 由 `std::shared_ptr` 持有，同时登记在所属 `EventLoop` 的连接注册表中。注册表只在循环线程内访问，因此无需加锁。

事件回调的存活由 `Channel::tie` 保证：Channel 持有持有者的**弱引用**，`handle_event` 在派发前先提升为强引用。关闭连接的回调正是从 `handle_event` 内部触发的，而关闭往往意味着持有者的最后一个 `shared_ptr` 被释放——没有这层保护，回调返回后访问的就是已析构的对象。

### 关闭

释放动作收敛到**唯一出口** `TcpConnection::release_fd()`，且保证**幂等**。五个触发点（读失败、处理失败、写失败、对端关闭、空闲超时）都汇聚到 `close()`，其中 `m_closed` 标志使重复调用成为空操作。

从注册表中移除采用**延迟擦除**：`remove_connection` 把擦除动作投递到任务队列，由循环在下一轮取出执行。若在回调栈内直接擦除，注册表持有的那份引用（也可能是最后一份）会在回调返回前析构连接对象。

空闲定时器持有连接的**弱引用**。这一点很关键：连接先于定时器销毁时，回调取到空指针直接丢弃，而不会访问已释放的内存。

## 五、一次请求的事件流

```
主线程
  epoll_wait → 监听套接字可读 → Acceptor::handle_read → accept
      └─ on_new_connection → next_loop() → run_in_loop(投递)

子线程
  任务队列 → 构造 TcpConnection → 登记进连接表 → start()（注册描述符、装载空闲定时器）
      └─ 读事件 → handle_read → http_conn::read_once → process_read（状态机解析）
                                                       └─ process_write（组装响应 / mmap 静态文件）
      └─ 写事件 → handle_write → http_conn::write（writev 发送）
                                 └─ 完成后：短连接关闭，长连接复位状态等待下一个请求
```

`when` 的关注事件切换由协议层经 `set_event_notifier` 注入的两个回调表达（「接下来关心可读」/「关心可写」），实际的 `epoll_ctl` 由 `Channel` 完成。这样协议层不直接操作 epoll。

## 六、与旧架构的关键差异

| 维度 | 旧（半同步/半反应堆） | 现在 |
|:--|:--|:--|
| 连接归属 | 读写事件可能由不同工作线程处理 | 连接全程归属同一线程 |
| 重复投递抑制 | 依赖 `EPOLLONESHOT` | 结构上不需要 |
| 完成确认 | 主线程轮询标志位（CPU 空转） | 无 |
| 定时器 | 单条升序链表 + `SIGALRM`，跨线程访问 | 每线程一个 timerfd 队列，只在本线程访问 |
| 连接对象 | 按 `MAX_FD` 预分配 | 按需 `make_shared` |
| 唤醒 | 自管道 | `eventfd` |

## 七、并发正确性

本模型在 ThreadSanitizer 下经过三组场景验证：连接的建立/关闭风暴、空闲超时与主动关闭同时发生、日志的跨线程写入。单元测试（其中 `event_loop_tests` 与 `timer_queue_tests` 会真的跑起事件循环、跨线程投递任务）在 TSan 下无数据竞争报告。

验证过程中发现并修复了三处问题，都与「跨线程或对象复用」有关：`http_conn` 构造函数未初始化文件映射指针，导致复用的连接对陈旧地址反复 `munmap`；日志依赖非线程安全的 `localtime`；`m_fp` 的判空位于锁外。详见 [changes/029-uninit-mapping-crash.md](changes/029-uninit-mapping-crash.md)。
