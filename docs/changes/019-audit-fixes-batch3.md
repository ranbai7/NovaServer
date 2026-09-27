# 019 · 核查问题修复（批次三：清理）

> 本记录对应 `016` 第六节修复计划的**批次三**，处理 4.7 表中的 7 项。批次一见 `017`，批次二见 `018`。

## 一、背景与动机

`016` 的 4.7 把七项攒在一张表里，它们没有共同的技术主题，共同点只是**不影响当前可观测行为**：死成员、注释掉的代码块、只在特定路径下才成立的指针运算、可以更早给出的拒绝、多付一次的内存拷贝、未被检查的返回值、一个拼错的日志词。

但其中三项有实际分量：

| 项 | 为什么值得单独说 |
|:--|:--|
| 负偏移指针运算 | 赋值本身就构成未定义行为，只是恰好没被解引用 |
| 扩展名校验偏晚 | 不合格的上传要收完整个请求体（上限 8 MiB）才拒绝 |
| 内存放大 | 一次上传的峰值比必要值高出一倍 |

## 二、现状分析

### 3 项 · 负偏移指针运算

`process_write` 的 HEAD 分支把响应头结束位置记为截断点：

```cpp
    if (m_method == HEAD && m_header_end > 0)
    {
        m_iv[0].iov_len = m_header_end;   // 由 m_write_idx 缩短
        bytes_to_send = m_header_end;
    }
```

但这条路径上 `m_iv_count` 是 1，`m_iv[1]` 从未被设置。而 `write()` 在推进 iovec 时无条件计算第二个：

```cpp
        if (static_cast<size_t>(bytes_have_send) >= m_iv[0].iov_len)
        {
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
```

`m_header_end < m_write_idx`（正文也在写缓冲里），于是差为负；`m_file_address` 在动态正文路径上为空。**这个赋值本身**就是越界的指针运算，只是结果没被解引用——因此常规测试与 ASan 都看不见它，只有 UBSan 会报。

### 4 项 · 扩展名校验偏晚

上传的校验顺序是：收完整具请求体 → 解析 multipart → 取文件名 → 判扩展名。而 `do_request` 只在请求体**已完整读入**后才会被调用，所以一个 4 MiB 的 `.bin` 在限速 1 MB/s 的链路上要传满 4.06 秒才被告知 415。

### 5 项 · 内存放大

```cpp
    std::string body(m_string, m_content_length);      // 拷贝①：整个请求体，≤8 MiB
    ...
    m_file_content = body.substr(content_start, ...);  // 拷贝②+③：文件内容，≤4 MiB
```

`substr` 返回临时 `std::string`，再赋值给 `m_file_content` 又是一次拷贝。实测上传 4 MiB 时进程峰值内存增量 16.3 MB，与「请求体 4 + 临时 4 + 文件内容 4 + 其它」吻合。

## 三、设计方案

### 3 项：把「已发完」的判断提到推进之前

```cpp
        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_to_send <= 0)
        {
            ... 收尾与返回 ...
        }

        //尚未发完，推进还没写完的那一段
        if (2 == m_iv_count && bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            ...
        }
```

调整后，下面的指针运算全部建立在「确有剩余字节」之上：走 `else` 分支时 `bytes_have_send < m_iv[0].iov_len`，`iov_len` 的减法不会下溢；走 `if` 分支时已排除 `iov_count == 1`（HEAD 与纯写缓冲响应），差必为正。

### 4 项：请求体边收边判

multipart 的文件名就在 part 头部里，而 part 头部通常是请求体最前面的几百字节。因此不必等收完——把「定位 part 头部」抽成一个可作用于任意长度前缀的函数，在内容状态机里反复尝试即可：

- 结构尚未到齐 → `NO_REQUEST`，等下一次读取
- 头部到齐但文件名不合格 → 立即返回 400 / 415 并**置 `m_linger = false`**（请求体没读完，连接上必然残留字节，不能复用）

这一项需要配套改两处：

1. **`process_read` 的内容分支此前丢弃了所有非 `GET_REQUEST` 的返回值**，会把提前判定当成「还没收完」。补上 `if (ret != NO_REQUEST) return ret;`
2. `parse_content` 的返回值里 `GET_REQUEST` 另有含义（请求体已收完），因此提前判定中表示「准入」的 `GET_REQUEST` 必须转成 `NO_REQUEST` 再返回

