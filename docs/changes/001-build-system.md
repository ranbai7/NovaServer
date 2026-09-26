# 001 · 构建系统迁移至 CMake

## 一、背景与动机

项目此前使用手写 Makefile 构建，只有一条编译规则。它缺少构建类型区分、依赖探测与动态检查工具的接入能力——而这些是后续引入单元测试、多配置验证和 Sanitizer 检查的前提。

同时，语言标准此前未显式声明，编译行为依赖编译器默认值。并发模型重构需要智能指针与 `std::function` 等设施，必须先把标准固定下来。

## 二、现状分析

原 Makefile：

```makefile
CXX ?= g++
DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
endif

server: main.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp \
        ./CGImysql/sql_connection_pool.cpp webserver.cpp config.cpp
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient
```

局限：

1. 未声明语言标准
2. 无编译告警开关
3. 依赖以 `-lmysqlclient` 硬编码，未做探测
4. 无法接入 Sanitizer
5. 全量编译，改动任一文件都会重编全部

## 三、设计方案

选用 CMake，理由：

- 依赖探测交给 pkg-config，不硬编码库名与路径
- 构建类型与自定义选项是一等公民
- 后续接入测试只需追加 `add_subdirectory`
- 是 C++ 生态的事实标准，降低他人上手成本

被否决的方案：

| 方案 | 否决理由 |
|:--|:--|
| 继续维护 Makefile | 需自行实现依赖探测与多配置管理，与 CMake 的成熟度差距过大 |
| Meson | 语法更简洁，但生态与工具链支持不及 CMake |
| Bazel | 对当前项目规模而言过重，配置成本高于收益 |

## 四、实现要点

`CMakeLists.txt` 的关键配置：

| 配置 | 说明 |
|:--|:--|
| `CMAKE_CXX_STANDARD 17` 搭配 `CXX_STANDARD_REQUIRED ON` 与 `CXX_EXTENSIONS OFF` | 固定语言标准并禁用 GNU 扩展，保证可移植性 |
| `pkg_check_modules(MYSQLCLIENT REQUIRED IMPORTED_TARGET mysqlclient)` | 探测 MySQL 客户端库 |
| `target_compile_options(... -Wall -Wextra)` | 开启编译告警 |
| `ENABLE_ASAN` / `ENABLE_TSAN` | 动态检查工具开关，二者互斥 |
| `CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG"` | 固定优化级别，理由见下 |

**关于优化级别**：CMake 的 Release 配置默认使用 `-O3`，而性能基线是以 `-O2` 采集的。若沿用默认值，后续所有性能对比都会混入优化级别带来的差异。因此显式固定为 `-O2`，使对比结果只反映代码改动本身。

`build.sh` 改为调用 CMake，产物统一输出至 `build/`；配套移除 `makefile` 与根目录的旧二进制，并同步更新 README 中的构建与运行路径。

## 五、验证

### 语言标准兼容性

迁移前先确认代码未使用 C++17 移除的特性（`register`、`throw()`、`auto_ptr`）——检索结果为空，因此迁移**无需改动任何源码**。

以 `-std=c++17` 直接编译：**0 错误**。

### 构建结果

| 项 | 结果 |
|:--|:--|
| 清空 `build/` 后完整重建 | 成功，退出码 0 |
| 实际编译选项 | `-O2 -DNDEBUG -Wall -Wextra -std=c++17` |
| 产物大小 | 98 KB，与迁移前 `-O2` 构建完全一致 |
| 运行时验证 | 服务端正常启动，`/judge.html`、`/picture.html`、`/video.html`、`/upload.html`、`/log.html` 均返回 200 |

### 启用告警后发现的问题

开启 `-Wall -Wextra` 后共报告 **45 个警告**：

| 类型 | 数量 |
|:--|--:|
| `return-type` 函数缺少返回值 | 14 |
| `unused-parameter` 未使用参数 | 8 |
| `stringop-truncation` 字符串截断风险 | 6 |
| `unused-variable` 未使用变量 | 5 |
| `sign-compare` 有符号与无符号比较 | 3 |
| `reorder` 成员初始化顺序 | 3 |
| `format-truncation` 格式化截断风险 | 3 |
| `unused-result` 返回值未使用 | 1 |
| `unused-but-set-variable` 变量赋值后未使用 | 1 |
| `implicit-fallthrough` 分支意外贯穿 | 1 |

其中 `return-type` 与 `stringop-truncation` 涉及实际行为，其余多为清理项。逐项治理单独进行。

## 六、遗留问题

1. **45 个编译告警待治理**——`log.h` 中两处函数缺少 `return` 已确认为真实缺陷
2. **单元测试尚未接入**——CMake 已具备接入条件，测试框架选型与目录结构待定
3. **Sanitizer 开关已配置但未实际运行**——需在告警治理后完整验证一次
4. **`-Werror` 未启用**——待告警清零后再考虑收紧
