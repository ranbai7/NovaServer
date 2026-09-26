# 005 · 单元测试框架接入

## 一、背景与动机

项目此前没有任何测试。阶段一完成了格式统一与告警清零，代码质量有了基础，但改动仍缺少回归保护——每次修改都只能手工启动服务、逐页访问来确认。

本次接入单元测试框架，目标有三：

1. 为可独立验证的模块建立可重复执行的检查
2. 为后续优化（尤其是并发模型重构）提供回归保障
3. 让持续集成具备实际可检查的内容

## 二、现状分析

项目结构对测试有两方面障碍：

| 障碍 | 表现 |
|:--|:--|
| **模块耦合** | `http_conn` 同时承担协议解析、连接管理、数据库访问，且解析方法均为 `private`，难以独立测试 |
| **缺少测试设施** | 没有测试目录、框架依赖与构建集成 |

因此本次选择**从可独立编译的基础设施模块入手**——`block_queue` 与 `lock` 下的同步原语。它们除 pthread 外无外部依赖，可单独编译链接。

## 三、设计方案

### 框架选择：GoogleTest

- C++ 生态的标准选择，断言宏与用例组织方式成熟
- Ubuntu 提供预编译包，内含静态库与 CMake 配置文件，接入成本低

### 依赖获取：系统包而非 FetchContent

使用 `find_package(GTest)` 查找系统中已安装的 GoogleTest，而不是用 CMake 的 `FetchContent` 从源码构建。原因有两点：

1. 当前环境无法访问 GitHub，`FetchContent` 不可用
2. 系统包方式对使用者更友好——一条 `apt install` 即可，无需额外的构建时间

### 测试目标独立

`unit_tests` 只链接被测模块自身所需的源文件，**不链接整个服务器**，避免把 socket、数据库等无关依赖引入测试，也让链接速度更快。

### 纳入 CTest

通过 `add_test` 注册测试，可直接用 `ctest` 运行。这样测试既能在本地一键执行，也为后续接入 CI 铺好了路。

## 四、实现要点

| 文件 | 改动 |
|:--|:--|
| `CMakeLists.txt` | 包含 `CTest`；在 `BUILD_TESTING` 开启时查找 GoogleTest 并加入 `tests` 子目录 |
| `tests/CMakeLists.txt` | 定义 `unit_tests` 目标，链接 `GTest::gtest_main` |
| `tests/test_block_queue.cpp` | 队列的基本操作、边界条件、超时行为与并发读写 |
| `tests/test_locker.cpp` | 互斥锁的互斥语义、信号量的等待与唤醒 |

运行方式：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## 五、验证

首轮运行结果为 13 个用例中 **12 通过、1 失败**。失败用例定位到一个真实缺陷。

### 测试发现的缺陷：`block_queue::front()` 越界读

```cpp
value = m_array[m_front];      // m_front 初始值为 -1
```

该队列的下标约定是：

- `m_back` 指向**最后入队**的元素
- `m_front` 指向**最近一次出队**的元素
- 因此队首元素实际位于 `(m_front + 1) % m_max_size`

当队列从未执行过 `pop` 时，`m_front` 仍为初始值 `-1`，此时读取 `m_array[-1]` 属于**越界访问**，返回的是相邻内存的内容（测试中表现为 0，而非预期的队首元素 10）。

**影响评估**：检索确认 `front()` 在项目代码中从未被调用（`threadpool` 与连接池中的 `.front()` 都是 `std::list::front()`），因此未造成实际影响，但属于确定缺陷。

**修复**：改为 `m_array[(m_front + 1) % m_max_size]`，并补充注释说明下标约定。

### 修复后的结果

```
[==========] 13 tests from 3 test suites ran.
[  PASSED  ] 13 tests.
100% tests passed, 0 tests failed out of 1
```

用例清单：

| 测试套件 | 用例数 | 覆盖内容 |
|:--|--:|:--|
| `BlockQueue` | 9 | 空队列状态、入队出队顺序、首尾元素、满队列拒绝、环形回绕、超时行为、并发生产消费 |
| `Locker` | 2 | 多线程计数保持正确（互斥）、底层互斥量可用 |
| `Sem` | 2 | 未 post 时阻塞、按初始计数放行 |

其中并发用例通过「生产者推送 1..N、消费者累加求和、比对总和」的方式验证了环形队列在多线程下的正确性。

**这正是接入测试的直接价值**：一个静态分析无法发现、且此前从未被触发的缺陷，被用例暴露了出来。

## 六、遗留问题

1. **覆盖范围有限**：目前仅覆盖基础设施模块。`http_conn` 的协议解析逻辑因私有访问与模块耦合尚未纳入，需要在后续重构中把解析器从连接管理里拆分出来，才能独立测试
2. **依赖需系统安装**：接入使用标准的 `find_package(GTest)`，运行前需 `sudo apt install libgtest-dev`
3. **CI 尚未接入**：测试已可通过 `ctest` 一键运行，下一步是接入 GitHub Actions，使每次推送自动执行
