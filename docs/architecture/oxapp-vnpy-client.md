# 工单 #16：oxApp vn.py 客户端通信抽象与 Mock 服务

## 目标和边界

本工单在独立 `client/vnpy_oxapp` Python 包中扩展 vn.py 的 `BaseGateway` 使用方式，并提供
可替换的传输接口、确定性 Mock 服务、请求关联、Qt 线程桥接和公共页面容器。它不修改或复制
上游 vn.py 源码。

```mermaid
flowchart LR
    V["vn.py\nEventEngine / BaseGateway"] --> G["OxAppGateway\n可选 vn.py 适配"]
    P["#18-#21 页面\n只依赖 Service 接口"] --> C["ClientProtocolClient\nrequest_id + session_id"]
    G --> C
    C --> T["IClientTransport"]
    T --> M["MockTransport\n延迟/错误/断线"]
    M --> S["MockServiceRouter\n系统、用户、资金、风控"]
    C --> Q["AsyncQtBridge\nQt queued signal"]
    Q --> P
    H["后续 HpSocketTransport"] -. 替换 .-> T
    R["真实 PackMessage / oxMBS"] -. Mentor确认后接入 .-> H
```

本工单实现：临时消息模型、JSON Mock Codec、传输接口、MockTransport、MockService、`Future`
请求接口、请求/响应关联、超时和重复/迟到包防护、vn.py Gateway 适配、Qt 页面基础壳。

本工单不实现：真实 HP-Socket、真实 PackMessage、真实登录鉴权、数据库/LBM、真实行情、真实
账户持仓、下单、撤单、成交或任何柜台行为。

## 请求时序

```mermaid
sequenceDiagram
    participant UI as Qt 页面
    participant S as Service
    participant C as ClientProtocolClient
    participant T as MockTransport Timer 线程
    participant B as AsyncQtBridge

    UI->>S: list_users(page, filter)
    S->>C: request(session, service, operation)
    C->>C: 生成 request_id + 双时钟 deadline
    C->>T: send(ClientRequest)
    T->>T: MockServiceRouter 生成成功/空/错误响应
    T-->>C: ClientResponse
    C->>C: 校验 request_id + session_id + 未超时
    C-->>B: publish_response(response)
    B-->>UI: Qt queued signal 主线程更新

    Note over C,T: 超时、重复、迟到或会话不匹配响应不会二次更新 UI
```

## 模块职责

| 模块 | 文件 | 输入 -> 处理 -> 输出 |
| --- | --- | --- |
| 临时消息契约 | `client/vnpy_oxapp/message.py` | 服务请求 -> 校验关联字段和绝对 deadline -> `ClientRequest/ClientResponse`。 |
| 可替换 Codec | `client/vnpy_oxapp/codec.py` | JSON bytes -> 显式字段和类型校验 -> 临时模型；将来替换为 PackMessage Codec。 |
| 请求关联 | `client/vnpy_oxapp/connection.py` | response -> `request_id + session_id + monotonic deadline` 判断 -> Future/UI 单次完成或拒绝。 |
| Mock 传输 | `client/vnpy_oxapp/mock/mock_transport.py` | request -> Timer 异步场景控制 -> 成功、错误、延迟、断线或不回包。 |
| Mock 服务 | `client/vnpy_oxapp/mock/mock_services.py` | service/operation/filter -> 分页和权限校验 -> 业务化 Mock payload。 |
| vn.py 适配 | `client/vnpy_oxapp/gateway.py` | client state/response -> vn.py EventEngine -> Monitor 可订阅 Event。 |
| Qt 安全边界 | `client/vnpy_oxapp/ui/qt_bridge.py` | Timer 线程回调 -> Qt Signal -> GUI 主线程。 |
| 页面公共壳 | `client/vnpy_oxapp/ui/main_window.py` | 导航选择 -> `QStackedWidget` -> 后续 #18-#21 页面插槽。 |

## 验收命令

```bash
PYTHONPATH=client python3 -m unittest discover -s tests/python -v
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

安装 `client/requirements.txt` 后可启动 Mock 客户端：

```bash
PYTHONPATH=client python3 -m vnpy_oxapp.app
```

## OX-P0-04 增量：正式 Transport 与 QMT/PG 查询

正式 `HpSocketTransport` 和 `PackMessageCodec` 已加入客户端，并通过运行配置显式启用：

```text
Qt Service -> ClientProtocolClient -> HpSocketTransport
-> C++ HP-Socket -> WorkFuncRouter -> Query Worker
-> QmtServiceRouter -> QmtRepository -> ta_data PostgreSQL
-> ReplyCollector -> HP-Socket -> ClientResponse -> Qt
```

`client/vnpy_oxapp/runtime_config.py` 负责 `mock/hpsocket` 工厂和环境变量覆盖；
`MarketService.list_quotes()` 在正式模式调用 `quote.latest`，原样保留 `source=QMT/PG`、
`quote_time` 和 `receive_time`。当前 Qt 页面正式模式只显示最新快照；没有历史 Tick/K 线/五档表时显示明确边界，禁止回退 Mock。

本增量已用真实 C++ 服务和 `ta_data` 返回 `1:688693` 的 QMT 行情；认证登录仍待 `OX-P0-05`，实时订阅 Push 仍待 `OX-P1-01`。

## 后续替换点

继续实现登录/权限和实时 Push 时，保留 `ClientProtocolClient`、服务接口、请求关联和 Qt 页面层不变；
`MockTransport` 继续作为 UI 状态和回归测试的故障注入实现，不能冒充生产行情。
