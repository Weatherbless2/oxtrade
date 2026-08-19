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
```

## Development Boundary

The first baseline deliberately excludes business services, dynamic LBM
loading, ODBC/OTL, HP-Socket, Qt Widgets, broker APIs, and real trading. Each
will be introduced through a separate work order and feature branch.

Local logs, build output, runtime configuration, credentials, and private keys
must not be committed.
