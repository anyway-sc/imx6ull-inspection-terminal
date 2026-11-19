# 按功能模块集成复盘

日期：2026-11-19。来源基线：`fd470565e244a84c97470a9e7b2b949e814e13cc`。

本次把当前已经完成的功能在独立工作树中重新按模块接入，实际执行了建立分支、导入代码、分批提交、运行检查和合并。提交使用本次操作日期，原始历史及来源声明保留。此前各功能的原始开发时间和 Git 分支不能由本复盘推断。

## 集成方式

集成主线为 `integration/modules-20261119`。先以 `chore(replay): 建立按模块重新集成的起点` 暂时移出隔离工作树中的业务模块，再按依赖逐项接入。原项目主分支在八个模块完成前保持现有完整版本。

每个功能分支基于上一阶段的集成结果建立，包含一次模块代码提交和一次验证记录提交，之后使用 `--no-ff` 合并。八个功能分支均保留可查看的引用。

## 模块与实际提交

| 模块与阶段记录 | 功能分支 | 代码提交 | 实际检查 | 合并提交 |
| --- | --- | --- | --- | --- |
| [TCP 通信基础](module-replay/01-tcp-base.md) | `feat/replay-tcp-base` | `6b993b4a` | ARM 静态 1 文件 | `9ca0f06b` |
| [AP3216C 环境计数采集](module-replay/02-ap3216c.md) | `feat/replay-ap3216c` | `d94c6cfb` | ARM 静态 2 文件 | `058596ab` |
| [ICM20608 六轴采集与驱动适配](module-replay/03-icm20608.md) | `feat/replay-icm20608` | `c1bf9f91` | ARM 静态 3 文件 | `3e6071bd` |
| [DHT11 温湿度采集](module-replay/04-dht11.md) | `feat/replay-dht11` | `06f038a4` | ARM 静态 4 文件；CTest 1/1 | `b3ac8342` |
| [相机预览与 Qt 交互](module-replay/05-camera-preview.md) | `feat/replay-camera-preview` | `9f2957e0` | ARM 静态 5 文件；CTest 5/5 | `796929b1` |
| [SR04 距离与画面联合监测](module-replay/06-sr04-monitor.md) | `feat/replay-sr04-monitor` | `0da417f2` | ARM 静态 6 文件；CTest 8/8 | `94b30eae` |
| [SR501 活动触发画面](module-replay/07-sr501-sentinel.md) | `feat/replay-sr501-sentinel` | `614e8c36` | ARM 静态 7 文件；CTest 10/10 | `d70ef559` |
| [服务启动与部署集成](module-replay/08-service-deployment.md) | `feat/replay-service-deployment` | `632dd67e` | ARM 静态 7 文件；CTest 10/10；启动 12 + SysV 33 | `5cb7eb75` |

完整引用、各提交修改文件及检查摘要保存在 [manifest.json](module-replay/manifest.json)。

## 依赖与验证范围

- TCP 阶段建立 PING、按行接收、发送完整性和请求处理基础；后续传感器分支各自加入协议分发和模块文件。
- DHT11 阶段首先加入驱动帧校验的主机检查；相机阶段统一接入已有 Qt 窗口、全部页面及 DHT11 / ICM / 预览 / 切页四项 Qt 检查。
- SR04 和 SR501 阶段接入对应服务、驱动或设备树，并逐步加入专项 Qt 与文本解析检查。Qt 页面集中在同一个窗口类中，未将其伪装为此前独立开发的页面历史。
- 最后恢复完整构建入口、CI、启动脚本和原有文档，执行全部 10 项 CTest、12 项启动脚本模拟和 33 项 SysV 隔离检查。

ARM 检查使用 `arm-linux-gnueabihf` 目标、已有 sysroot 和 Clang core/unix 分析，仅验证用户态 C 代码的语法和静态规则；没有进行 ARM 交叉链接。内核模块未重新构建，本次未新增实机验证。Qt 检查使用本机模拟服务。

在写入本汇总之前，基线中的 **72 个已跟踪文件全部与原完成版逐字节一致**，通过 Git 差异核对。本次最终新增内容是模块分支历史、阶段记录、机器可读索引及文档入口。

## 查看一次完整分支操作

各阶段记录包含实际执行的 `switch`、`commit` 和 `merge --no-ff` 命令。也可以直接查看历史：

```sh
git log --graph --oneline --all --decorate --date-order
git show --stat feat/replay-dht11~1
git log --oneline feat/replay-camera-preview -3
git show --no-patch module-replay-20261119
```

`feat/replay-dht11~1` 指向该分支的模块代码提交，分支末尾是随后保存的验证记录。最终集成结果通过独立合并节点进入 `main`，带注释标签 `module-replay-20261119` 标记这次复盘结果。

## 远程状态

新增功能分支、合并结果和标签尚未推送；本次没有创建远程 PR、执行 GitHub Actions 或新增他人评审。发布前需先核对远程历史，再推送实际引用：

```sh
git fetch origin
git log --graph --oneline --all --decorate
git push --atomic origin main integration/modules-20261119 'refs/heads/feat/replay-*:refs/heads/feat/replay-*' refs/tags/module-replay-20261119 refs/tags/workflow-baseline-20261119
```

后续新的功能和修复按 [开发规范](../CONTRIBUTING.md) 从主线建立正常的工作分支，并使用 PR 与相应验证记录。

## 项目介绍口径

> 我针对这个个人项目做过一次按功能模块的集成复盘：先建立 TCP 基础，再分支接入传感器、相机、距离监测、活动触发画面和启动部署。每个分支都有实际代码提交、对应检查和合并记录。最后核对源码与原完成版一致，并用标签保留集成基线。这个过程是现在完成的集成练习，原始参考来源和此前开发记录保留原样。
