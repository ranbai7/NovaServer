# 014 · HEAD 方法与请求头校验

## 一、背景与动机

本项处理路线图中的两条：

| 项 | 现状 | 方向 |
|:--|:--|:--|
| 请求方法 | 未支持 HEAD | 补充 HEAD 处理，复用 GET 的响应头构造 |
| 头部校验 | 缺少长度与合法性检查 | 增加头部长度上限与 `Content-Length` 合法性校验 |

`010` 的遗留问题之一是「不支持的方法未区分 405 与 501」，当时的结论是等 HEAD 支持落地后再处理（那时才能确定服务端实际支持的方法集合），因此一并在此完成。

## 二、现状分析

### 方法只识别 GET 与 POST

```cpp
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0) { m_method = POST; cgi = 1; }
    else
        return BAD_REQUEST;
```

两个后果：

- HEAD 被直接判为错误请求。HEAD 的语义是「响应头与 GET 完全相同、但不发送正文」，它是缓存校验与探活的常用方法
- 规范中已定义但本服务端未实现的方法（PUT / DELETE / TRACE / OPTIONS / CONNECT / PATCH）与「根本不是方法」的记号都得到 400，无法区分「语法错误」与「未实现」

### `Content-Length` 用 `atol` 解析

```cpp
        m_content_length = atol(text);
```

`atol` 对非法输入是静默的：`abc` 得到 0，`12abc` 得到 12，`-5` 得到 -5。长度与实际不符时，解析出的请求体边界也就是错的。

此外**重复的 `Content-Length` 不被检测**：两个取值不同的长度头同时出现时，「以哪个为准」成为实现相关的选择，这是请求走私的常见手法。

### 头部没有长度上限

只要整个请求不超过读缓冲的 8 MiB，单个头部行可以任意长——头部本应是很短的内容，允许它占满整个缓冲既无必要，也放大了内存占用。

### HTTP/1.1 要求的 Host 未校验

本服务端只接受 `HTTP/1.1`，而该版本要求请求必须携带 `Host`。缺失时当前不报错，继续按路径分发。

## 三、设计方案

### HEAD：复用 GET 的构造过程，最后截断正文

没有为 HEAD 单独写一套响应构造，而是**记录响应头结束的位置**，在组装完成后再决定是否附上正文：

```cpp
    //add_blank_line 中
    m_header_end = m_write_idx;
```

```cpp
    //process_write 末尾
    if (m_method == HEAD && m_header_end > 0)
    {
        m_iv[0].iov_len = m_header_end;
        bytes_to_send = m_header_end;
    }
```

这样 HEAD 与 GET 的响应头**由同一段代码生成**，不会出现两者随时间漂移。文件响应同理：只把承载文件内容的那段 `iovec` 去掉，`Content-Length` 仍是文件的真实长度（这正是 HEAD 的用途）。

### 区分「未实现」与「无法识别」

新增 `is_known_method`，列出规范定义的方法。请求方法命中该列表时返回 501 并给出 `Allow` 头，未命中则仍是 400（请求语法错误）。二者语义不同：前者表示服务端能力不足，后者表示客户端报文有误。

### `Content-Length` 严格解析

只接受非空十进制数字串（逐字符校验，不经过 `atol`）。三种非法情形分别处理：

| 情形 | 处理 |
|:--|:--|
| 取值非数字（含 `+`、`-`、空） | 400 |
| 重复出现 | 400 |
| 数值超过请求体上限 | 413（无需再传输请求体） |

数值超限时直接回 413 而不是继续接收，可以避免为一个注定被拒绝的请求传输数兆字节。

### 头部长度上限与 Host

头部在解析过程中逐行推进，因此以解析游标衡量其累计长度，超过 8 KiB 返回 431。`Host` 缺失返回 400。

## 四、实现要点

