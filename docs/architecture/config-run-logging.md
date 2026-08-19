# oxMBS 配置与运行日志基础

## 工单边界

本工单在 #1 工程基线上增加后台配置、运行日志和基础系统信息采集，不接入
Worker、System V IPC、HP-Socket、数据库或业务模块。

```mermaid
flowchart LR
    F["mbsconfig.ini"] --> P["MbsConfigLoader\n解析与类型转换"]
    P --> V["必填项、范围、白名单校验"]
    V --> C["MbsConfig\n运行时配置对象"]
    C -. "后续传入" .-> W["WorkerSupervisor"]
    C --> L["RunLogger\n文件 + stdout"]
    L --> R["独立 run 日志"]
    L --> D["Docker logs"]
    C --> S["SystemInfoCollector\nCPU / 内存 / 磁盘"]
    X["IPC、SQL、Qt 日志"] -. "不在本工单" .-> L
```

## 启动数据流

```mermaid
sequenceDiagram
    participant M as oxMbsBootstrap
    participant C as MbsConfigLoader
    participant L as RunLogger
    participant S as SystemInfoCollector
    participant O as 日志文件/stdout

    M->>C: load(mbsconfig.ini)
    C->>C: 解析 section/key 和类型
    C->>C: 校验必填项、范围、未知字段
    C-->>M: MbsConfig 或错误信息
    M->>L: open(log_directory, level)
    L->>O: 创建本次启动独立日志
    M->>S: collect(log_directory)
    S-->>M: CPU、内存、磁盘信息
    M->>L: 记录启动、地址、Worker 数量和系统信息
    L->>O: 同步写入文件和 stdout
```

## 配置范围

| Section | Key | 类型 | 约束 |
| --- | --- | --- | --- |
| `service` | `name` | string | 非空 |
| `service` | `environment` | string | 非空 |
| `server` | `host` | string | 非空 |
| `server` | `port` | uint16 | 1-65535 |
| `worker` | `query_worker_count` | size | 1-128 |
| `worker` | `shutdown_timeout_ms` | uint32 | 1-600000 |
| `logging` | `directory` | path | 非空，可创建 |
| `logging` | `level` | enum | DEBUG/INFO/WARN/ERROR |

未知 section、未知 key、重复 key、缺少必填 key 和非法值都会拒绝启动，并返回包含行号或字段名的诊断信息。

## 日志格式

```text
YYYYMMDD HHMMSSmmm|[5位pid][6位tid][module][level]: message
```

例如：

```text
20260819 101523042|[03210][012345][oxMBS][INFO]: service started
```

日志器使用互斥锁保护文件和 stdout 的整行写入，按最低级别过滤消息，每次 `open()` 创建新的时间戳日志文件；目录不存在时自动创建。

## 验收

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/oxMbsBootstrap config/mbsconfig.ini
```

验收重点：合法配置可转换为 `MbsConfig`，非法配置能定位原因；日志文件和 stdout 的关键事件格式一致；WSL 下可以读取 CPU、内存和日志目录所在磁盘信息。
