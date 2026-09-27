# 023 · 用 timerfd 与 signalfd 统一事件源

> 本记录是**阶段三「并发模型重构」的第二批**，承接 `022`。这一批同样未接入主流程，服务端行为不变。

## 一、背景与动机

`ROADMAP` 把「统一事件源」列为阶段三的配套改动。现有实现里有两类事件不经过 epoll：

| 事件 | 现有做法 |
|:--|:--|
| 定时 | `alarm(TIMESLOT)` → `SIGALRM` → 信号处理函数往 `socketpair` 写一个字节 → epoll 命中管道读端 |
| 进程退出 | `SIGTERM` 走同一条管道 |

两者的共同问题是**信号处理函数**：它运行在异步上下文里，能安全调用的函数极其有限。现有实现里那个函数能做的事只有「往管道写一个字节」——这正是这种限制的体现。定时精度也因此被绑死在 `TIMESLOT`（5 秒）上。

换成 `timerfd` 与 `signalfd` 之后，两类事件都变成普通的可读事件，处理逻辑回到事件循环里，不再有异步上下文的约束。

## 二、现状分析

### 定时器

`sort_timer_lst` 是一个按到期时间升序的双向链表，每个连接一个 `util_timer`。整点由 `alarm` 驱动，每 5 秒全表扫描一次。

`020` 的验证暴露了这套结构在释放路径上的问题（重复回收、悬垂指针），但那属于「谁负责关闭连接」的范畴，与定时器本身是两件事。这一批只替换**事件源与数据结构**，定时器与连接的协作方式要到 3.3 引入 `TcpConnection` 时才重新安排。

### 信号

`SIGTERM` 与 `SIGALRM` 混在同一个处理函数里，靠管道里那个字节的值区分。`SIGPIPE` 则是 `SIG_IGN`。

## 三、设计方案

### TimerQueue：timerfd + 无序表

| 选择 | 理由 |
|:--|:--|
| 每个事件循环一份 | 定时器因此天然归所属线程所有——`ROADMAP` 里「定时器下沉至各子线程」指的就是这件事。它让定时器与连接之间的并发访问**在结构上不再存在**，而不是靠加锁 |
| `steady_clock` | 超时判定必须用单调时钟。墙上时钟被调整（或 NTP 校正）不该让连接被误回收 |
| `unordered_map` + 到期时扫描，不维护排序索引 | 条数是本线程的连接数，且只在该线程**确实有定时器到期**时才扫描一次——`rearm` 已经把 timerfd 精确装载到最早到期时刻，空转不产生唤醒。换来的是不必同时维护「有序容器 + 反向索引」两份结构 |
| `rearm` 先比较装载时刻 | 这条判断不是可有可无的优化：空闲超时在每次读写后都会 `refresh`，而 `refresh` 出来的到期时刻总是最晚的，改不到最早时刻，因此绝大多数 `refresh` 都不该产生 `timerfd_settime` |
| 到期项先摘出再执行回调 | 回调里可能取消或顺延同一个定时器（连接因空闲超时而关闭时会取消自己），不先摘就会二次执行 |

`kNever` 是一个哨兵时刻（`time_point::max()`），表示「没有定时器」。`rearm` 时若最早时刻为 `kNever`，`it_value` 保持全零即停止计时——不需要为「空表」单独写一条分支。

### SignalWatcher：signalfd

用法上有一步时序要求必须写明：**先在所有线程内屏蔽信号，再让它们只经 signalfd 送达**。`block_signals` 用的是 `pthread_sigmask`，它只作用于调用线程，因此必须在创建任何线程之前调用——子线程继承创建时的掩码，晚调用的部分线程不会被屏蔽，信号可能被它们按默认行为处理掉（`SIGTERM` 会让进程直接退出）。

这条约束写在头文件注释里，3.4 创建子 Reactor 线程时要照着它排序。

## 四、实现要点

| 文件 | 内容 |
|:--|:--|
| `net/timer_queue.h` / `.cpp` | `TimerQueue`：`add_timer` / `refresh_timer` / `cancel_timer`、`handle_read`、`rearm` |
| `net/signal_watcher.h` / `.cpp` | `SignalWatcher`：`signalfd` + `Channel`，静态的 `block_signals` |
| `tests/test_timer_queue.cpp` | 6 个用例（含 `SignalWatcher`） |
| `CMakeLists.txt` / `tests/CMakeLists.txt` | 加入两个新源文件与 `timer_queue_tests` 目标 |

## 五、验证

### 单元测试（6 个用例）

| 用例 | 覆盖点 |
|:--|:--|
| `FiresAfterDelay` | 到期触发 |
| `RefreshPostponesFiring` | 顺延生效（实测触发时刻落在顺延后的窗口内） |
| `CancelledTimerDoesNotFire` | 取消后不再触发 |
| `CallbackCanCancelItself` | 到期项已先摘除，回调里取消自己无副作用、不二次执行 |
| `RearmKeepsEarlierTimerOnRefresh` | 把较早的定时器顺延到较晚之后，触发顺序随之改变——验证 `rearm` 确实按最早时刻重装 |
| `DeliversBlockedSignal` | 屏蔽后 `raise` 的信号经 signalfd 送达回调 |

### 动态检查

| 项 | 结果 |
|:--|:--|
| `ctest` | 7/7 通过（新增 `timer_queue_tests`） |
| TSan（`setarch -R`） | 6/6 通过，无数据竞争报告 |
| ASan | 6/6 通过，无内存错误 |
| Release / TSan / ASan 三套构建 | 0 错误 0 警告 |
| `clang-format --dry-run --Werror` | 无差异 |
| 服务端行为 | 未改变（这一批未接入主流程） |

## 六、遗留

1. **定时器与连接的协作方式尚未确定**：这一批只提供了「定时器」这一能力，谁在什么时候添加、顺延、取消它，要到 3.3 引入 `TcpConnection` 时才安排。`020` 暴露的那些重复回收问题，要靠那时「释放收敛为单一出口」来解决
2. **`refresh` 的调用频率没有约束**：连接每读写一次就顺延一次。当前实现下这不产生系统调用（见 `rearm` 的比较），但接口本身没有防抖，接入时若发现调用过密，需要考虑限流
3. **`SignalWatcher` 的生命周期**：与 `TimerQueue` 一样，它要求使用方在销毁前保证 `EventLoop` 仍然有效。这个约束在 3.3 接入主流程时会集中显现（`Channel` 的析构不摘除事件表项，见 `022` 的遗留 1）
