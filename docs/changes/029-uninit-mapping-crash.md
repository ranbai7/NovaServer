# 029 · 短连接崩溃的根因与修复，附 3.4-b/c 的验证

> 本记录承接 `027`。`027` 把崩溃范围收窄到 `do_request` 一带但未能定位根因；本记录给出根因、修复，并补完 3.4-b（TSan 三场景）与 3.4-c（优雅退出）的验证。TSan 在此过程中又暴露出两处独立的数据竞争，一并记录。

## 一、根因

`http_conn` 的构造函数只初始化了两个事件通知器，**其余成员全部未初始化**：

```cpp
http_conn()
{
    m_want_read = [] {};
    m_want_write = [] {};
}   // m_url、m_file_address、m_file_stat 等均未初始化
```

而 `reset()` 的**第一步**就是 `unmap()`，它要读 `m_file_address` 判断有没有映射待释放：

```cpp
void http_conn::reset()
{
    unmap();                   // http_conn.cpp:524 —— 读 m_file_address
    ...
    m_file_address = nullptr;  // 到这里才置空，迟了一步
}

void http_conn::unmap()
{
    if (m_file_address)        // http_conn.cpp:1193 —— 读的是未初始化值
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
```

连接对象**复用堆内存**。`make_shared<TcpConnection>` 拿到的内存块会**残留上一个连接的取值**，那个地址对应的映射早已被释放。于是：

1. 连接 A 处理文件请求，`mmap` 得到地址 P，写入 `m_file_address`
2. 连接 A 关闭，`unmap()` 释放 P，对象析构，内存归还堆
3. 连接 B 复用同一块内存，`m_file_address` **残留着 P**（`malloc` 不清零）
4. 连接 B 构造时 `init()` → `reset()` → `unmap()` 读到 P → `munmap(P, 残留的 st_size)`

**这一步 `munmap` 解除的不是自己的映射**——P 那块地址可能已被重新用于堆扩展、malloc arena 或其他 mmap。堆就这样被无声地破坏，直到很久以后某次 `malloc` 走到 `_int_malloc` 才发现链表断了。这解释了 `027` 观察到的全部现象：**破坏点与崩溃点分离**、崩溃轮 QPS 先退化（破坏在累积）、只在高频短连接下出现（对象复用频繁）。

### 为什么文件请求是必要条件

只有走 `do_request` 的文件请求才会 `mmap`，也**只有它会让 `m_file_address` 变为非零**。这正好解释了 `027` 的二分为什么能收窄到那条链上——跳过 `do_request` 连接就从不 `mmap`，残留值保持为 `nullptr`，`unmap()` 直接跳过。

## 二、定位过程

### 工具选择

按「零成本 → 需授权」的顺序试了两步：

| 手段 | 结果 |
|:--|:--|
| `LD_PRELOAD=libc_malloc_debug.so.0 MALLOC_CHECK_=3` | 复现得 5/5 崩溃，但 stderr **零输出** |
| **valgrind memcheck** | **一击命中** |

第一步的失败本身给出了有用的结论。用已知 double-free 的最小程序做对照，确认该配置**确实生效**（输出从 `double free detected` 变为 `invalid pointer`），因此零输出不是"没启用"，而是**这类破坏不在它的检查范围内**：`MALLOC_CHECK_` 只在 `free` 时校验 chunk 头，而我们的破坏走的是 `munmap`，且在 **malloc** 时才暴露。

（另需说明：此前一度裸设 `MALLOC_CHECK_=3`——在 glibc ≥2.34 下该检查已移入独立的 `libc_malloc_debug.so.0`，不 `LD_PRELOAD` 它则为空操作。这是当时的操作失误。）

### valgrind 的结论

```
Conditional jump or move depends on uninitialised value(s)
   at unmap (http_conn.cpp:1193)
   by http_conn::reset() (http_conn.cpp:524)
   by TcpConnection::TcpConnection (tcp_connection.cpp:14)
   by make_shared<TcpConnection>
   by on_new_connection lambda (webserver.cpp:208)
Uninitialised value was created by a heap allocation
```

一行就指出了根因，与第一节的推理完全吻合。**这也说明此前的困境不是分析不够，而是工具不对**：`munmap` 的是合法地址、合法长度，只是"不属于自己"——ASan 不检查这种语义错误；破坏要在满足「对象复用 + 残留非零 + 地址仍是有效映射」三个条件时才发生，所以是概率性的。翻查 45 轮不如换一个对的工具。

## 三、修复

在构造函数里初始化相关成员，让对象一构造就处于有效状态：

```cpp
m_url = nullptr;
m_version = nullptr;
m_host = nullptr;
m_string = nullptr;
m_file_address = nullptr;
m_file_stat = {};
```

## 四、TSan 暴露的两处数据竞争

