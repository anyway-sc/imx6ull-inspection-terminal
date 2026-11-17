# 参考来源与贡献说明

本仓库是基于参考实现的 i.MX6ULL 学习与集成项目，包含项目适配源码、参考代码修改版和整理文档。

| 来源 | 使用关系 |
| --- | --- |
| [DescartesMessi/Intelligent-Vehicle-System-Based-on-i.MX6ULL](https://github.com/DescartesMessi/Intelligent-Vehicle-System-Based-on-i.MX6ULL) | 智能车载系统学习参考；当前板端 C 服务与 Windows Qt 的集成方式及验证结论以本仓库为准 |
| 正点原子 ALPHA V2.8 资料，`22_spi/icm20608.c` / `icm20608reg.h`，原作者左忠凯 | ICM20608 驱动参考来源；保留文件内作者、版权和模块许可声明，适配了旧片选属性和用户态读取接口 |
| Linux 4.1.15 配套 BSP 与 Qt | 构建和运行依赖，分别遵循其自身许可；BSP、Qt SDK 和开发板资料整包需自行获取 |

已标注 `SPDX-License-Identifier: GPL-2.0` 或 `MODULE_LICENSE("GPL")` 的文件保留原声明。本仓库不通过统一的 MIT 等许可覆盖第三方代码；未注明独立许可的文件不作额外授权承诺。

当前项目工作包括修改适配、应用集成、交叉构建、部署与实机排障，部分参考源码和检查由助手辅助准备。实机操作成功、本人对设计的理解和独立实现能力分别记录，不把全部驱动、协议与 Qt 代码表述为个人独立原创。
