# 024 · 接入单 loop Reactor

> 本记录是**阶段三「并发模型重构」的第三批（3.3a）**，承接 `022` 与 `023`。前两批只新增模块，这一批**真正替换了服务端的主循环**——`webserver.cpp` 的旧主循环、旧连接管理与旧定时器接线全部退场，服务端改由 `EventLoop` + `Acceptor` + `TcpConnection` 驱动。

## 一、背景与动机

`022` 落地了 `EventLoop` 与 `Channel`，`023` 落地了 `TimerQueue` 与 `SignalWatcher`，但它们都没有接入主流程。这一批完成接线，架构上从此是 one loop per thread——只是当前 loop 数为 1。

拆分上原本希望「先做骨架、再改协议层」，执行时发现这一步走不通：协议层自己持有 `static m_epollfd` 并直接调 `epoll_ctl`（7 处），而新架构的事件由 `Channel` 管理。两者对同一个描述符重复注册（旧的还带 `EPOLLONESHOT`）无法共存，因此**协议层的事件注册改造必须与骨架同批做**。好在改造本身很小：注入两个回调、替换 5 处调用点，1662 行协议代码的其余部分没有改动。

于是批次重新划分为：本批（3.3a）接入新架构并保留旧文件；下批（3.3b）删除旧架构残留。

## 二、现状分析

旧主循环与协议层的耦合点（`022` 已列出四处，这里是它们的具体形态）：

| 耦合点 | 旧形态 | 新形态 |
|:--|:--|:--|
| 事件注册 | `http_conn::m_epollfd` 静态单例，7 处 `modfd` | 注入 `want_read` / `want_write` 两个回调，协议层不再认识 epoll |
| 连接计数 | `http_conn::m_user_count` 静态成员，在三处增减且跨线程 | `WebServer` 的 `std::atomic<long>` |
| 关闭判断 | `close_conn()` 自己 `EPOLL_CTL_DEL` + `close`，且有负偏移等问题 | `process()` 返回 `bool`，释放交给 `TcpConnection::close()` 这一唯一出口 |
| 跨线程信令 | `improv` / `timer_flag` / `m_state` + 主线程 `while(true)` 自旋 | 全部删除 |

## 三、设计方案

### 协议层只表达「关心什么」，不执行注册

```cpp
    void set_event_notifier(EventCallback want_read, EventCallback want_write);
```

`modfd(EPOLLIN)` → `m_want_read()`、`modfd(EPOLLOUT)` → `m_want_write()`。这样协议层完全不认识 epoll 与 `Channel`——「请求还没收全就继续读」「响应就绪就转去写」本来就是解析逻辑的一部分，留在协议层最自然。

缺省值为空操作（构造函数里设置），未注入时退化为「什么也不做」而不是抛 `bad_function_call`。

### 连接的生命周期全部归属 `TcpConnection`

- 构造、读写、析构都在所属事件循环的线程内，协议状态因此不需要任何跨线程保护
- `start()` 里注册描述符、装载空闲定时器，并给 `Channel` 绑上 `tie(weak_from_this())`
- **释放收敛为唯一出口** `close()`：读失败、处理失败、写失败、对端关闭、空闲超时五个触发点全部调用它，`m_closed` 保证幂等
- 实际释放拆到 `release_fd()`，因为析构里不能调 `shared_from_this()`：析构只调 `release_fd()`
- 释放顺序固定：取消定时器 → `m_conn.unmap()` → `disable_all` → `Channel::remove()`（`EPOLL_CTL_DEL`）→ `::close`。**`EPOLL_CTL_DEL` 必须先于 `close`**，这是唯一能保证该顺序的地方

### 半关闭的判断沿用 `020` 的结论

`Channel::handle_event` 的顺序是读 → 写 → 关闭，且关闭回调里先查 `has_pending_work()`：

```cpp
    if (m_conn.has_pending_work())
        return; //由读或写路径收尾
```

这样「请求刚读完、响应还没发完」的连接不会被半关闭事件打断。`020` 里那套用例在本批全部重跑过。

### 顺带修掉的两处

- **空闲超时改为 timerfd 精确到期**：旧实现靠 `SIGALRM` 每 5 秒全表扫描，实测回收时刻是 18.6 秒；现在实测 **15.0 秒**（配置值就是 15 秒）
- **`trig_mode` 的未初始化缺陷**：旧代码没有 `else` 分支且两个成员未初始化，非法取值会以不确定的方式注册描述符。新实现改成 `switch` + `default` 终止启动（原计划这是 3.5 的内容，因为要写新的初始化逻辑，顺手做了）

### 未做的事：描述符耗尽兜底已就位但未验证

`Acceptor::handle_fd_exhausted()` 已经写好（EMFILE 时释放预留的 `/dev/null` 描述符、accept 一个后立刻关闭、再占回来），但**本批没有构造出描述符耗尽的场景**去验证它。验证安排在 3.5。

## 四、实现要点

