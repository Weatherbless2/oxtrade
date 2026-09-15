# OX-P0-09 阶段说明：真实查询先进入 LBM

## 做了什么

以前后台 Worker 收到 QMT 行情请求后，直接调用查询服务。现在它会先进入 LBM 调度器，再决定该走正式动态模块还是迁移桥接。这为正式 `lbm_qmt.so` 和后面的全量行情页面打通了主干。

```text
原来：Worker -> QMT 查询服务 -> PG
现在：Worker -> LBM Dispatcher -> 正式 .so（下一步）或三个精确 QMT 桥接 -> PG
```

## 用户能看到什么

本阶段是后端主干改造，现有真实 QMT/PG 分页查询不会倒退，但页面外观暂时不变。完整的“登录后分页浏览全市场真实股票和期货”将在 `OX-P0-11` 和 `OX-P0-12` 完成后交付；实时自动刷新是 `OX-P1-01`。

## 技术与架构

- C++ 函数指针和上下文：Worker 只依赖统一的 Dispatcher 入口。
- `fork` 后初始化：每个 Worker 自己打开 PostgreSQL 连接，避免跨进程复制连接。
- XML 路由和动态库：正式模块将以 XML 定位，且比临时桥接优先。
- System V IPC：仍只负责低频请求；实时行情不走队列。

## 验收

```bash
cd /root/projects/TA/oxApp
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

结果：C++ `19/19`，相关 Python `13/13` 均通过。

## 面试讲法

“我把低频 QMT/PG 查询由直接 Service 调用改为经 LBM Dispatcher 调度。在正式模块完成前，对三个冻结接口做精确桥接，XML 动态模块优先。每个 Worker 在 fork 后独立建立 PG 连接，因此后续迁移到动态业务模块时不需要改网络、IPC 和回包主链路。”

## 下一步

进入 `OX-P0-11`：实现正式 `lbm_qmt.so`，随后做 `OX-P0-12`，把真实多页 QMT/PG 数据直接呈现在 Qt 行情工作台。
