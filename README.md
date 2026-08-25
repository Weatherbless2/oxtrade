# oxApp

`oxApp` 是 UltraQuant 平台的 Qt 桌面客户端工程。当前分支用于在真实数据层
（工单 #9-#13）暂停期间，先用 Mock 数据把最终架构中的客户端业务链路跑通。

当前 Mock 能力包括：登录/注销、菜单、数据字典、系统日志、服务状态、用户、角色、
站点、托管服务配置、角色菜单权限、用户菜单权限、产品权限、资产权限、产品、项目、
资金账号、资产-资金账号关联、风控参数/规则/事件、行情报价/Tick/K 线/盘口、订阅、
限价委托、撤单、模拟成交、资金、持仓、委托和成交查询；管理配置和权限资源支持新增、
编辑、启停和二次确认删除，审计日志、服务状态和风控事件保留不可删除。

这些数据和交易结果全部是 `Mock-only`，不代表真实登录、真实授权、真实数据库落库、
真实行情、真实柜台报单或真实风控执行；不会连接生产柜台、交易所或数据库。

## Build

Prerequisites:

- CMake 3.20 or newer
- C++17 compiler

Configure and build on Linux or WSL:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

运行 C++ 测试：

```bash
ctest --test-dir build --output-on-failure
```

运行基础 C++ 程序：

```bash
./build/bin/oxApp
```

Run the oxMBS configuration and runtime logging bootstrap:

```bash
cp config/mbsconfig.ini.example config/mbsconfig.ini
./build/bin/oxMbsBootstrap config/mbsconfig.ini
```

`oxMbsBootstrap` 是工单 #3 的后台基础程序。它校验 INI 配置、创建本次运行日志、
输出关键事件，并报告 Linux/WSL CPU、内存和磁盘信息；它不是本次 Mock 客户端的完整业务入口。

## Layout

```text
config/     Checked-in configuration templates only
docs/       Architecture and development notes
include/    Public C++ headers
src/        Application source
tests/      CTest-based automated tests
client/     #16 Python/vn.py extension, Mock transport and Qt page shell
```

## Mock 端到端客户端

`client/vnpy_oxapp` 是基于 vn.py Qt 组织方式改造的独立客户端扩展：它提供可替换的通信接口、
统一请求/响应/错误模型、会话、客户端日志、控件加载耗时和左侧导航工作台。页面只依赖 Service
接口，不直接读取 Mock 容器或数据库。

管理类请求的实际 Mock 链路为：

```text
Qt 页面 -> Service -> MockBackendTransport -> oxMbsMockBackend
-> WorkFuncRouter -> System V Request Queue -> Query Worker
-> Mock LBM 动态库 -> Reply Queue -> ReplyCollector -> Qt 页面
```

行情和交易保持架构要求的独立边界：

```text
行情页面 -> MockMarketGateway（模拟 oxMWH）
交易页面 -> MockTradeGateway（模拟交易核心）
账户/持仓/委托/成交查询 -> MockQueryGateway（模拟查询核心）
```

三条路径共用异步请求关联和错误模型，但行情/交易不会进入慢查询 IPC；正式 HP-Socket、
oxMWH、交易网关、查询核心和真实 LBM 将在后续工单替换适配层。

安装依赖并运行：

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r client/requirements.txt
PYTHONPATH=client python -m unittest discover -s tests/python -v
PYTHONPATH=client python -m vnpy_oxapp.app
```

## 运行方式

```bash
cmake --build build -j2
PYTHONPATH=client .venv/bin/python -m vnpy_oxapp.app
```

登录演示用户：`yangjh`；演示密码：`mock-password`。

## 开发边界

本分支只完成 Mock-first 的客户端可见业务演示和 #1-#8 基础设施联调。工单 #9-#13
仍未实现：`tt_stream/OTL/ODBC`、数据库表实体生成、正式 `lbm_sys/lbm_right/lbm_fund`
及真实持久化。正式 HP-Socket/PackMessage 也未接入，临时 stdin/stdout 协议和 Mock
消息编号不能作为生产协议使用。

Local logs, build output, runtime configuration, credentials, and private keys
must not be committed.
