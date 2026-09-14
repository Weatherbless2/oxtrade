# OX-P0-06 PostgreSQL Repository

## 作用

本模块是 oxMBS 查询业务与 PostgreSQL 之间的唯一访问边界。它不属于 Qt 客户端，不读取 QMT SDK，也不把用户输入拼接到 SQL 中。

```text
Query Service（OX-P0-07）
        |
        v
QmtRepository
  ├── 固定参数化 SQL
  ├── 分页与排序白名单
  └── 字段映射
        |
        v
PostgresConnectionPool
  ├── 有界连接数
  ├── acquire 超时
  └── 失效连接重建
        |
        v
libpq -> PostgreSQL ta_data
```

## 已实现查询

| Repository 方法 | PostgreSQL 表 | 业务用途 |
| --- | --- | --- |
| `list_stock_instruments` | `ta_data.stkinfo` | 股票、可转债和回购类主数据分页查询 |
| `list_futures_instruments` | `ta_data.ft_stkinfo` | D/F/H/Z/I/G 六个期货市场合约分页查询 |
| `list_calendar` | `ta_data.trading_calendar` | 日期范围、市场集合和 `Y` 派生结果查询 |
| `list_stock_quotes` | `ta_data.stkprice` + `stkinfo` | 股票类最新行情与名称关联 |
| `list_futures_quotes` | `ta_data.ft_stkprice` + `ft_stkinfo` | 期货最新行情与合约名称关联 |
| `list_freshness` | `stkprice`、`ft_stkprice` | 行情行数、最新行情时间和最新接收时间 |

八张核心表以外的历史 K 线、Tick 和盘口明细没有查询入口；后续 Service 必须返回能力未提供错误，不能猜测表名。

## 连接和错误边界

- `PostgresConnection` 使用 `PQconnectdbParams`，连接串为空时由 `DATABASE_URL`/`PG*` 环境变量提供。
- 每条连接建立时设置 `statement_timeout`、`lock_timeout` 和 `idle_in_transaction_session_timeout`。
- `PostgresConnectionPool::Lease` 以 RAII 归还连接；坏连接不回收到空闲池，下一次获取时创建新连接。
- `PostgresTransaction` 提供 `BEGIN`、`COMMIT` 和析构回滚，后续写业务可复用。
- PostgreSQL SQLSTATE `57014`/`55P03` 映射为超时，连接失败、池关闭和参数错误分别映射为稳定 Repository 状态。

## QMT 口径

Repository 只使用已审计的 `ta_data` 字段。`Y` 仍是 0/1/2 的业务集合，`9` 仍使用补充日历；期货只按 D/F/H/Z/I/G 处理。QMT 采集和 PG 装载由独立 `qmt_market_data` 项目负责，不在本模块直接调用 XtData。

