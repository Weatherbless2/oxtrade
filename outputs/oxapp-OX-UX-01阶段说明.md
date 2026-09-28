# oxApp OX-UX-01 阶段说明

## 1. 本阶段完成了什么

本阶段把已经确认的 OX-UX-01 原型方向落到了 PySide6 前端，并接入现有正式行情查询链路。登录页保持非全屏小窗；登录后进入深蓝工作台，包含自定义标题栏、左侧系统菜单、中央业务区、右侧账号与配置面板、底部状态栏，以及深蓝/浅色主题切换。

行情中心不再只显示一条演示记录：正式 `hpsocket` 模式下，页面通过 `MarketService` 请求 `quote.latest`，后端从 PostgreSQL `ta_data` 返回全量统计和分页快照。页面增加总量、本页数量、最近入库、当前范围四个摘要卡片，并支持选中行情行查看详情。

## 2. 用户能看到的变化

| 位置 | 用户可见结果 |
| --- | --- |
| 登录页 | 默认是居中的小窗，不强制全屏；账号输入框预填 `yangjh`，仍通过正式认证请求登录 |
| 标题栏 | 有导航、系统配置、主题、账号和窗口控制入口 |
| 左侧菜单 | 工作台、行情中心、策略研究、交易终端、产品管理、系统管理，可收起 |
| 右侧面板 | 账号摘要、连接状态、环境、主题、自动刷新和退出登录，可隐藏 |
| 行情中心 | 显示全市场分页数据、`6080` 总量、本页多条真实行情和 QMT/PG 来源 |
| 行情详情 | 点击行后展示标的、最新价、涨跌、成交量、行情时间、入库时间、市场和资产类别 |
| 表格视觉 | 取消刺眼的白色交替行，采用低对比底色、悬停态和选中态 |
| 底部状态栏 | 显示连接、任务、消息、延迟、主题、版本和日期 |

## 3. 使用的技术

- `PySide6`：窗口、布局、表格、信号与槽、主题 QSS。
- `QMainWindow + FramelessWindowHint`：自定义标题栏和窗口按钮。
- `QStackedWidget`：登录态与工作台态隔离，登录后切换中央页面。
- `QHBoxLayout / QVBoxLayout / QGridLayout`：固定三栏和详情字段布局。
- `ClientSession`：只在进程内保存 Session、Token 和权限，不保存密码。
- `UserService`：执行正式 challenge/login 两阶段认证。
- `HpSocketTransport + PackMessage`：Python/Qt 客户端通过 TCP 访问 C++ HP-Socket Server。
- `MarketService`：页面只调用 `quote.latest`，不直接访问数据库或 Socket。
- PostgreSQL `ta_data`：后端行情数据源，页面回包保留 `source=QMT/PG`。

## 4. 实际架构工作图

```text
用户
  │ 点击登录、菜单、主题、行情行
  ▼
PySide6 OxAppMainWindow
  ├── LoginPage ── UserService ── ClientSession
  ├── MarketPage ── MarketService
  ├── NavigationPanel / ConfigPanel / TitleBar
  └── AsyncQtBridge（后台回包转 Qt 主线程）
             │
             ▼
Python HpSocketTransport
             │ PackMessage / TCP
             ▼
C++ HP-Socket Server
             │ 认证、路由、Worker/LBM
             ▼
PostgreSQL ta_data（QMT 全量与最新行情）
             │
             └── quote.latest 回包：total=6080，items=5，source=QMT/PG
```

## 5. 面试中怎么讲

可以这样说明：

> 我负责把 oxApp 的 PySide6 桌面壳层从原型落成可用工作台，并保持 UI 与通信层解耦。登录页通过 UserService 走 challenge/login，Session 只在内存中维护；行情页面不直连数据库，而是通过 MarketService 和 Python HpSocketTransport 调用 C++ HP-Socket 服务，后端从 PostgreSQL 的 QMT 数据返回分页快照。为了让正式数据可验证，我在页面上展示全量统计、分页条数、来源和入库时间，并增加选中行详情面板；这样用户看到的是实际 QMT/PG 回包，而不是页面内写死的 Mock 数据。

窗口方面使用无边框 QMainWindow、自定义标题栏和 QStackedWidget 管理登录态；左、右面板通过状态模型显示/隐藏，主题由统一 QSS token 切换。一个实际处理的问题是 WSLg 下 `showMaximized()` 的窗口状态更新存在事件延迟，因此用 Qt 窗口状态同步和事件处理保证按钮状态与用户操作一致。

## 6. 本阶段验证记录

- Python `compileall` 通过。
- 登录窗口最大化/还原验收通过。
- 登录、菜单、左侧导航、右侧面板和主题相关用例分别通过。
- 正式联调：`yangjh / 123456` 登录成功。
- 正式行情回包：`total=6080`、当前页 `items=5`、来源 `QMT/PG`。
- 当前新版 Qt 客户端已启动，进程使用 `hpsocket` 模式连接 `127.0.0.1:18080`。

## 7. 当前边界

本阶段完成的是全量/最新行情快照工作台。实时行情订阅、广播、序列号、断线补发和慢客户端策略仍属于 `OX-P1-01`，没有把快照查询误称为实时 Push。交易下单、TLS 和生产部署也不在本阶段范围内。

## 8. 启动命令

在 WSL 项目根目录执行：

```bash
cd /root/projects/TA/oxApp
export PYTHONPATH=client
export OXAPP_TRANSPORT=hpsocket
export OXAPP_HPSOCKET_HOST=127.0.0.1
export OXAPP_HPSOCKET_PORT=18080
export OXAPP_SESSION_ID=desktop-anonymous
python -m vnpy_oxapp.app
```

登录后打开左侧“行情中心”即可查看真实 QMT/PG 分页数据。
