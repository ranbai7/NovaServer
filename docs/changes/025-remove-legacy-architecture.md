# 025 · 删除旧架构残留

> 本记录是**阶段三「并发模型重构」的第三批之后半（3.3b）**，承接 `024`。`024` 把主循环切换到单 loop Reactor，但把旧文件留在了磁盘上；这一批删掉它们。

## 一、背景与动机

`024` 之后，旧架构的代码已经不参与构建，但文件仍在仓库里。保留它们有两个问题：一是读者难以判断哪些代码还活着，二是「不参与构建」本身是个临时状态，不应当长期存在。

之所以在 `024` 里没有一并删除，是因为那时无法确认删除后是否还有隐藏的依赖——只有编译一次才知道。`024` 提交、验证通过之后，删除的风险就只剩「编译不过」，而那是立即可见的。

## 二、现状分析

`024` 提交后仍留在仓库里的旧实现：

| 文件 | 内容 | 被谁引用 |
|:--|:--|:--|
| `threadpool/threadpool.h` | 半同步/半反应堆线程池（含 `append` / `append_p` / 忙等配套） | 无 |
| `threadpool/README.md` | 该目录的说明 | — |
| `timer/lst_timer.h` / `.cpp` | 升序双向链表定时器、`client_data`、`Utils`、`SIGALRM` + socketpair 接线 | `http/http_conn.h` 的一行 `#include` |
| `timer/README.md` | 该目录的说明 | — |

引用检查显示**只有一处活引用**：`http/http_conn.h` 里的 `#include "../timer/lst_timer.h"`，是改造前为 `Utils` 或 `client_data` 而包含的，改造后已无用途。

## 三、设计方案

删除两个目录，并去掉那行 `#include`。删除方式是 `git rm -r`（保留删除记录，便于日后回溯）。

**不删除 `lock/locker.h`**：它的 `locker` 与 `sem` 仍被 `log/block_queue.h` 和 `CGImysql/sql_connection_pool.h` 使用。`timer/lst_timer.cpp` 里的 `Utils` 类虽然也用它，但那部分随目录一并消失，与这个头文件无关。

## 四、实现要点

| 动作 | 内容 |
|:--|:--|
| 删除 | `threadpool/`（2 个文件）、`timer/`（3 个文件） |
| 修改 | `http/http_conn.h` 去掉对 `lst_timer.h` 的包含 |

## 五、验证

| 项 | 结果 |
|:--|:--|
| 编译（删 include 之后） | 0 错误 0 警告，`Built target server` |
| 孤立头文件检查 | 全部头文件仍被引用（逐个头文件反查引用者） |
| `ctest` | 7/7 通过 |
| 全链路：`/judge.html`、`/`、404、`/upload`、HEAD、上传、下载 | 全部符合预期 |
| 半关闭用例 | 通过 |
| 服务端 stderr | 无输出 |

## 六、遗留

1. **`README.md` 与 `docs/ROADMAP.md` 仍描述旧架构**：前者说「线程池 + Epoll + Reactor/Proactor」，后者的「现有架构」章节列着 `threadpool/threadpool.h` 与 `timer/lst_timer.cpp`。这两处要等 `-a` 参数移除（3.5）与 `architecture.md` 新建（3.6）时一并更新，否则改了还要再改一遍
2. **`m_thread_num` 配置项与 `-t` 参数当前无效果**：线程池尚未接入（3.4）
