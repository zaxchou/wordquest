# WordQuest 社区 0.2 发布包

社区上传请选择根工作区 `firmware/wordquest-v0.2-system-update.irisfw`。

| 项目 | 值 |
| --- | --- |
| 文件大小 | 2,269,436 字节 |
| SHA-256 | `25d1dac8ddf2c230ffcbbee3400eb21022086b7dd309318c0883e12db8b316c1` |
| schema | `esp-iris-system-update/0.2` |
| release | `1.0.0` |
| 最低 Vibe Mode | `0.2.0` |
| 契约 | `esp-mosaico/v2` / `mosaico-retained-test-2m-v2` / ABI 2 |
| utils revision | `be151ae047014e79a9c7ae0092dd203844499003` |
| ESP-IDF revision | `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` |

独立构建源：`C:/Users/zeroz/esp/work/wordquest-v02-release`。原 0.1 开发工作区和设备保持不变。迁移遵循官方 [0.2 说明](https://github.com/esp-mosaico/esp-mosaico-utils/blob/be151ae047014e79a9c7ae0092dd203844499003/mosaico-tools/docs/migration-0.2.md)，重新编译应用，没有仅改旧 ZIP 的 schema。

## 校验结果

- 官方 bundle inspect 和 Ideas `validate_firmware()` 均通过。同一校验器拒绝旧 `/v1` 包，原因是 unsupported schema。
- 三个组件长度和 SHA-256 核对通过：分区表 4,096 字节；应用 1,858,576 字节（写入 `main_app`）；UI 1,521,552 字节。
- 包内没有 Bootloader、Vibe Mode 固件或 NVS 镜像。
- 有效配置符合官方 v2 契约、ABI 2、双 CDC、QIO，应用不含 OTA writer。
- 70 个 main/ui/pc 源文件与原权威副本哈希一致，UI 镜像与旧包逐字节一致。
- 模型/进度 5 组、800 个种子会话通过；共享 C headless 模拟器启动通过。
- 完整 low-noise build 成功，552.5 秒，runner 报告 2 warnings：已有未使用函数，以及 IDF 将应用与较小的 Vibe Mode/test 分区比较的尺寸警告；应用实际写入容量充足的 main_app，官方产品校验通过。其他 CMake/GSP 提示见 evidence/warnings.json。

**验证范围：本地构建与官方校验通过；尚未在 Vibe Mode 0.2 设备上实测，也没有执行社区上传。** 旧 0.1 设备迁移到 0.2 基础固件属于单独操作，本应用包不负责升级基础固件。

## 证据

本目录保留迁移补丁、源码快照、依赖 lock、有效 sdkconfig、bin/ELF/map、构建日志、复现脚本。主要结果见 `evidence/validation.json`、`evidence/schema-comparison.json`、`evidence/warnings.json`。

主线程独立复核脚本：工作区 `.agents/analysis/verify_wordquest_v02_release.py`。使用 `C:/es/.espressif/python_env/idf6.2_py3.12_env/Scripts/python.exe -I` 运行，脚本读取官方 0.2 校验器，不连接设备。
