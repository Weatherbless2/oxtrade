# oxtrade / oxApp

`oxtrade / oxApp` 是一个面向交易与行情研究的桌面客户端 + C++ 服务端工程。它把 Qt 工作台、认证会话、HP-Socket 通信、System V IPC、Query Worker、LBM 插件、QMT/PostgreSQL 行情查询、实时行情接入边界和前端体验设计放在同一套可构建、可测试、可交付的代码与文档体系中。

这个仓库的重点不是直接接入真实交易柜台，而是把从 UI、通信、服务路由、数据库读取到工单沉淀的完整工程链路做成可演示、可扩展、可接手的项目。

仅用于工程开发、联调验证、学习和后续生产化改造，不构成投资建议，也不代表已接入真实柜台自动下单。

## 一句话理解

用户登录后，Qt 客户端通过 HP-Socket 发起登录和行情请求；`oxMBS` 负责会话、权限、路由和低频查询分发；`lbm_qmt.so` 调用 PostgreSQL `ta_data` 中的 QMT 数据；响应再沿原路径回到 Qt 页面，展示分页行情、主数据、日历、K 线等业务结果。

```text
PySide6 Qt Client
  -> HP-Socket / PackMessage
  -> C++ oxMBS Server
  -> AuthService / Session / Permission
  -> WorkFuncRouter / System V IPC
  -> Query Worker
  -> LBM Dispatcher
  -> lbm_qmt.so
  -> QmtRepository
  -> PostgreSQL ta_data
```

## 当前进度

截至当前仓库分支，核心链路已经完成并保留在工单文档中：

| 模块 | 状态 | 当前情况 |
| --- | --- | --- |
| PySide6 Qt 客户端 | 已完成 | 无边框窗口、统一主题、导航、工作台、行情页和演示页面 |
| HP-Socket 通信 | 已完成 | C++ Server、Python/Qt Transport、PackMessage 编解码和请求关联 |
| 认证与权限 | 已完成第一版 | challenge、password proof、session、access token、权限校验和会话撤销 |
| 后端服务骨架 | 已完成 | 配置、日志、进程生命周期、路由、IPC、Worker、LBM 装载 |
| QMT/PG 查询 | 已完成 | 行情快照、标的、日历、K 线、IPO 等业务接口按工单沉淀 |
| 响应容量与分页 | 已完成 | 统一响应模型、分页、容量保护、IPC/Worker/LBM 链路验证 |
| QMT 工作台 | 已完成 | 行情中心、筛选、分页、空态/错误态、来源展示和前端体验优化 |
| 实时 QMT Push | 规划中 | `OX-P1-01` 已定义订阅、广播、HP-Socket Push 和故障处理方案 |
| TLS 与真实柜台 | 不包含 | 当前阶段不启用 TLS，不包含交易柜台、报单和真实资金操作 |

## 已有真实结果

- QMT/PostgreSQL 行情读取链路已经跑通，页面不再使用 Mock 行情作为主数据源。
- `lbm_qmt.so` 已作为独立 LBM 插件接入 Worker，替代直接硬编码路由。
- 行情页已支持分页、筛选、空态、错误态、来源展示和正式 QMT/PG 数据。
- 认证链路已覆盖 challenge、password proof、access token、session 撤销和权限校验。
- 工单、阶段说明、路线图、原型说明和架构文档已经随代码一起上传到仓库。

这些结果属于当前仓库和当前运行环境下的实现状态，不代表已完成生产部署、TLS、压测、柜台接入或全链路高可用验收。

## 系统怎么工作

```text
用户登录
  |
  v
Qt 客户端发起 challenge / login
  |
  v
oxMBS 校验账号与 password proof
  |
  v
服务端签发 session / access_token / permissions
  |
  v
客户端携带 session 发起 quote.latest / instrument.search / calendar.page / bar.page / ipo.list
  |
  v
WorkFuncRouter 分发低频业务请求
  |
  v
System V IPC -> Query Worker
  |
  v
LBM Dispatcher -> lbm_qmt.so
  |
  v
QmtRepository -> PostgreSQL ta_data
  |
  v
PackMessage response -> HP-Socket -> Qt 页面
```

整体是服务端驱动的低频查询链路；行情、主数据、日历、K 线和 IPO 都按统一协议返回。认证、权限、会话撤销、超时和错误码由服务端统一处理。

## 目录说明