### 5 项：改在缓冲区上取视图

`m_string` 指向读缓冲内部、`parse_content` 已保证内容完整，因此不需要拷贝一份 `std::string`：改用 `std::string_view` 做查找，只在最后落到 `m_file_content` 时拷一次。

这一项在计划中带「评估」性质，判断依据是**改造不会让解析复杂化**——查找与截取都可直接作用于视图，反而省掉了 `substr` 的临时对象。

## 四、实现要点

| 文件 | 改动 |
|:--|:--|
| `http/http_conn.h` | 删 `m_users`、`sql_user` / `sql_passwd` / `sql_name` 与 `init()` 的三个死形参；新增 `check_upload_name_early()` |
| `http/http_conn.cpp` | 新增文件内辅助 `check_upload_name()`（准入判定，两处共用）与 `locate_first_part()`（part 头部定位，容忍不完整前缀）；`parse_content` 调用提前判定、`process_read` 保留其返回值；`parse_multipart_content` 改用视图与 `locate_first_part`；`do_request` 改用 `check_upload_name`；`write()` 调整 iovec 推进顺序；`process()` 在关闭后不再对 -1 调 `epoll_ctl`；`process_write` 全部 `add_*` 调用补返回值检查；删除注释代码块；`unknow` → `unknown` |
| `webserver.cpp` | `timer()` 中的 `init` 调用随之删去三个实参 |

`process_write` 的返回值检查按同一模式统一：`if (!add_status_line(...) || !add_content_type(...) || !add_headers(...) || !add_content(...)) return false;`

## 五、验证

### 项 4：拒绝时机（限速 1 MB/s 上传 4 MiB 的 `.bin`）

| 版本 | 状态码 | 耗时 |
|:--|:--|:--|
| 旧（`ba3f62a`，`git worktree` 单独构建） | 415 | 4.06 s |
| 新 | 415 | **0.105 s** |

### 项 5：上传期间的内存峰值（`VmHWM`，上传同一个 4 MiB 文件）

| 版本 | 上传前 | 上传后 | 增量 |
|:--|--:|--:|--:|
| 旧 | 77764 kB | 94016 kB | +16.3 MB |
| 新 | 73780 kB | 81864 kB | **+8.1 MB** |

### 项 3：受影响路径

| 用例 | 结果 |
|:--|:--|
| 下载 5 MiB 文件（走 writev 部分写路径） | 200，5242880 字节，md5 与源文件一致 |
| `HEAD /big5.bin`（`iov_count == 1`，原触发点） | 200，`Content-Length: 5242880`，正文 0 字节（响应共 105 字节） |
| `HEAD /upload`（动态正文路径） | 200 |

### 回归与动态检查

| 项 | 结果 |
|:--|:--|
| `ctest`（Release） | 5/5 通过 |
| Release 与 ASan 双构建 | 0 错误 0 警告 |
| `clang-format --dry-run --Werror` | 无差异 |
| 全链路：`/judge.html`、`/`、`/nosuchfile`、`/upload`、上传 `.png`、下载、415、501、431 | 状态码全部符合预期 |
| 上传准入：0 字节文件、4 MiB `.png`、点号开头、无扩展名、非白名单 | 200 / 200（md5 一致）/ 400 / 415 / 415 |
| ASan 全链路 | 无任何报告 |

## 六、遗留问题

1. **不支持 `Transfer-Encoding: chunked` 的请求体**：服务端只按 `Content-Length` 界定请求体，而 `m_content_length` 为 0 时 multipart 解析直接判为错误请求。这是核查期间用 `curl -F "file=@/dev/null"` 做题外验证时发现的——curl 对字符设备无法预知长度，会自动改用 chunked 传输，于是得到 400。它不属于本批范围（`016` 未列），记录在此
2. **体积上限仍只能事后判定**：`MAX_UPLOAD_SIZE` 比的是文件内容长度，而 `Content-Length` 是整个请求体的长度（含边界与 part 头部），请求体未超限并不说明文件未超限。若要在接收途中提前拒绝，需要一个能容忍边界开销的保守上界
3. **`check_upload_name_early` 只在 multipart 的第一个 part 上判定**：若文件名出现在后续 part，提前判定不会触发（正常流程仍会在 `do_request` 中判定）。本服务端只处理单文件上传，实践中构不成问题