| 文件 | 改动 |
|:--|:--|
| `net/acceptor.{h,cpp}` | 新增：监听套接字、循环 accept、EMFILE 兜底 |
| `net/tcp_connection.{h,cpp}` | 新增：连接生命周期、读写、空闲定时器、唯一释放出口 |
| `net/socket_utils.{h,cpp}` | 新增：`set_nonblocking` |
| `net/event_loop.{h,cpp}` | 连接注册表与延迟擦除；持有 `TimerQueue`；注册表元素类型为 `shared_ptr<void>`（避免事件循环依赖连接类型，否则每个用到 `EventLoop` 的测试都会被拖去链接整个协议层） |
| `http/http_conn.{h,cpp}` | 删 `static m_epollfd` / `m_user_count` / `close_conn` / `rearm_epoll` / `timer_flag` / `improv` / `m_state`；`process()` 返回 `bool`；私有 `init()` 改名 `reset()`；`unmap()` 提为公开并在 `reset()` 开头调用（复用前释放上一次的映射）；`initmysql_result` 改为静态并接收日志开关 |
| `webserver.{h,cpp}` | 重写：主循环改为 `EventLoop`，连接由 `Acceptor` 接受并交给 `TcpConnection`，计数改原子量 |
| `main.cpp` | 启动序列简化为 `init → log_write → sql_pool → run` |
| `CMakeLists.txt` | 加入 `net/` 各文件，**移除 `timer/lst_timer.cpp`**（旧文件仍在磁盘上，下批删除） |

## 五、验证

### 全链路

| 用例 | 结果 |
|:--|:--|
| `/judge.html`、`/`、`/register.html`、`/nosuchfile`、`/upload` | 200 / 200 / 200 / 404 / 200 |
| `HEAD /judge.html` | 200 且无正文 |
| 上传 `.png` → 下载 | 200 / 200 |
| 上传非白名单 `.bin` | 415 |
| URL 转义 `%6a` | 200 |

### `020` 的定向用例（四种触发模式各跑一遍）

| 用例 | 结果 |
|:--|:--|
| 同一连接串行 10 轮 × 6 个请求 | 4/4 模式通过 |
| 8 条连接 × 各 20 个请求 | 4/4 模式通过 |
| 半关闭（小页面 + 5 MiB） | 4/4 模式通过，正文 md5 一致 |

### 超时与性能

| 指标 | 旧架构（`021` 基线） | 本批 | 说明 |
|:--|:--|:--|:--|
| 空闲回收时刻 | 18.6 s（配置 15 s） | **15.0 s** | timerfd 精确到期 |
| QPS（`-c100 -d10s`） | 25858 | **28108** | 单线程已追平旧的「主线程 + 8 工作线程」 |
| P50 / P99 | 3.69 / 7.19 ms | 3.41 / 7.00 ms | |

QPS 的 8.7% 提升落在 `021` 记录的 ±10% 噪声内，因此**不能断言性能改善**；可以断言的是「没有退化，且单线程已追平多线程」，这与「消除主线程忙等」的预期一致。

### 动态检查

| 项 | 结果 |
|:--|:--|
| `ctest` | 7/7 通过 |
| ASan 全链路（含半关闭大文件） | 无任何报告 |
| Release / ASan 双构建 | 0 错误 0 警告 |
| `clang-format --dry-run --Werror` | 无差异 |

### 一次排查上的弯路（值得记下）

验证初期出现「请求被完整解析、响应也组装好了，却发不出去」。strace 显示连接描述符注册之后**没有出现 `MOD` 到 `EPOLLOUT`**，据此判断协议层的事件通知器没生效。逐个排查后才发现真正的原因：此前做告警检查时执行过 `rm -rf build/CMakeFiles/server.dir`，删掉了 `server` 目标的构建规则却没有重新配置 CMake——**从那一刻起 `server` 就再没被编译过**，改的代码根本没进二进制。

教训是：清理构建目录的一部分之后必须重新配置，而「编译通过」的判断要看 `Built target server` 这类目标级输出，不能只看有无 error。

## 六、遗留

1. **旧架构的文件仍在磁盘上**：`threadpool/threadpool.h`、`timer/lst_timer.{h,cpp}`、`lock/locker.h` 中的 `Utils` 相关部分。它们已不参与构建，下批（3.3b）删除。当前 `lock/locker.h` 仍被 `log/block_queue.h` 使用，不能整体删除
2. **描述符耗尽兜底未验证**：见第三节末
3. **`m_thread_num` 与 `-a` 参数当前无效果**：线程池尚未接入（3.4），而 `-a`（Proactor/Reactor）在新架构下不再有意义（3.5 移除）
4. **单线程是当前的稳态**：`ROADMAP` 的目标是「主线程只 accept、连接分发给若干子 Reactor 线程」，本批只是把 loop 数定在了 1。3.4 接入 `EventLoopThreadPool` 后才算落实
5. **`inet_ntoa` 在日志里使用**：它返回静态缓冲区，当前单线程下安全，但 3.4 引入多线程后需要替换
