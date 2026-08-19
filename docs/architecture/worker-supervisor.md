# Query Worker 进程池与 Supervisor

## 范围

本工单为 oxMBS/oxServer 增加 Linux/WSL 下的查询 Worker 进程生命周期管理。
Worker 当前只提供可运行的进程骨架，后续由 IPC 和业务执行工单接入请求处理。

本工单实现：

- Supervisor 创建指定数量的同质 Query Worker；
- 保存 Worker 的 PID、状态、启动时间、退出信息和重启次数；
- 使用非阻塞 `waitpid(..., WNOHANG)` 检查子进程是否退出；
- 异常退出后自动重启，保持目标进程数量；
- 通过 `SIGTERM` 请求优雅退出，超时后使用 `SIGKILL` 兜底；
- 父进程最终使用 `waitpid` 回收子进程，避免僵尸进程。

本工单不实现：

- System V 请求队列和应答队列；
- `task_seq`、`session_id` 等跨进程消息协议；
- WorkFunc 高低频路由；
- `funclist.xml`、LBM 动态加载；
- 数据库、OTL、ODBC 和具体查询业务；
- Windows IPC 适配。

## 进程结构

```mermaid
flowchart LR
    M["oxMBS 主进程"] --> S["WorkerSupervisor"]
    S --> W1["Query Worker 1"]
    S --> W2["Query Worker 2"]
    S --> WN["Query Worker N"]
    S -. "waitpid / SIGTERM / restart" .-> W1
    S -. "waitpid / SIGTERM / restart" .-> W2
    S -. "waitpid / SIGTERM / restart" .-> WN
    Q["后续：System V IPC"] -. "不在本工单" .-> W1
```

## Worker 状态变化

```mermaid
stateDiagram-v2
    [*] --> Created
    Created --> Starting: start()
    Starting --> Running: fork 成功
    Starting --> Stopped: fork 失败
    Running --> Crashed: 异常退出
    Crashed --> Restarting: restart()
    Restarting --> Running: fork 成功
    Restarting --> Crashed: fork 失败
    Running --> Stopping: stop()
    Stopping --> Stopped: SIGTERM 后正常退出
    Stopping --> Stopped: 超时 SIGKILL 并回收
    Stopped --> Starting: 再次 start()
```

## 异常重启流程

```mermaid
sequenceDiagram
    participant S as Supervisor
    participant W as Query Worker
    participant K as Linux Kernel

    S->>K: fork()
    K-->>S: 返回 Worker PID
    S->>S: 保存 PID，状态 Running
    W->>W: 执行 Worker 入口
    W-->>K: 异常退出
    S->>K: waitpid(pid, WNOHANG)
    K-->>S: 返回退出状态
    S->>S: 解析退出码/信号
    S->>S: restart_count + 1
    S->>K: fork() 重新创建 Worker
    K-->>S: 返回新 PID
    S->>S: 更新 PID，状态 Running
```

## 测试覆盖

`oxapp_worker_supervisor_test` 覆盖：

1. 创建 2 个 Worker 并验证 PID 不同；
2. 验证正常停止和全部回收；
3. Worker 非零退出后由 Supervisor 自动重启；
4. 验证重启次数增加；
5. 拒绝 Worker 数量为 0 的配置。

## 后续接入顺序

```text
#2 Worker 生命周期
-> #4 System V IPC
-> #5 请求/应答协议
-> #7 Worker 阻塞消费和业务执行
-> #8 LBM 动态加载
```
