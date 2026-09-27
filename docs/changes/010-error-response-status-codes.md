# 010 · 错误响应与状态码语义修正

## 一、背景与动机

`003-warning-cleanup.md` 的遗留问题中记下了两处错误响应的缺陷：`NO_RESOURCE` 缺少响应分支、`BAD_REQUEST` 返回的状态码与语义不符。在后续的排查中又发现两处同类问题。它们有一个共同点——**都不是「响应内容写错了」，而是响应根本发不出去、或发出去了但语义错误**：

| 缺陷 | 表现 |
|:--|:--|
| `NO_RESOURCE` 无分支 | 控制流落入 `default` 返回 false，连接被直接关闭，客户端收到连接重置 |
| `BAD_REQUEST` 返回 404 | 请求语法错误却回「文件未找到」 |
| 错误响应无 `Content-Type` | 客户端只能自行猜测正文类型 |
| 行结束符非法时不作判定 | 请求被搁置，一直等到连接超时 |

前两项在功能测试中完全看不出来：前者表现为「浏览器偶尔连接被重置」，后者表现为「状态码不对但不影响页面」。

## 二、现状分析

### `NO_RESOURCE` 落入 `default` 后连接被关闭

`process_write` 的 `switch` 中没有 `NO_RESOURCE` 分支，请求不存在的文件时控制流落到 `default: return false`，于是 `process()` 中：

```cpp
bool write_ret = process_write(read_ret);
if (!write_ret)
    close_conn();
```

连接被关闭，**一个字节的响应都没有发出**。实测：

```
$ curl -o /dev/null -w '%{http_code}' http://127.0.0.1:9102/nope.html
000
```

`000` 表示 curl 未收到任何 HTTP 响应。这与 `003` 中修复的「空文件响应发不出去」是同一类问题——响应组装流程中的分支缺失。

### 请求语法错误返回 404

```cpp
case BAD_REQUEST:
{
    add_status_line(404, error_404_title);
    add_headers(strlen(error_404_form));
```

而 `error_400_title` 与 `error_400_form` 早已定义却从未被引用。路径穿越、请求体字段缺失、目录请求、不支持的 HTTP 版本都走这条分支。

### 错误响应缺少 `Content-Type`

`add_headers` 只写 `Content-Length`、`Connection` 与空行，四类错误响应都没有 `Content-Type`；文件响应在 `FILE_REQUEST` 分支里单独补了一个。`add_content_type()` 这个成员函数已经写好，但**从未被调用过**，且硬编码为 `text/html`。

### 行结束符非法时不作判定

`process_read` 的循环条件依赖 `parse_line()` 的返回值：

```cpp
while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
       ((line_status = parse_line()) == LINE_OK))
```

`parse_line()` 遇到非法行结束符（单独的 `\n`，或 `\r` 之后不是 `\n`）返回 `LINE_BAD`，循环条件为假直接退出，函数末尾返回 `NO_REQUEST`。调用方据此认为「数据还没收完」，于是重新注册读事件继续等——但后续数据再到达也拼不出合法的行。实测以单独 `LF` 结尾的请求行**不产生任何响应**，连接一直挂到定时器超时回收。

## 三、设计方案

### 补齐 `NO_RESOURCE` 分支

为 `NO_RESOURCE` 添加 404 分支。补上之后控制流会落入 `switch` 之后的公共发送逻辑，与其余错误响应一起发出。这属于「修好已有的发送路径」，而不是新增机制。

### `BAD_REQUEST` 改为 400

改用一直闲置的 `error_400_title` 与 `error_400_form`。文案里另有两处拼写错误（`staisfy`、`get file form this server`）一并更正。

### 错误响应统一声明类型

`add_content_type()` 改为接受 MIME 类型参数，四类错误响应统一传 `text/plain; charset=utf-8`，文件响应改用它传入自身类型（原先是一行手写的 `add_response`）。错误响应的正文是纯文本句子，声明为 `text/plain` 与内容一致。

