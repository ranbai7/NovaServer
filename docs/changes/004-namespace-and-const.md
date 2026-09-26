# 004 · 命名空间与 const 正确性

## 一、背景与动机

头文件中的 `using namespace std;` 会把整个 `std` 命名空间引入所有包含者，属于典型的头文件污染。项目中存在 4 处这样的写法，其影响已经实际显现——`http_conn.h` 自身并未声明该 using，却能直接使用 `string` 与 `map`。

同时，部分只读方法缺少 `const` 限定，只读的字符串参数存在不必要的按值传递。

## 二、现状分析

### 命名空间的分布

| 位置 | 影响范围 |
|:--|:--|
| `config.h` | 污染所有包含者 |
| `log/log.h` | 污染所有包含者 |
| `log/block_queue.h` | 污染所有包含者 |
| `CGImysql/sql_connection_pool.h` | 污染所有包含者 |
| `log/log.cpp` | 仅限本文件 |
| `CGImysql/sql_connection_pool.cpp` | 仅限本文件 |

**污染的具体表现**：`http_conn.h` 通过包含 `sql_connection_pool.h` **间接**获得了 `std` 命名空间，因而可以在不写前缀的情况下使用 `string`、`map`。这类依赖是隐式的——一旦包含链调整，编译会在看似无关的地方失败。

### const 与参数传递

| 位置 | 问题 |
|:--|:--|
| `http_conn::get_address()` | 返回非 const 指针，调用方可意外修改内部地址 |
| `connection_pool::GetFreeConn()` | 只读方法未标记 `const` |
| 三处 `init` | 以值传递 `std::string`，产生不必要的拷贝 |

## 三、设计方案

### 命名空间：头文件与源文件区别对待

- **头文件**：移除 `using namespace std;`，所有 std 类型显式加 `std::` 前缀
- **源文件**：保留各自的 `using namespace std;`

判据是**影响范围**：头文件的 `using` 会外溢到所有包含者，是真正的问题；源文件的作用域限于本文件，属业界普遍接受的做法。这样既解决了问题，也避免为一致性而制造大量无意义的改动。

### const：明确适用边界

`const` 只能标记**不修改对象状态**的方法。审视后发现，本项目的绝大多数方法——协议解析、事件分发、日志写入、连接管理——都会改变对象状态，因此可标记的位置本就有限，强行标记反而失真。

真正普遍的改进在**参数传递**层面：只读的 `std::string` 参数应由按值改为 const 引用。

## 四、实现要点

### 命名空间

- 移除 4 个头文件的 `using namespace std;`，为其中的 std 类型补前缀
- 源文件中因失去间接声明而不再成立的地方同步补前缀（`main.cpp`、`webserver.cpp`、`http_conn.cpp` 共 11 处）
- 保留 `log.cpp` 与 `sql_connection_pool.cpp` 自身的 `using` 声明

### const 与参数传递

- `http_conn::get_address()` → `const sockaddr_in *get_address() const`
- `connection_pool::GetFreeConn()` → 补充 `const`
- `WebServer::init`、`http_conn::init`、`connection_pool::init` 的 `std::string` 参数改为 const 引用

### 顺带修复：端口号被隐式截断

`connection_pool::init` 中存在这样一行：

```cpp
m_Port = Port;    // m_Port 声明为 std::string，Port 是 int
```

这行能通过编译，是因为 `std::string::operator=(char)` 存在——**整型被静默截断为单个字符**。实测验证：

```
int 3306  ->  string 长度=1，首字符=-22
```

即端口号 3306 被存成了截断后的单字节。该成员在项目中**从未被读取**，所以没有造成实际影响，但它同时是「死成员」与「类型错误」。

**处理**：将 `m_Port` 的类型改为 `int`，与语义一致。

> 这类隐式转换若启用 `-Wconversion` 类告警可在编译期直接暴露，值得在后续的编译选项中评估。

## 五、验证

| 项 | 结果 |
|:--|:--|
| 编译 | 0 错误、0 警告 |
| 静态页面 | `/judge.html` `/log.html` `/register.html` `/picture.html` `/video.html` `/upload.html` `/fans.html` `/welcome.html` 全部 200 |
| 路由映射 | `/0` `/1` `/5` `/6` `/7` `/8` 全部 200 |
| 数据库读取路径 | 登录请求正常处理 |
| 文件上传 | 返回 200，且文件正确落盘 |
| 空文件响应 | 200 |

## 六、遗留问题

1. **`-Wconversion` 尚未启用**——本次发现的隐式截断问题可由该告警在编译期捕获
2. **源文件仍保留 `using namespace std;`**——属于可接受范围；若后续引入单元测试并需要更严格的一致性，可再统一
3. **剩余的手动内存管理**——`webserver.cpp` 中站点根路径、`http_conn.cpp` 中拼装 SQL 语句仍使用 `malloc`/`free`，可在阶段二的「参数化查询」改造中一并替换为 `std::string`