| 文件 | 改动 |
|:--|:--|
| `http/http_conn.h` | 新增 `MAX_HEADER_SIZE`、`REQUEST_HEADER_TOO_LARGE` 与 `METHOD_NOT_IMPLEMENTED` 状态、`m_header_end` 与 `m_has_content_length` |
| `http/http_conn.cpp` | 新增 `is_known_method` 与 `parse_content_length`；`parse_request_line` 识别 HEAD 并区分 501/400；`parse_headers` 增加长度上限、Host 必填、`Content-Length` 校验；`add_blank_line` 记录响应头结束位置；`process_write` 新增 431 与 501 分支、按 HEAD 截断正文；`process_read` 的请求行分支改为「非 NO_REQUEST 即为最终结果」 |

### 顺带修正：请求行结果被漏判

修正后的用例立刻暴露出一个缺陷：`process_read` 的请求行分支原本只判 `BAD_REQUEST`——

```cpp
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;   // 其它取值被当作「还没解析完」
        }
```

新增的 `METHOD_NOT_IMPLEMENTED` 因此被漏过：解析继续推进，把下一行（`Host: t`）当作请求行来解析，最终以 400 收场——PUT 请求返回 400 而不是 501。改为「非 `NO_REQUEST` 即返回」后语义与上一项（头部阶段）一致。

这类「只判一个特定错误值」的写法在新增错误类别时容易漏改，两处分支现已统一为「未完成即返回」。

## 五、验证

### HEAD 方法

对 4 个路径分别比较 GET 与 HEAD 的响应：

| 路径 | 结果 |
|:--|:--|
| `/judge.html` | 200，`Content-Length` 与 `Content-Type` 与 GET 一致，正文为空 |
| `/`（映射到 judge.html） | 200，同上 |
| `/5`（映射到 picture.html） | 200，同上 |
| `/test1.jpg` | 200，`Content-Length: 78443` 与 GET 一致，正文为空 |
| `/nope.html` | 404 且无正文（错误响应同样不携带正文） |

「正文为空」以「读到的字节恰好在响应头结束处为止」判定，而非只看 curl 的输出。

### 请求头校验

| 用例 | 期望 | 结果 |
|:--|:--|:--|
| 缺少 Host | 400 | 通过 |
| `Content-Length` 非数字 | 400 | 通过 |
| `Content-Length` 带符号（`+5`） | 400 | 通过 |
| 重复 `Content-Length` | 400 | 通过 |
| `Content-Length` 超过上限 | 413 | 通过 |
| 头部超过 8 KiB | 431 | 通过 |
| `PUT` | 501 | 通过（修正前为 400） |
| `DELETE` | 501 | 通过（修正前为 400） |
| 无法识别的方法（`FOO`） | 400 | 通过 |
| 501 响应包含 `Allow` | `GET, HEAD, POST` | 通过 |

### 回归与动态检查

| 项 | 结果 |
|:--|:--|
| 6 类用例 × 4 触发模式 × 2 并发模型（6 组合） | 全部通过 |
| `ctest`（Release 与 ASan 两种构建） | 5/5 通过 |
| 编译 | 0 错误 0 警告 |
| `clang-format --dry-run --Werror`（全项目 .cpp/.h） | 无差异 |
| AddressSanitizer 全链路（6 类用例） | 无任何报告 |

## 六、遗留问题

1. **HTTP/1.0 仍返回 400**：更贴合的语义是 505 `HTTP Version Not Supported`。本次只按「方法」这一维度完善了状态码，版本维度留待后续
2. **HEAD 不适用于 CGI 路径**：`HEAD /2CGISQL.cgi` 不带 `cgi` 标志，会被当作静态路径处理并返回 404。登录/注册本就应使用 POST，影响有限
3. **已实现方法的清单是硬编码的字符串比较**：新增方法时需同步更新两处（解析分支与 `is_known_method`）
4. **头部上限为硬编码常量**：8 KiB 写在头文件中，未纳入配置文件
5. **对端关闭写端时请求被丢弃**：与 `013` 遗留问题 7 为同一问题，已定位、留待阶段三的事件循环重写一并处理
6. **未处理 `Transfer-Encoding: chunked`**：分块传输的请求体不会被解析，`Transfer-Encoding` 头被当作未知头部忽略。对应当前只有表单与 multipart 两种上传方式，均为定长请求体
