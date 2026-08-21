# oxApp Python 客户端（工单 #16）

本目录是对 `vn.py` Qt 客户端的业务化扩展，不复制或修改上游 `vn.py` 源码。
当前只实现通信抽象、确定性 Mock 服务、请求关联和公共 Qt 页面容器；它**没有**连接真实
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
  ui/              左导航、右页面容器和 Qt 线程桥接（可选依赖）
```

后续真实接入时，只需要新增 `HpSocketTransport` 和正式协议 Codec，并保持页面和服务接口不变。
