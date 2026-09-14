# oxApp Python 客户端（工单 #16/#17/#14）

本目录是对 `vn.py` Qt 客户端的业务化扩展，不复制或修改上游 `vn.py` 源码。
当前实现通信抽象、确定性 Mock 服务、正式 `HpSocketTransport`、`PackMessage` 编解码、请求关联、公共 Qt 页面容器、
客户端日志和控件加载耗时。默认启动仍为 Mock；显式启用 `hpsocket` 后，行情查询可以经过 oxMBS 读取 PostgreSQL `ta_data`。

## 安装和运行

在 WSL 仓库根目录执行：

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r client/requirements.txt
PYTHONPATH=client python -m unittest discover -s tests/python -v
PYTHONPATH=client python -m vnpy_oxapp.app
```

开发/验收时可使用 `QT_QPA_PLATFORM=offscreen` 运行无窗口 Qt 测试；有 WSLg/X11 时直接启动即可看到桌面窗口。
客户端启动默认进入登录欢迎页，使用 Mock 用户（例如 `yangjh` / `mock-password`）登录后进入 Mock 工作台。
正式 `hpsocket` 模式的连接和 `quote.latest` 查询已完成，真实登录/权限仍由 `OX-P0-05` 接入。

没有安装 PySide6 或 vn.py 时，消息、Mock 服务和请求关联的纯 Python 测试仍可运行；启动 Qt
客户端会给出明确的依赖提示。

## 目录

```text
vnpy_oxapp/
  message.py       临时适配消息模型，不等于真实 PackMessage
  codec.py         JSON 编解码，方便 Mock 和测试追踪
  pack_message.py  正式 OXPK PackMessage 编解码
  hpsocket_transport.py  Python/Qt 正式 HP-Socket TCP Transport
  runtime_config.py  Mock/正式 Transport 运行模式工厂
  connection.py    request_id/session_id 关联与超时保护
  transport.py     传输接口和状态模型
  mock/            可控延迟、错误、断线、权限等 Mock 实现
  services/        后续页面共用的服务接口和 Mock 业务数据
  gateway.py       vn.py BaseGateway 适配层（可选依赖）
  logging.py       oxTrader 启动日志和组件加载计时
  ui/              固定三栏壳层、标题栏、登录页、导航、配置面板、业务页和 Qt 线程桥接（可选依赖）
    shell_state.py    壳层会话/菜单/面板状态（不保存密码）
    menu_config.py    一级菜单和左侧菜单配置
    title_bar.py      无边框标题栏和窗口控制
    navigation_panel.py  固定左侧系统菜单（可见六项，保留旧索引兼容）
    secondary_navigation.py 一级菜单下方二级上下文导航
    config_panel.py   账号与配置面板（自绘 Switch）
    welcome_page.py   欢迎首页、产品摘要和走势状态
```

启动客户端时会在仓库根目录 `logs/` 生成 `oxTrader_YYYYMMDD_HHMMSS.log`，并记录启动、连接、
页面构造和停止事件。当前壳层采用无边框窗口、深蓝/浅色主题、60px 标题栏、46px 二级导航、固定三栏（左导航/中央工作区/右配置面板）和底部状态栏，
右面板可以显示/隐藏但不支持 Dock 浮动或拖拽。`OxAppMainWindow.load_timings` 保存布局和页面构造耗时。

主题可通过标题栏主题按钮或右侧配置面板切换：

```python
from vnpy_oxapp.ui.theme import apply_workbench_theme
apply_workbench_theme(app, "light")  # 或 "dark_navy"
```

正式后台模式通过运行配置切换，不需要页面直接接触 Socket：

```bash
export OXAPP_TRANSPORT=hpsocket
export OXAPP_HPSOCKET_HOST=127.0.0.1
export OXAPP_HPSOCKET_PORT=18080
export OXAPP_SESSION_ID=desktop-readonly
PYTHONPATH=client python -m vnpy_oxapp.app
```

该模式不会把数据库密码写入客户端；Qt 页面只通过 Service 请求 `quote.latest`，返回中的 `source=QMT/PG` 可用于验收真实来源。

## 开发版行情接入（#14）

`market/` 目录提供客户端行情抽象：`MarketHub` 提供标准化快照、去重、乱序保护、有界 Tick 缓冲和分钟 K 线；
后台线程通过 `AsyncQtBridge` 的 Qt 信号进入主线程，行情页按 200ms 默认节流刷新。`source=mock` 默认使用
`oxApp` 仓库内的 Mock 行情源；可选 `PyTdxJsonLineFeed` 只作为兼容开发适配，连接失败时自动回退
`MockMarketFeed`，不要求也不会自动启动其他项目。

当前 #14 还提供 `MarketWireFeed`，用于验证 oxMBS 推送帧到客户端标准事件的边界：它实现固定头、显式
little-endian 字段、半包/粘包、订阅过滤和源序号保护。它通过 `feed_bytes()` 接收外部传输层交付的字节，
不会自行创建 HP-Socket，也不会启动另一个行情项目。

这不是生产低时延行情 Push 实现：客户端的正式 `quote.latest` 查询已经可以连接 oxMBS/QMT/PG，
但实时行情源仍通过同仓库 `oxMWH`
的 `IMarketSource` 接入，正式发布协议通过 `IMarketPublisher` 接入，不应把开发 Mock 作为真实下单或风控依据。
配置和详细边界见 [行情路由说明](../docs/architecture/market-routing.md)。

当前 #18-#21 已提供完整的 Mock 管理客户端：系统菜单、数据字典、字典明细、在线日志、用户、
角色、站点、角色菜单权限、产品、项目、资金账号、风控参数、规则和异常事件。页面支持关键字筛选、
状态筛选、刷新、标签懒加载、空结果、错误和无权限状态；后续只需替换服务实现即可接入真实后台。
和服务抽象。
