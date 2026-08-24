# oxApp Python 客户端（工单 #16/#17）

本目录是对 `vn.py` Qt 客户端的业务化扩展，不复制或修改上游 `vn.py` 源码。
当前实现通信抽象、确定性 Mock 服务、请求关联、公共 Qt 页面容器、客户端日志和控件加载耗时统计；它**没有**连接真实
HP-Socket、`PackMessage`、oxMBS、数据库、柜台或交易系统。

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

没有安装 PySide6 或 vn.py 时，消息、Mock 服务和请求关联的纯 Python 测试仍可运行；启动 Qt
客户端会给出明确的依赖提示。

## 目录

```text
vnpy_oxapp/
  message.py       临时适配消息模型，不等于真实 PackMessage
  codec.py         JSON 编解码，方便 Mock 和测试追踪
  connection.py    request_id/session_id 关联与超时保护
  transport.py     传输接口和状态模型
  mock/            可控延迟、错误、断线、权限等 Mock 实现
  services/        后续页面共用的服务接口和 Mock 业务数据
  gateway.py       vn.py BaseGateway 适配层（可选依赖）
  logging.py       oxTrader 启动日志和组件加载计时
  ui/              左导航、右页面容器和 Qt 线程桥接（可选依赖）
```

启动客户端时会在仓库根目录 `logs/` 生成 `oxTrader_YYYYMMDD_HHMMSS.log`，并记录启动、连接、
页面构造和停止事件。`OxAppMainWindow.load_timings` 保存布局和页面构造耗时，后续 #18-#21
可以继续对具体业务控件分别计时。

后续真实接入时，只需要新增 `HpSocketTransport` 和正式协议 Codec，并保持页面和服务接口不变。

当前 #18 已提供系统管理页的 Mock 交互：系统菜单、数据字典、字典明细和在线日志。页面支持
关键字筛选、状态筛选、刷新、异步加载、空结果、错误和无权限状态；#19-#21 会沿用相同的表格
和服务抽象。
