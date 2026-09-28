# oxApp OX-P0-05A 阶段说明

## 本阶段完成了什么

本阶段把 oxApp 的登录从演示流程接入正式认证链路。用户登录后，服务端创建会话并返回访问令牌；行情请求会携带会话信息，由后端校验 `market.quote.read` 等权限后，继续进入 LBM/Worker 和 PostgreSQL `ta_data` 查询。

同时将正式 QMT/PG 行情工作台的默认分页调整为每页 5 条。这样首屏可以稳定显示多条真实行情，并通过下一页继续浏览全量数据；本次联调确认数据库中行情总量为 6080 条。

## 使用的技术

- PySide6：登录页、工作台和行情表格。
- Python `HpSocketTransport`：客户端通过 HP-Socket 协议发送异步请求。
- C++ HP-Socket Bridge：接收请求、调用认证服务并返回响应。
- `PackMessage`：统一承载 `auth.challenge`、`auth.login` 和 `quote` 请求。
- `AuthService`：校验账号、生成服务端会话和权限集合。
- LBM/Worker：执行后端业务任务并收集响应。
- PostgreSQL `ta_data`：提供 QMT 股票和期货行情数据。

## 面试中怎么讲

“我把 Qt 登录页接入了正式的异步认证链路。客户端通过 HP-Socket 发送 challenge/login 请求，C++ 服务端由 AuthService 完成密码证明校验、Session 创建和权限绑定。登录成功后，行情请求会携带 Session，经过权限校验后进入 LBM/Worker 查询 PostgreSQL 中的 QMT 数据，最后通过 ReplyCollector 回包到 Qt 主线程。为适配 IPC 单帧容量，行情页面采用多条小分页，用户可以连续翻页查看全量数据。”

## 架构工作图

```text
Qt 登录页
    |
    | auth.challenge / auth.login
    v
Python HpSocketTransport
    |
    v
C++ HP-Socket Bridge
    |
    v
AuthService -> Session + Permissions
    |
    | quote.list (携带 Session)
    v
权限校验 -> LBM/Worker -> QmtRepository
                              |
                              v
                       PostgreSQL ta_data
                              |
                              v
ReplyCollector -> HP-Socket 回包 -> Qt 行情表格
```

## 用户现在能看到的效果

- 使用本地开发账号登录后，界面自动进入“行情中心”。
- 首屏显示 5 条真实 QMT/PG 行情，而不是 Mock 数据。
- 页面显示全量总数 `6080`，可使用上一页/下一页继续浏览。
- 当前实现是实时数据库快照查询；实时 Push 行情仍属于 `OX-P1-01`，尚未宣称完成。

## 本次启动说明

本次使用一次性本地自动登录启动器，密码只通过当前进程环境变量传入，不写入源码、配置、日志或文档。关闭该客户端后自动登录脚本即失效。
