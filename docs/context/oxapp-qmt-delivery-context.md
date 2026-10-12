# oxApp / QMT / PostgreSQL 交付上下文

最后更新：2026-10-12

## 这份文档的作用

这份文档保存项目在多轮开发中的关键决策、工程边界、工单顺序和迁移注意事项。它面向后续接手者，避免只看代码时把 Mock、QMT 数据库快照、实时行情 Push 和真实交易柜台混为一谈。

## 已确定的架构选择

第一阶段采用 **C++ HP-Socket Server + Python/PySide6 Qt 客户端**：

```text
Qt 客户端
  -> Python HpSocketTransport
  -> C++ oxMBS / HP-Socket
  -> AuthService、WorkFuncRouter、System V IPC
  -> Query Worker、LBM Dispatcher、lbm_qmt.so
  -> QmtRepository
  -> PostgreSQL ta_data
```

- 第一阶段不包含交易柜台、报单、持仓和资金操作。
- 第一阶段不启用 TLS；网络隔离和访问控制由部署环境承担。
- 实时行情 Push 与低频数据库查询分离。低频查询可通过 Worker/IPC/LBM；高频 Push 不应经过慢查询队列。
- Qt 页面不直连 PostgreSQL，也不直接调用 QMT/XtData。

## QMT 数据与 Token 边界

QMT 数据采集属于独立的 Windows QMT/XtData 环境，而不是 oxApp 进程的一部分：

```text
券商开通 QMT 权限
  -> Windows QMT 客户端和本地授权 profile
  -> XtData / xtquant 采集程序
  -> PostgreSQL ta_data
  -> oxMBS 查询服务
  -> Qt 工作台
```

- QMT token、Windows 加密 profile、券商账号和数据库密码不能提交到仓库。
- oxApp 内的 `access_token` 是用户登录后的应用会话令牌，不是 QMT token。
- QMT 原始采集项目和 token/profile 不在本仓库；本仓库只负责消费已经落入 PostgreSQL 的业务数据。

## 当前已实现的主链路

1. `PackMessage` 和 C++ HP-Socket Server 提供客户端请求和应答边界。
2. `AuthService` 提供 challenge、password proof、session、access token、权限和会话撤销。
3. `WorkFuncRouter -> System V Request Queue -> Query Worker -> Reply Queue -> ReplyCollector` 提供低频请求链路。
4. `lbm_qmt.so` 通过 `QmtRepository` 查询 PostgreSQL `ta_data` 中的 QMT 股票、期货、交易日历和最新行情等数据。
5. Python/Qt 客户端通过 `HpSocketTransport`、`UserService`、`MarketService` 访问服务端，而不直接访问数据库。
6. 行情工作台提供全市场筛选、服务端分页、来源标识、空态和错误态；页面不应将数据库快照描述为实时 Push。

## 工单状态与推荐顺序

已沉淀的主要工单位于 `docs/work-orders/`：

| 工单 | 主题 | 当前说明 |
| --- | --- | --- |
| `OX-P0-04` | Python/Qt 正式传输 | HP-Socket 客户端边界 |
| `OX-P0-07` | QMT Service Router | QMT/PG 查询路由 |
| `OX-P0-08` | Reply Bridge | Worker 回包回到真实连接 |
| `OX-P0-09` | LBM Runtime | Worker 统一经 Dispatcher |
| `OX-P0-10` | 容量与分页 | 大结果集分页与响应上限 |
| `OX-P0-11` | `lbm_qmt.so` | 正式 QMT LBM 动态模块 |
| `OX-P0-12` | QMT 工作台 | Qt 全市场分页浏览 |
| `OX-P1-01` | 实时行情 Push | 已规划，尚需正式源适配和验收 |

后续应优先完成真实数据库部署可达性、QMT 实时源到 `IMarketSource` 的跨进程适配、订阅 Push、可观测性、联调与部署回滚，而不是再扩大 Mock 页面范围。

## 迁移与恢复建议

电脑迁移时，代码和文档可通过 GitHub 恢复；数据库和 QMT 授权需要单独处理：

1. 优先使用 `pg_dump -Fc` 对可访问的 PostgreSQL 业务库做整库备份。
2. 保存备份校验值、库名、版本、扩展、恢复命令和非敏感连接地址。
3. 在新机器上通过 `pg_restore` 恢复数据库；不要把 dump、密码或 QMT token 提交到 Git。
4. 如果历史库无法获得，重新部署 QMT 客户端和采集环境，并重新采集/入库；这一条依赖券商 QMT 权限和原始授权环境。
5. `config/lbm/` 是构建目录中 LBM 动态库的本机符号链接，部署时重新生成，不提交。

## 公开仓库安全边界

可公开提交：源代码、测试、配置模板、工单、架构说明、原型、阶段说明、运行命令和不含凭据的样例数据。

不能公开提交：QMT token、Windows profile、数据库密码、生产连接串、私钥、真实登录密码、资金账号、行情原始授权文件、运行日志和构建二进制。

## 快速阅读顺序

1. `README.md`
2. `docs/work-orders/OX-production-roadmap.md`
3. `docs/architecture/postgresql-repository.md`
4. `docs/work-orders/OX-P0-08-reply-bridge.md`
5. `docs/work-orders/OX-P0-11-lbm-qmt.md`
6. `docs/work-orders/OX-P0-12-qmt-workbench.md`
7. `docs/work-orders/OX-P1-01-qmt-market-push.md`
8. `outputs/oxapp-OX-UX-01阶段说明.md`
