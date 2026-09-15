# OX-P0-11 阶段说明：正式 QMT 查询模块

## 做了什么

真实 QMT/PG 查询现在有了正式动态模块 `lbm_qmt.so`。Worker 按 XML 找到模块，模块通过 Worker 提供的 Repository 端口查询 `ta_data`，再把结果原路回给前端。上一阶段的临时桥接仅作为部署前兼容路径，正式 XML 同键路由会优先。

```text
Qt -> HP-Socket -> IPC -> Worker -> Dispatcher -> lbm_qmt.so -> QmtRepository -> ta_data
```

## 用户能看到什么

这一步仍主要是后台能力，但它保证后续页面展示的股票、期货、日历和最新行情可以由正式模块提供，而不是 Mock 或临时直调。下一步 `OX-P0-12` 会把这些真实全量数据做成可浏览的 Qt 行情工作台。

## 用到的技术

- C++ 动态库：`dlopen/dlsym` 按 XML 延迟加载模块。
- 版本化 ABI：模块只拿到只读业务端口，不自行建立数据库连接。
- PostgreSQL Repository：仍由每个 Worker fork 后独立连接的连接池使用。
- 服务端分页：真实数据按页返回，不把全表塞进 IPC 消息。

## 验收和面试讲法

结果：C++ `20/20`、相关 Python `13/13` 通过。

“我把三个 QMT 查询封装为独立 LBM 动态模块，采用 XML 路由和版本化宿主端口。模块不持有网络、IPC 或数据库连接池，只通过 Worker 注入的 Repository 查 PG，因此业务模块可以替换而不改主进程和前端协议。”

## 下一步

直接进入 `OX-P0-12`，把全市场真实 QMT/PG 数据做成用户可操作的 Qt 工作台。
