# oxApp

`oxApp` is the desktop client entry point for the ox platform. This initial
baseline provides a small, portable C++ build that later work can extend with
the Qt interface, HP-Socket message client, logging, and management pages.

The project currently does not connect to oxMBS, oxMWH, a database, a broker,
or a trading counter.

## Build

Prerequisites:

- CMake 3.20 or newer
- C++17 compiler

Configure and build on Linux or WSL:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Run the smoke test:

```bash
ctest --test-dir build --output-on-failure
```

Run the application:

```bash
./build/bin/oxApp
```

Run the oxMBS configuration and runtime logging bootstrap:

```bash
cp config/mbsconfig.ini.example config/mbsconfig.ini
./build/bin/oxMbsBootstrap config/mbsconfig.ini
```

`oxMbsBootstrap` is the work-order #3 backend foundation. It validates the
INI configuration, creates a per-run log file, mirrors key events to stdout,
and reports Linux/WSL CPU, memory, and log-disk information. It does not yet
start Query Workers, open System V queues, or process business messages.

## Layout

```text
config/     Checked-in configuration templates only
docs/       Architecture and development notes
include/    Public C++ headers
src/        Application source
tests/      CTest-based automated tests
client/     #16 Python/vn.py extension, Mock transport and Qt page shell
```

## 工单 #16 Mock 客户端

`client/vnpy_oxapp` 是独立的 Python 客户端扩展：它提供可替换的通信接口、Mock 系统/用户/资金/
风控服务、请求关联与 Qt 页面壳，并且不修改上游 vn.py。当前是 Mock-only，不连接真实 HP-Socket、
PackMessage、oxMBS、数据库或交易柜台。

安装依赖并运行：

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r client/requirements.txt
PYTHONPATH=client python -m unittest discover -s tests/python -v
PYTHONPATH=client python -m vnpy_oxapp.app
```

## Development Boundary

The first baseline deliberately excludes business services, dynamic LBM
loading, ODBC/OTL, HP-Socket, Qt Widgets, broker APIs, and real trading. Each
will be introduced through a separate work order and feature branch.

Local logs, build output, runtime configuration, credentials, and private keys
must not be committed.