> 备选方案是把错误响应渲染为与静态页风格一致的 HTML。未采用：这些文案是纯文本，改成 HTML 需要同时维护模板与转义，收益仅是观感；且这部分响应主要被工具与扫描器消费。若后续需要自定义错误页，应由配置提供文件路径而非硬编码模板。

### 非法行结束符直接判错

循环退出后补一次判定：

```cpp
if (line_status == LINE_BAD)
    return BAD_REQUEST;
```

`LINE_OPEN`（行尚未收完）仍返回 `NO_REQUEST` 继续等待——这正是两者的区别所在。当前实现没有对二者加以区分，等于把「数据格式错误」当成了「数据还没到」。

## 四、实现要点

| 文件 | 改动 |
|:--|:--|
| `http/http_conn.cpp` | `process_write` 新增 `NO_RESOURCE` 分支、`BAD_REQUEST` 改 400、四类错误响应补充 `Content-Type`；`process_read` 补 `LINE_BAD` 判定；`add_content_type` 改为带参；更正 `error_400_form` / `error_403_form` 的拼写 |
| `http/http_conn.h` | `add_content_type` 签名同步 |

## 五、验证

### 修复前后对照

| 请求 | 修复前 | 修复后 |
|:--|:--|:--|
| `/nope.html`（文件不存在） | **000**，连接被关闭，无任何响应 | 404 + `text/plain` 正文 |
| `/../CMakeLists.txt`（路径穿越） | 404 | **400** |
| `POST /2CGISQL.cgi`（请求体缺字段） | 404 | **400** |
| `GET /` + 单独 `LF` | 无响应，连接挂到超时 | **400** |
| 错误响应头 | 无 `Content-Type` | `Content-Type: text/plain; charset=utf-8` |

### 端到端用例

新增 9 项断言，同时校验状态码、`Content-Type` 与正文关键词：

| 组 | 用例 | 结果 |
|:--|:--|:--|
| 4xx 语义 | 文件不存在 → 404、路径穿越 → 400、请求体缺字段 → 400 | 通过 |
| 403 | 在 `root/` 下放置一个 `0600` 的文件（验证后删除），请求它 → 403 | 通过 |
| 畸形请求行 | 单独 `LF` 结尾、缺少版本号、未知方法、`HTTP/1.0` → 均 400 | 通过 |
| 正常请求 | 静态页面 → 200（确认改动未波及正常路径） | 通过 |

畸形请求行通过 `nc` 直接发送原始报文构造，`curl` 无法产生这类输入。

### 回归与动态检查

| 项 | 结果 |
|:--|:--|
| 4 套用例 × 4 触发模式 × 2 并发模型（6 组合） | 全部通过 |
| `ctest` | 3/3 通过 |
| 编译 | 0 错误 0 警告 |
| `clang-format --dry-run --Werror`（23.1.1） | 无差异 |
| AddressSanitizer 全链路（4 套用例）+ 泄漏检查 | 无任何报告 |

## 六、遗留问题

1. **不支持的方法未区分 405 与 501**：`PUT` / `DELETE` / `TRACE` 等方法当前统一返回 400。按规范，方法未被实现应为 501、资源不支持该方法应为 405。方法支持集合在补充 `HEAD` 时才会明确，因此与那一项一并处理
2. **`INTERNAL_ERROR` 分支的可达性**（此条已过时，见 `019`）：本记录撰写时该分支确实只在 `process_read` 的 `default` 处触发，而那个 `default` 覆盖不到任何实际路径，因此当时判断为「没有可复现的触发方式」。此后 `013` 为 `mmap` 补上了 `MAP_FAILED` 检查、`015` 又在上传文件的读取路径补了同样的检查，该分支现已由「文件映射失败」触达（`http_conn.cpp` 中 `do_request` 与 `serve_uploaded_file` 各一处）
3. **错误响应无自定义页面**：正文硬编码在源码中。若需要按站点风格展示，应改为从配置读取错误页文件
4. **`FORBIDDEN_REQUEST` 的判定依据是 `S_IROTH`**：以「对 other 可读」作为可访问条件，语义上接近但不完全等价于「Web 服务器进程可读」。当前服务端以普通用户运行且文件属主一致，未作调整
