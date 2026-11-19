# 开发与提交规范

本项目采用以 `main` 为主线的轻量流程：建立工作分支，按独立目的提交，检查和验证后合并。适用于个人项目，也为后续协作保留入口。

## 分支约定

| 分支 | 用途 |
| --- | --- |
| `main` | 已完成相应验证的集成版本 |
| `feat/<name>` | 新功能，例如 `feat/sensor-status` |
| `fix/<name>` | 问题修复，例如 `fix/sr04-retry` |
| `docs/<name>` | 文档修改 |
| `test/<name>` | 测试修改 |
| `build/<name>` / `ci/<name>` | 构建或持续集成修改 |
| `chore/<name>` | 工程维护，例如 `chore/git-workflow` |

分支从最新的 `main` 建立。`main` 的稳定性依靠检查和验证维护，分支名称本身不会保证稳定。

```sh
git switch main
git pull --ff-only
git switch -c fix/sr04-retry
```

`pull --ff-only` 在本地和远程历史分叉时停止，应先检查提交关系再决定如何整合。未提交的修改需要先提交或临时保存。

## 提交约定

提交说明采用 `类型(范围): 中文说明`，范围可以省略。

| 类型 | 用途 | 示例 |
| --- | --- | --- |
| `feat` | 新增功能 | `feat(client): 增加采样状态提示` |
| `fix` | 修复问题 | `fix(server): 修复距离读取失败后的状态处理` |
| `docs` | 文档 | `docs: 补充开发板部署说明` |
| `test` | 测试 | `test(protocol): 补充 JPEG 分段接收检查` |
| `build` | 构建 | `build: 调整交叉编译配置说明` |
| `ci` | 持续集成 | `ci: 接入项目构建和回归检查` |
| `chore` | 工程维护或合并 | `chore(workflow): 合并 Git 工作流建设分支` |

一个提交对应一个独立目的。功能修改、无关格式整理和部署配置修改应分别提交；修复需要说明问题和验证方式。

提交前查看改动，按文件或代码块选择本次内容：

```sh
git status
git diff
git add server/sr04.c
git diff --staged
git diff --cached --check
git commit -m "fix(server): 修复距离读取失败后的状态处理"
```

以上文件和提交说明仅展示流程，不表示该示例修复已经发生。

## 验证与 PR

提交源码修改时，按 [构建说明](docs/build.md) 和 [测试说明](tests/README.md) 运行相应检查。Qt/协议改动需要执行相关 CTest；驱动和设备树改动还需记录匹配的 BSP、板型和实机验证范围。

```sh
git push -u origin fix/sr04-retry
```

推送工作分支后，创建目标为 `main` 的 PR，填写仓库内置模板：说明问题、结果、实际运行的检查和未覆盖范围。个人项目可以自行检查，但应注明检查者身份；有其他审阅人时再记录对应评审。

CI 定义在 `.github/workflows/ci.yml`，包含 Windows Qt 构建与 11 项 CTest、Linux 服务构建与 6 项非 Qt 回归，以及脚本语法检查。核实本次 PR 的运行结果后再合并。首次引入工作流时，发布前只能核实本机检查，记录中应明确这一状态。

GitHub 合并方式使用 **Create a merge commit**，保留提交和分支合并节点。合并后更新本地 `main`：

```sh
git switch main
git pull --ff-only
git branch -d fix/sr04-retry
```

尚未发布的个人工作也可以在本地完成检查后使用 `git merge --no-ff` 合并，并记录为本地合并。它不会自动产生 GitHub PR 或他人评审记录。

建议在 GitHub Rulesets 中为 `main` 配置 PR 和上述 CI 检查要求。模板和工作流文件只是规则定义；仓库是否启用强制保护，需要在远程设置中单独确认。

## 问题追踪与恢复

问题和功能建议使用仓库 Issue 模板，包含复现步骤、提交 ID 和环境。已有 Issue 时，在 PR 描述中用 `Closes #编号` 关联实际问题。

- 用 `git log --oneline --graph --all --decorate` 查看提交与分支关系。
- 用 `git show <commit>` 或 `git diff <old> <new>` 定位修改。
- 已共享提交需要撤销时，优先使用 `git revert <commit>`，通过新提交保留处理历史。
- `git restore --staged <file>` 取消暂存并保留工作区修改；`git restore <file>` 会丢弃文件尚未暂存的修改。

## 发布与记录

经过验证的功能版本可建立带注释标签，并在发布记录中写明提交、工具链、内核/模块匹配条件及验证范围。标签使用实际创建日期；产品版本号按实际发布决定。

工作流建设可以使用 `workflow-baseline-YYYYMMDD` 形式的标签，它只标识工程流程基线。原有源码来源及验证边界继续以 [NOTICE.md](NOTICE.md) 和 [验证记录](docs/validation.md) 为准。

`.gitignore` 排除构建目录、可执行文件、内核模块、日志和个人配置。源码、构建入口、部署说明、协议文档及必要的参考补丁保留在仓库中。

本项目八个功能模块已进行一次当前日期的集成复盘，实际分支、提交和检查示例见 [按功能模块集成复盘](docs/module-replay.md)。