```text
client/                         Python/PySide6 客户端
  vnpy_oxapp/
    ui/                         登录页、导航、行情页、工作台页面
    services/                   UserService、MarketService、Session
    hpsocket_transport.py       PackMessage HP-Socket 传输层
    market/                     本地行情适配、事件模型和开发桥接
  README.md                     客户端运行说明

src/                            C++ 服务端实现
  oxmbs/
    network/                    HP-Socket Server、Bridge、PackMessage
    auth/                       AuthService、credential store、session
    routing/                    WorkFuncRouter
    ipc/                        System V IPC 与关联管理
    worker/                     Query Worker runtime
    lbm/                        LBM loader、dispatcher、QMT 模块
    service/                    QMT Service Router、Repository Adapter
    storage/                    PostgreSQL Repository 与连接池
    config/                     INI 配置解析
    mock/                       开发联调用的 mock backend

include/                        C++ 公共头文件
config/                         配置模板
docs/                           架构、工单、路线图、阶段说明
  work-orders/                  OX-P0 / OX-P1 工单
  architecture/                 架构说明
outputs/                        原型、阶段说明、交付说明
tests/                          C++ 与 Python 测试
```

## 关键技术栈

- C++17、CMake、CTest
- HP-Socket、PackMessage、JSON 协议
- PostgreSQL、libpq、连接池、QMT `ta_data`
- PySide6、Python 3.12、客户端传输封装
- System V IPC、Query Worker、LBM 动态插件
- Mermaid、Markdown 工单与阶段说明

## 最快启动方式

### 1. 克隆仓库

```bash
git clone git@github.com:Weatherbless2/oxtrade.git
cd oxtrade
```

### 2. 构建 C++ 服务

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

### 3. 启动客户端

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r client/requirements.txt
PYTHONPATH=client python -m unittest discover -s tests/python -v
PYTHONPATH=client python -m vnpy_oxapp.app
```

### 4. 配置文件

服务端配置模板：

```bash
cp config/mbsconfig.ini.example config/mbsconfig.ini
```

客户端配置模板：

```bash
cp config/oxapp.ini.example config/oxapp.ini
```

当前默认演示用户：

- 用户名：`yangjh`
- 密码：`mock-password`

## 常用验证命令

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
PYTHONPATH=client .venv/bin/python -m unittest discover -s tests/python -v
PYTHONPATH=client .venv/bin/python -m compileall -q client/vnpy_oxapp
```

## 工单和学习路线

建议按下面顺序阅读，而不是一开始就看全部代码：

1. `docs/work-orders/OX-production-roadmap.md`
2. `docs/work-orders/OX-P0-04-client-real-transport.md`
3. `docs/work-orders/OX-P0-07-qmt-service-router.md`
4. `docs/work-orders/OX-P0-08-reply-bridge.md`
5. `docs/work-orders/OX-P0-09-lbm-runtime.md`
6. `docs/work-orders/OX-P0-10-response-capacity.md`
7. `docs/work-orders/OX-P0-11-lbm-qmt.md`
8. `docs/work-orders/OX-P0-12-qmt-workbench.md`
9. `outputs/oxapp-前端体验与QMT工作台改版规划-v1.0.md`
10. `outputs/oxapp-frontend-prototype-v2.html`

每张工单都尽量保留代码、测试、阶段说明和设计边界。

## 当前限制和下一步

- 当前阶段不包含 TLS、真实柜台、报单和资金操作。
- `OX-P1-01` 的实时 QMT Push 已设计但未作为本轮核心落地。
- QMT token、本地 profile、数据库密码和私有密钥都不属于仓库提交内容。
- 当前仓库更强调工程可演进性，而不是直接交付生产交易能力。

## GitHub 仓库描述建议

如果要把仓库描述同步到 GitHub，建议填写：

`oxtrade / oxApp：Qt 桌面客户端 + C++ 服务端 + HP-Socket + QMT/PostgreSQL 行情链路，覆盖登录、认证、会话、LBM 插件、行情工作台和工程化文档。`

## 关于仓库内容和敏感配置

当前仓库保留了代码、工单、阶段说明、架构文档、原型和运行说明。按安全边界，仓库不提交 QMT token、数据库密码、本机 profile、SSH 私钥或任何本地凭据文件。

本地 `build/`、`.venv/`、`logs/`、`config/lbm/`、运行时配置和凭据文件都不应上传。
