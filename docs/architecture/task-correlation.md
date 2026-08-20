# 工单 #5：跨进程任务协议与请求应答关联

## 1. 工单边界

本工单在工单 #4 原始字节双 System V 队列之上增加稳定的任务帧和主进程关联表：

```text
主进程收到请求
-> register_request(session_id, timeout) 分配 task_seq
-> encode_task(IpcTaskMsg)
-> Request Queue
-> Worker decode_task / 业务处理
-> encode_response(IpcRespMsg)，原样带回 task_seq/session_id
-> Reply Queue
-> 主进程 decode_response
-> resolve_response() 匹配会话或报告迟到/错会话结果
```

本工单不接入 HP-Socket、WorkFunc、Worker 业务函数、LBM、数据库和 Windows IPC。

## 2. 协议帧

正文放在工单 #4 `SystemVMessageQueue` 的消息正文中，System V 的 `long mtype` 仍由队列封装层负责；下面是业务正文的固定 48 字节头，字段按小端序逐字段编码：

| 偏移 | 长度 | 字段 | 含义 |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | `0x4F584D42`，识别 oxMBS 任务帧。 |
| 4 | 2 | `version` | 当前为 `1`，不兼容版本拒绝。 |
| 6 | 2 | `kind` | `1=Task`，`2=Response`。 |
| 8 | 2 | `header_bytes` | 当前固定为 `48`。 |
| 10 | 2 | `reserved` | 当前必须为 `0`。 |
| 12 | 4 | `frame_bytes` | 头和 payload 的总字节数，必须等于本次接收长度。 |
| 16 | 8 | `task_seq` | 主进程生成的非零递增任务号。 |
| 24 | 8 | `session_id` | 客户端会话标识，Worker 原样透传。 |
| 32 | 4 | `msg_type` | 业务消息类型，当前只做协议字段保存。 |
| 36 | 4 | `func_id` | 业务函数标识，后续交给 LBM 路由。 |
| 40 | 4 | `response_status` | Task 必须为 `0`；Response 为 `Ok/Rejected/Failed/TimedOut`。 |
| 44 | 4 | `payload_bytes` | payload 的精确长度。 |
| 48 | N | `payload` | 二进制业务正文，可含 `0` 字节。 |

`IpcTaskMsg` 和 `IpcRespMsg` 是 C++ 业务对象，不直接作为跨进程内存布局。编码函数显式写入字段，解码函数按偏移读取，避免 C++ ABI、结构体填充和大小端差异改变协议含义。

## 3. 主进程关联表

`TaskCorrelationRegistry` 只存在于主进程：

- `register_request()` 校验非零 `session_id` 和正超时，使用原子 CAS 分配全局递增 `task_seq`，再把会话和截止时间写入互斥保护的 `pending_`。
- `resolve_response()` 先按 `task_seq` 查找，再比对 `session_id`。匹配成功后删除记录；未知序号、错会话和过期应答不会投递给客户端。
- `expire()` 定期删除达到截止时间的记录并返回 `task_seq/session_id`，上层可生成超时回包或记录日志。
- `steady_clock` 用于超时，避免系统墙上时间被手工调整导致任务提前或延后过期。

## 4. 验收测试

| 测试 | 对应文件 | 验证 |
| --- | --- | --- |
| Task/Response 往返 | `tests/test_task_protocol.cpp` | 字段、状态和包含 `0` 字节的 payload 原样保留。 |
| 协议损坏拒绝 | `tests/test_task_protocol.cpp` | 魔数、版本、类型、头长、帧长、payload 长度和最大帧限制。 |
| 关联和超时 | `tests/test_task_protocol.cpp` | 正常匹配、错会话保留、迟到应答、定时清理和非法注册。 |
| 父子进程真实往返 | `tests/test_task_protocol.cpp` | fork 后经工单 #4 双队列传输 Task/Response，并由主进程按关联表匹配。 |

构建和测试：

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```
