# SR501 最终 GPIO1 设备树方案

`gpio1-sr501.patch` 针对配套 BSP 的 `arch/arm/boot/dts/imx6ull-14x14-evk.dts`：

1. 将原来的 `dht11`、`ds18b20` 节点设为 `disabled`，释放 GPIO1_IO01。
2. 在默认 `hoggrp-1` 中加入 `MX6UL_PAD_GPIO1_IO01__GPIO1_IO01`，供 HC-SR501 OUT 输入使用。

这是从原厂文件到项目方案的参考差异，只保留最终 GPIO1 方案。完整 BSP 与板端 DTB 需自行准备，不能将补丁直接当作可加载 DTB。

在匹配 BSP 根目录先检查上下文：

```sh
git apply --check /path/to/imx6ull-vehicle-terminal/device-tree/gpio1-sr501.patch
```

根据实际顶层 DTS 的 include 关系确认构建目标，然后应用补丁并编译对应 DTB。应核对运行中的设备树属性及 pinctrl；仅在源码里改成 `disabled` 不代表板端已经加载了新文件。项目已验证的最终 DTB 身份见 [验证记录](../docs/validation.md)。