3.4-b 在 `-t 4` 且**开启日志**下跑连接风暴时，TSan 报出两处此前未发现的竞争。

### 1. `localtime` 不是线程安全的

```
Write of size 8 by thread T4:
  #1 tzset_internal (time/tzset.c:401)
  #2 Log::write_log (log.cpp:100)
  ← http_conn::process_read ← TcpConnection::handle_read ← 子线程循环
```

`localtime` 返回指向**进程内静态缓冲区**的指针，多个子 Reactor 线程同时写日志就会同时读写它；首次调用还会触发 `tzset` 改写全局时区状态。改为可重入的 `localtime_r`，并在单线程的 `Log::init()` 里先 `tzset()` 预热一次，此后并发取时间只剩读操作。

### 2. `m_fp` 的判空在锁外

```
Write of size 8 by thread T3 (mutexes: write M29):
  #0 Log::write_log (log.cpp:152)      ← m_fp = fopen(...)，持锁
Previous read of size 8 by thread T2:
  #0 Log::write_log (log.cpp:99)       ← if (m_fp == nullptr)，无锁
```

日志轮转（`fclose` 之后 `fopen` 之前）会让 `m_fp` 一度成为悬垂指针，而另一线程在锁外读它判空。把判空移入锁内。

### 3. `m_root` 的释放早于子线程退出

```
Write of size 8 by main thread:
  #0 free
  #1 WebServer::~WebServer() (webserver.cpp:33)     ← free(m_root)
Previous read by thread T2
```

`free(m_root)` 写在析构函数体里，而 `m_thread_pool` 作为成员要在**函数体之后**才析构（由它 join 子线程）。子线程可能仍在用 `m_root` 拼路径。改为在函数体开头先 `m_thread_pool.reset()`，等子线程全部退出后再释放。

## 五、验证

### 修复判据

| 项 | 修复前 | 修复后 |
|:--|:--|:--|
| 短连接 `-t 1` | 4/5 崩溃 | **0/10** |
| 短连接 `-t 0/2/4` | 0/5、5/5、2/5 | **0/15** |
| `-t 0` 加大压力 | 1/8 | **0/6** |
| valgrind（新连接构造） | 11 errors（含 uninitialised） | **0 errors** |
| TSan（连接风暴 + 日志） | 4 处竞争 | **0** |
| `SIGTERM` 退出码 | — | **0**（自行退出，非被信号杀死） |
| valgrind 退出泄漏 | — | `definitely/indirectly lost` 均 **0** |
| `ctest` | 7/7 | **7/7** |

（退出时的 `possibly lost: 248 bytes in 2 blocks` 经栈确认来自 `libmysqlclient` 的 `mysql_server_init` / `pthread_once`，与项目代码无关。）

### 性能未退化，反而提升

| `-t` | 修复前 | 修复后 |
|--:|--:|--:|
| 0 | 25315 | 29625 |
| 2 | 45918 | **50530** |
| 4 | 32739 | 38033 |

提升本身是旁证：此前破坏在持续累积、拖慢 `malloc`（崩溃轮 QPS 从 6500 掉到 3672），现在这条损耗消失了。

### 3.4-b / 3.4-c

| 场景 | 手段 | 结果 |
|:--|:--|:--|
| 连接建立/关闭风暴 | TSan，`-t 4`，`-c100` 短连接 | 无报告 |
| 空闲超时与主动关闭同时发生 | TSan，一半连接等超时回收、一半主动关闭 | 无报告 |
| 日志跨线程写入 | TSan，`-t 4` 且 `-c 0` 开启日志 | 无报告 |
| 单元测试 | TSan 下逐个运行 | 56 个用例全部通过 |
| 优雅退出 | `SIGTERM` | 退出码 0；valgrind 无泄漏 |

## 六、复现命令

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
cp build/server ./server
./server -p 9006 -m 0 -t 1 -c 1 &
taskset -c 0 wrk -t1 -c100 -d4s -H "Connection: close" http://127.0.0.1:9006/judge.html
```

valgrind 定位（`-t 0` 单线程配合低并发，避开其 10~30x 减速带来的调度失真）：

```bash
valgrind --tool=memcheck --track-origins=yes --log-file=/tmp/vg.log \
  ./build-rwd/server -p 9101 -m 0 -t 0 -c 1 &
wrk -t1 -c10 -d60s http://127.0.0.1:9101/judge.html
```

TSan（Ubuntu 22.04+ 需 `setarch -R` 绕过 ASLR 冲突）：

```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TSAN=ON
cmake --build build-tsan -j4
TSAN_OPTIONS=halt_on_error=0 setarch -R ./build-tsan/server -p 9200 -m 0 -t 4 -c 0 &
wrk -t1 -c20 -d30s -H "Connection: close" http://127.0.0.1:9200/judge.html
```

## 七、遗留

无。阶段三至此收口。
