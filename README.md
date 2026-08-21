# oxApp

`oxApp` 是 UltraQuant 交易平台的客户端工程入口。当前版本提供可移植的
C++ 工程基线，后续将逐步接入 Qt 界面、HP-Socket 消息客户端、客户端日志和
系统管理页面。

当前工程还不会连接 oxMBS、oxMWH、数据库、券商柜台或真实交易环境。

## 构建环境

依赖环境：

- CMake 3.20 或更高版本
- 支持 C++17 的编译器

Linux 或 WSL 中配置并构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

运行全部自动化测试：

```bash
ctest --test-dir build --output-on-failure
```

运行客户端程序：

```bash
./build/bin/oxApp
```

## 目录结构

```text
config/     纳入版本管理的配置模板
docs/       架构说明和开发文档
include/    对外暴露的 C++ 头文件
src/        应用程序源代码
tests/      CTest 自动化测试
```

## 当前开发边界

当前工程基线暂不包含业务服务、LBM 动态加载、ODBC/OTL、HP-Socket、Qt Widgets、
券商接口和真实交易功能。每项能力都通过独立工单和功能分支逐步实现。

本地日志、构建目录、运行时配置、账号凭据和私钥禁止提交到仓库。

## 工单开发流程

`main` 分支只保存稳定基线，不直接开发功能。每个工单使用独立的
`feature/<工单号>-<主题>` 分支，完成编码和测试后推送远程，并创建合并请求：

```text
工单 -> 创建功能分支 -> 本地开发 -> 编译和测试 -> 推送远程分支
     -> 创建合并请求 -> 代码审查 -> 合并到 main -> 关闭工单
```

提交信息应包含工单号，例如：

```text
feat(worker): add query runtime (#7)
```
