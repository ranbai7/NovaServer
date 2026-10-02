# NovaServer

[![CI](https://github.com/ranbai7/NovaServer/actions/workflows/ci.yml/badge.svg)](https://github.com/ranbai7/NovaServer/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)

Linux 下 C++ 轻量级 Web 服务器。并发模型为**主从 Reactor（one loop per thread）**：主线程只负责 `accept`，新连接按轮转策略分发给若干子 Reactor 线程，每个线程持有独立的 `epoll` 实例与事件循环；连接的文件描述符、协议解析状态与其超时定时器全部归属所属线程。定时器、跨线程唤醒与退出信号统一为文件描述符（`timerfd` / `eventfd` / `signalfd`），不使用信号处理函数。

构建、配置与运行见[快速开始](#快速开始)；各模块的职责划分与一次请求的事件流见 [docs/architecture.md](docs/architecture.md)；各阶段的改动与实测数据见 [docs/changes/](docs/changes/) 与 [docs/performance.md](docs/performance.md)。

> **项目来源**：本项目的架构设计参考自开源项目 [qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer)（MIT 协议），在其基础上进行二次开发与重构。原项目版权归原作者所有，详见 [LICENSE](LICENSE)。

## 目录

| [相对原项目的改动](#相对原项目的改动) | [特性](#特性) | [快速开始](#快速开始) | [配置](#配置) | [架构与设计文档](#架构与设计文档) | [界面展示](#界面展示) | [测试](#测试) | [项目结构](#项目结构) | [许可证](#许可证) |
|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|

## 相对原项目的改动

原项目是半同步 / 半反应堆模型：主线程与工作线程共享同一组连接，事件注册与连接生命周期分散在多个路径上。本项目的改动集中在并发模型与验证体系，按工程量排序如下。

### 一、并发模型重构

- **主从 Reactor（one loop per thread）**：主线程只 `accept`，新连接按轮转策略分发给子 Reactor 线程池；每个子线程持有独立的 `epoll` 实例与事件循环，一个连接的全部事件都在其所属线程内处理，无需跨线程同步。
- **事件源统一为文件描述符**：`epoll` + `timerfd` + `eventfd` + `signalfd`。空闲连接回收由 `timerfd` 驱动，跨线程唤醒由 `eventfd` 完成，退出信号由 `signalfd` 读入——**全项目没有信号处理函数**，也消除了原实现中「以忙等标志位等待工作线程」的路径。
- **连接生命周期收敛**：连接的关闭入口（对端关闭、读写错误、超时回收、主动关闭、进程退出）全部汇聚到唯一出口且幂等；`Channel` 通过弱引用保护回调，`EPOLL_CTL_DEL` 恒先于 `close(fd)`，避免描述符被复用后误操作新连接。

### 二、日志系统重写

- **异步写入改为缓冲池批量落盘**：调用线程只把整行日志追加进线程私有缓冲，由专门的写盘线程按缓冲块（默认 64 KiB）整块落盘，写入路径上不再逐行产生系统调用。每行日志的写系统调用降到约 **1/4.3**。
- **背压可见**：写盘速度跟不上时丢弃新行并在日志中记下「已丢弃 N 行」，使丢失可见而非无声无息。
- **同步方式保留每行落盘语义**：语义是「写入即落盘」，不参与缓冲。

### 三、内存占用

原实现按最大连接数预分配连接对象及其读写缓冲（每连接约 3 KB × 65536），常驻内存恒为 **262.8 MB** 且与负载无关。本项目改为按需分配：静态资源用 `mmap` 按文件长度映射，连接对象动态管理，常驻内存降至 **12.6 MB**。

### 四、正确性与协议完整性

- **静态资源路径规范化**：请求路径先解码 `%XX` 再规范化，越出站点根目录的路径被拒绝，避免任意文件读取。
- **状态码语义修正**：区分「规范定义但未实现的方法」（`501`）与「无法识别的请求记号」（`400`），补齐 `404` / `413` / `415` / `431` 等响应。
- **请求头校验**：`Host` 缺失、头部超长、`Content-Length` 与 `Expect: 100-continue` 等按规范处理。
- **管线化**：一个 TCP 段内的多个完整请求被依次处理，不再静默丢弃。
- **启动参数校验**：端口、连接数、关闭方式等越界时直接报错终止，而不是静默截断或忽略。

### 五、安全性

- **口令加盐哈希**：注册与登录使用 PBKDF2-HMAC-SHA256，库中不保存明文；历史明文记录在启动时自动升级。
- **数据库访问参数化**：用户名与口令经 `mysql_stmt` 绑定传递，不再拼接 SQL 字符串。
- **上传约束**：扩展名白名单、单文件体积上限，下载一律加 `Content-Disposition: attachment`。

### 六、工程基础与验证体系

- **构建与规范**：CMake 构建、`.clang-format` 统一风格、`-Wall -Wextra` 零告警。
- **持续集成**：GitHub Actions 在 Release 与 Debug 两种配置下从零构建并运行单元测试。
- **单元测试**：GoogleTest 覆盖路径规范化、配置解析、事件循环、定时器队列、口令哈希、日志与并发原语。
- **动态检查**：AddressSanitizer 全链路运行（静态资源、注册登录、上传、退出）、ThreadSanitizer 覆盖并发场景，均无报告。
- **设计记录**：`docs/changes/` 下有 35 份记录，逐项写明背景、方案、实现要点与实测数据。

### 新增功能

- **文件上传**：支持 `multipart/form-data`，解析 boundary 后提取文件名与内容，保存至 `./upload/`。
- **上传内容可访问**：`/upload` 列出已上传文件，`/upload/<文件名>` 下载。
- **MIME 类型映射**：按扩展名返回正确的 `Content-Type`，解决部分浏览器直接显示源码或乱码的问题。
- **路由扩展**：新增 `/8`（上传页面）与 `/upload`（上传接口）。
- **界面重做**：所有 HTML 页面统一为现代卡片式布局。

## 特性

* 并发模型为主从 Reactor（one loop per thread），连接与其事件源、定时器归属同一线程
* 事件源统一为 `epoll` + `timerfd` + `eventfd` + `signalfd`，没有信号处理函数
* 使用**状态机**解析 HTTP 请求报文，支持 **GET / POST / HEAD**，支持管线化请求
* 数据库实现 Web 端用户**注册、登录**，口令以 PBKDF2-HMAC-SHA256 加盐哈希保存
* **同步 / 异步日志**：异步写入下由专门的写盘线程按缓冲块批量落盘，支持按日期与行数轮转
* 支持 `multipart/form-data` 文件上传，受扩展名白名单与体积上限约束，下载一律按附件处理
* 静态资源支持路径解码与规范化、按扩展名映射 MIME 类型、`mmap` 零拷贝发送
* 单元测试 + GitHub Actions CI + AddressSanitizer / ThreadSanitizer 验证

## 快速开始

### 环境要求

| 项 | 要求 |
|:--|:--|
| 操作系统 | Linux（在 Ubuntu 22.04 上开发与验证，内核 6.8） |
| 编译器 | 支持 C++17（GCC 9+ / Clang 10+） |
| 构建 | CMake ≥ 3.16 |
| 数据库 | MySQL（在 8.0 上验证，5.7 亦可） |
| 依赖库 | `libmysqlclient-dev`、`libssl-dev`（口令哈希）；`libgtest-dev`（仅单元测试需要） |

```bash
sudo apt install build-essential cmake libmysqlclient-dev libssl-dev libgtest-dev
```

### 准备数据库

```sql
-- 建立 yourdb 库
CREATE DATABASE yourdb;

-- 创建 user 表
USE yourdb;
CREATE TABLE user(
    username char(50) NULL,
    passwd varchar(255) NULL
) ENGINE=InnoDB;
```

口令在库中以**加盐哈希**（PBKDF2-HMAC-SHA256）保存。直接插入明文同样可用：服务端启动时会把明文记录自动升级为哈希，并在需要时把 `passwd` 列加宽到 `varchar(255)`，因此上述建表语句即使沿用旧的 `char(50)` 也能自动修正。

### 准备配置文件

仓库提供 `config.example.ini` 作为模板，其中包含监听端口、数据库连接、日志等全部可配置项。复制并按实际环境修改，**数据库口令写在这里而不是源码中**：

```bash
cp config.example.ini config.ini
# 编辑 config.ini 中的 [database] 一节
```

`config.ini` 已被 `.gitignore` 忽略，不会被提交。命令行参数优先于配置文件，因此可以两者混用——把口令放在配置文件中，临时换端口时用 `-p` 覆盖。各配置项的含义见 `config.example.ini` 内的注释。

### 构建

```bash
sh ./build.sh
```

等价于 `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`，产物为 `build/server`。

### 运行

服务端使用相对路径查找站点根目录、日志目录与上传目录，**需在仓库根目录下启动**：

```bash
./build/server
```

默认监听 9006 端口。浏览器访问 `http://<服务器IP>:9006` 即可（本机访问用 `http://127.0.0.1:9006`）。

按 `Ctrl+C` 或发送 `SIGTERM` 可优雅退出，退出码为 0 且日志完整落盘。

## 配置

### 命令行参数

```bash
./build/server [-p port] [-l LOGWrite] [-m TRIGMode] [-o OPT_LINGER] [-s sql_num] [-t thread_num] [-c close_log] [-f config_file]
```

以上参数均为可选，按需搭配使用即可。未给出的参数取自配置文件（默认 `./config.ini`，文件不存在时回退到内置默认值）。

* `-p`，自定义端口号
	* 默认 9006
* `-l`，选择日志写入方式，默认异步写入
	* 0，同步写入：每写一行就落盘。进程崩溃不丢日志，代价是每行的写入开销都压在调用线程上
	* 1，异步写入：写入线程私有的缓冲，由专门的写盘线程按整块批量落盘。代价是进程被 SIGKILL 或崩溃时，最多丢掉 `flush_interval` 内的日志（优雅退出仍完整落盘）
* `-m`，listenfd 和 connfd 的模式组合，默认使用 LT + LT
	* 0，表示使用 LT + LT
	* 1，表示使用 LT + ET
	* 2，表示使用 ET + LT
	* 3，表示使用 ET + ET
* `-o`，优雅关闭连接，默认不使用
	* 0，不使用
	* 1，使用
	* **本服务端有意让它不产生效果**：真正生效需把 `SO_LINGER` 施加在连接描述符上，而 `l_linger = 0` 的语义是关闭时发送 RST 而非 FIN、并丢弃未发送数据，会截断响应并破坏长连接，对 Web 服务端有害。保留该参数是为兼容原项目的命令行接口
* `-s`，数据库连接数量
	* 默认为 8
* `-t`，子 Reactor 线程数
	* 默认为 8
	* 0 表示不建子线程，全部连接归主循环（用于与多线程分发对照）
* `-c`，关闭日志，默认打开
	* 0，打开日志
	* 1，关闭日志
* `-f`，指定配置文件路径
	* 默认 `./config.ini`，文件不存在时使用内置默认值
	* 显式指定却找不到文件时终止启动

示例命令：

```bash
./build/server -p 9007 -l 1 -m 0 -o 1 -s 10 -t 4 -c 1
```

上述命令的含义：端口 9007、异步写入日志、LT + LT、使用优雅关闭连接、连接池内 10 条连接、4 个子 Reactor 线程、关闭日志。

### 日志配置

`[log]` 节的各项取值见 [config.example.ini](config.example.ini) 内的注释，其中几项有取值范围限制，越界会在启动阶段直接报错而不是静默夹取：

| 键 | 默认值 | 有效范围 | 说明 |
|:--|--:|:--|:--|
| `write_mode` | 1 | 0 或 1 | 0 = 同步（每行落盘），1 = 异步（批量落盘） |
| `buf_size` | 2000 | 128 ~ 4096 | 单条日志的长度上限，超出会被截断 |
| `split_lines` | 800000 | ≥ 1 | 单个文件的行数**软**上限：写入以整块为单位，可能多出不到一块的行数 |
| `batch_buf_size` | 65536 | 4096 ~ 8388608 | 缓冲块大小。内存中最多保留 9 块，即日志的内存上界是它的 9 倍 |
| `flush_interval` | 1000 | 1 ~ 3600000 | 异步写入的定时刷新间隔（毫秒） |

## 架构与设计文档

| 文档 | 内容 |
|:--|:--|
| [docs/guide/](docs/guide/) | 项目解读：逐册讲解整体架构、各技术专题与性能度量，说明每处设计的来由与取舍（12 册 + 导读） |
| [docs/summary.md](docs/summary.md) | 优化笔记：按阶段叙述做了什么、为什么、怎么做、效果如何 |
| [docs/architecture.md](docs/architecture.md) | 系统架构：并发模型、模块划分、连接归属约定、一次请求的事件流 |
| [docs/performance.md](docs/performance.md) | 性能数据汇总：各阶段压测数据、优化前后对比与测量局限 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 优化路线图：分阶段目标、技术方案与逐项进度 |
| [docs/changes/](docs/changes/) | 35 份逐项改动记录：背景、方案、实现要点与实测数据 |

## 界面展示

> * 首界面

<div align=center><img src="root/judge.png" height="429"/> </div>

> * 注册界面

<div align=center><img src="root/register.png" height="429"/> </div>

> * 登录界面

<div align=center><img src="root/log.png" height="429"/> </div>

> * 欢迎界面

<div align=center><img src="root/welcome.png" height="429"/> </div>

> * 图片展示界面

<div align=center><img src="root/pictrue.png" height="429"/> </div>

> * 视频播放界面

<div align=center><img src="root/video.png" height="429"/> </div>

> * 文件上传界面

<div align=center><img src="root/upload.png" height="429"/> </div>

> * 文件上传成功界面

<div align=center><img src="root/uploadSuccess.png" height="429"/> </div>

## 测试

### 单元测试

单元测试依赖 GoogleTest：

```bash
sudo apt install libgtest-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

测试目标覆盖路径规范化、配置解析、事件循环、定时器队列、口令哈希、日志与并发原语。CI 在 Release 与 Debug 两种配置下各跑一遍。

### 压力测试

工具为 `wrk`，`-c100`，服务端以 `-c 1` 关闭日志，每组 5 次取中位数。子 Reactor 线程数的影响：

| `-t` | QPS | P50 (ms) | P99 (ms) | RSS (MB) |
|--:|--:|--:|--:|--:|
| 1 | 28372 | 3.35 | 7.61 | 11.7 |
| **2** | **48185** | **2.06** | **2.96** | **11.8** |
| 4 | 36815 | 2.58 | 8.00 | 12.0 |
| 8 | 33151 | 3.11 | 16.79 | 12.6 |

测试机的 4 个 vCPU 实为 2 物理核 + 超线程，且 `wrk` 与服务端同机，因此 2 个子线程即已占满可用并行度，更多线程反而带来调度开销。上表取自 [`034`](docs/changes/034-final-perf-revalidation.md) 的同时段对照，原始输出在 `test_pressure/results/perf_2026-10-02/`。

**优化前后的整体对照**：阶段零基线（[`000`](docs/changes/000-baseline.md)）为后续对比设定了四项指标，如今都有了结果：

| 指标 | 优化前（`000`） | 优化后（`034`） | 变化 |
|:--|--:|--:|:--|
| 默认配置 QPS | 19808 | **33151** | **+67%** |
| P99 延迟 | 899.99 ms | **16.79 ms** | 降至约 1/54 |
| 常驻内存 | 262.8 MB | **12.6 MB** | **−95%** |
| 线程数对吞吐的影响 | 无差异（1.3% 以内） | `-t1` 28372 → `-t2` **48185** | 出现显著影响 |

> `000` 与 `034` 采于不同日期，含约 ±10%~27% 的时段差异，上表仅作方向性参考。**精确对照以同一时段重测的 A/B 为准**，见 [`031`](docs/changes/031-baseline-comparison.md) 与 [`034`](docs/changes/034-final-perf-revalidation.md)。各阶段完整数据见 [docs/performance.md](docs/performance.md)，压测脚本见 [test_pressure/bench.sh](test_pressure/bench.sh)。

## 项目结构

```
NovaServer/
├── main.cpp              程序入口：参数解析、初始化与启动
├── webserver.cpp/.h      服务器装配与启停
├── config.cpp/.h         配置文件与命令行参数解析
├── net/                  网络层：事件循环、通道、监听器、连接、定时器、信号
├── http/                 HTTP 协议层：状态机解析与响应组装
├── log/                  日志系统：同步 / 异步写入、缓冲池批量落盘
├── auth/                 口令哈希：PBKDF2-HMAC-SHA256
├── lock/                 并发原语包装
├── CGImysql/             数据库连接池
├── root/                 站点静态资源与页面
├── tests/                单元测试（GoogleTest）
├── test_pressure/        压力测试脚本与工具
├── docs/                 架构说明、性能数据、逐项优化记录
└── upload/               用户上传内容（运行时生成）
```

## 许可证

本项目基于 [qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 二次开发，遵守原项目的 MIT 许可证。详见 [LICENSE](LICENSE)。

## 致谢

感谢原作者的开源。如果这个项目对你有帮助，欢迎给[原项目](https://github.com/qinguoyi/TinyWebServer)点个 Star⭐️。
