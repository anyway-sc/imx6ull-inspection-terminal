# Git 工作流落地记录

日期：2026-11-19。

本次为现有个人学习项目建立后续开发流程。原始整理提交 `e0f2e9a` 保持原样，源码来源继续见 [NOTICE.md](../NOTICE.md)。本记录中的提交、检查与合并均对应此次实际操作。

## 分支与提交

- 起点：`main`，提交 `e0f2e9a`。
- 工作分支：`chore/git-workflow`。
- 合并目标：`main`。

| 提交 | 目的 |
| --- | --- |
| `d55d115` | 建立分支、提交、验证和恢复规范，加入 PR / Issue 模板 |
| `d2c6466` | 加入 Windows Qt 回归和 Linux 服务构建工作流，说明 CI 范围 |
| 本记录所在的后续文档提交 | 保存实际验证结果、流程基线和变更记录 |

提交按独立目的拆分；原整理提交之前的逐步开发过程未在当前仓库中补写。

## 本次实际验证

验证源码为 `d2c6466`，从 Git 暂存索引导出到纯英文临时目录。随后新增的文件为验证记录和变更说明，不修改业务源码或构建逻辑。

环境：Windows、Qt 6.8.3、MinGW GCC 13.1.0、Ninja、Release；启动检查使用 Python 和本机 Git/MSYS。

| 检查 | 本次结果 |
| --- | --- |
| Qt 客户端与测试配置、构建 | 成功，54 个构建步骤完成 |
| CTest | 10/10 通过，总耗时 31.60 秒 |
| `python tests/startup_check.py` | 12 项启动脚本模拟检查通过 |
| `python tests/sysv_startup_check.py` | 33 项 SysV 隔离检查通过 |
| `bash -n scripts/start-server.sh scripts/vehicle-stack` | 退出码 0 |
| `git diff --cached --check` | 提交前通过 |

初次在沙箱中执行时，Git Bash 的进程管道和 Python 临时目录访问被系统拒绝；获得沙箱外执行批准后，在新的临时副本中完成上述检查。

这些检查使用模拟数据、临时文件和受管测试进程。当前 CI 的 Linux 主机服务构建尚未在本机 Windows 环境执行；本次未进行 ARM 交叉链接、内核模块构建或新增实机验证。既有硬件证据见 [验证记录](validation.md)。

## 本地合并与基线标签

本次使用 `--no-ff` 合并，保留真实的工作分支提交和单独的合并节点。工作分支保留供对照，最终合并节点由带注释标签 `workflow-baseline-20261119` 标识。

```sh
git switch main
git merge --no-ff chore/git-workflow -m "chore(workflow): 合并 Git 工作流建设分支"
git tag -a workflow-baseline-20261119 -m "建立 Git 工作流基线：规范、CI 配置与本机验证记录"
```

查看实际图谱和标签：

```sh
git log --oneline --graph --all --decorate
git show --no-patch workflow-baseline-20261119
```

此标签表示工程流程基线，不代表经过完整验收的产品发布版本。

## 远程状态与后续入口

本次先完成本地提交、验证、合并与标签；未推送这些新增引用，也未创建 GitHub PR。远程 Actions 结果和 `main` 的强制保护设置均未核实。本地通过的检查不能写成“远程 CI 已通过”或“他人评审通过”。

后续发布前核对远程历史，再将本次引用推送到已有 `origin`：

```sh
git fetch origin
git log --oneline --graph --all --decorate
git push --atomic origin main chore/git-workflow refs/tags/workflow-baseline-20261119
```

推送主线和工作分支会触发新配置的 CI，应再查看对应提交的远程结果。后续功能和修复按 [开发与提交规范](../CONTRIBUTING.md) 创建 PR，使用模板记录问题、改动和实际验证结果。

## 可核实的项目描述

> 项目采用主分支和工作分支管理。本次 Git 工作流建设在独立分支上分批提交了规范、CI 配置和验证记录，完成本机构建与回归检查后，通过保留合并节点的方式合入主线，并用带注释标签标记流程基线。当前已经有 PR / Issue 模板，远程 PR 和 CI 运行需要在后续发布时继续落实。

## 后续模块集成记录

建立规范之后，继续完成了八个功能模块的分支接入、检查和合并，见 [按功能模块集成复盘](module-replay.md)。
