---
name: oxapp-work-order-delivery
description: 为 oxApp/oxMBS 工单实现、复核和交付提供统一流程：核对工单边界，补充必要中文注释，构建测试，编写阶段说明，并按安全边界提交版本。
---

# oxApp 工单交付 Skill

## 适用范围

用于 oxApp/oxMBS 的工单实现、补充注释、测试验收、阶段说明、提交和推送。它不适用于将 Mock 能力包装成真实交易、将 QMT token 写入项目、或在未授权时修改生产数据库。

## 固定流程

1. 读取当前工单、架构文档、现有分支、Git 状态和关联源码。
2. 明确本工单实现内容、不实现内容、前置依赖和可验证验收项。
3. 只修改工单边界内的文件；关键业务判断、资源所有权、跨进程/线程边界、错误处理和回滚路径使用简洁中文注释。
4. 按风险执行 `git diff --check`、CMake 构建、CTest、Python 单测或实际可执行验证。
5. 每完成一张工单，在 `outputs/` 或 `docs/work-orders/` 记录简单阶段说明：做了什么、数据如何流动、用户能看到什么、技术点、面试表述、当前边界和下一步。
6. 提交前扫描凭据；禁止提交 QMT token、数据库密码、私钥、运行配置、日志、构建目录和二进制。
7. 只有用户明确要求时才创建提交、推送分支或更新远端 `main`。

## 代码阅读和交付要求

交付时至少说明：

```text
入口/配置
  -> 对象创建
  -> 输入编码、校验或路由
  -> 核心业务调用
  -> 下游 Worker、数据库或网络处理
  -> 响应、状态和错误映射
  -> 正常关闭或失败回滚
```

对 Worker、IPC、动态库、Socket、数据库和 Qt 线程交互，说明输入来自哪里、谁拥有资源、失败后由谁处理；不要把规划能力描述成已实现能力。

## 质量与安全门禁

- Mock 数据、QMT/PG 快照和实时 Push 必须明确区分。
- Qt 只通过 Service/Transport 访问后端，不能直连数据库。
- QMT 采集 token/profile 不属于 oxApp 仓库。
- 正式低频查询与高频行情 Push 应保持分离。
- 公开仓库仅提交模板配置，真实运行配置必须被 `.gitignore` 排除。

## 常用验证命令

```bash
git diff --check
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
PYTHONPATH=client .venv/bin/python -m unittest discover -s tests/python -v
PYTHONPATH=client .venv/bin/python -m compileall -q client/vnpy_oxapp
```

如果运行环境没有 PostgreSQL、QMT/XtData 或 HP-Socket 依赖，记录为环境限制；不能用 Mock 成功替代真实链路验收。
