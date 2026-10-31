# 社区投稿：文件清单与上传步骤

> 2026-11-01 格式修复：当前社区要求 `esp-iris-system-update/0.2`。旧文件 `firmware/wordquest-system-update.irisfw` 不再用于社区上传。新版独立构建目标为 `firmware/wordquest-v0.2-system-update.irisfw`；构建、官方校验与复现证据归档于 `release-v02/`。本机 0.1 设备未升级。
>
> 官方依据：[0.2 迁移说明](https://github.com/esp-mosaico/esp-mosaico-utils/blob/be151ae047014e79a9c7ae0092dd203844499003/mosaico-tools/docs/migration-0.2.md)、[应用集成契约](https://github.com/esp-mosaico/esp-mosaico-utils/blob/be151ae047014e79a9c7ae0092dd203844499003/mosaico-tools/docs/application-integration.md)。二者明确要求重新构建，不接受仅修改旧 manifest 的版本字符串。

## 当前上传附件（已重新构建并通过官方本地校验）

- 文件：`firmware/wordquest-v0.2-system-update.irisfw`，2,269,436 字节。
- SHA-256：`25d1dac8ddf2c230ffcbbee3400eb21022086b7dd309318c0883e12db8b316c1`。
- schema：`esp-iris-system-update/0.2`；release：`1.0.0`；最低 Vibe Mode：`0.2.0`。
- 官方包检查器及 Ideas 产品校验通过；UI 镜像与旧包逐字节一致。
- 尚未进行 0.2 真机测试或生产社区上传验证。详见 [发布验证记录](release-v02/README.md)。

## 1. 旧版固件记录（0.1 设备基线，不能上传当前社区）

| 项目 | 值 |
| --- | --- |
| 文件 | `firmware/wordquest-system-update.irisfw` |
| 大小 | 2,261,396 字节（约 2.16 MiB） |
| SHA-256 | `07204372593cd41bf3233f1735446cab6b71dea6ba85719eb58a44eccdf65948` |
| 包内 schema | `esp-iris-system-update/v1`（无签名） |
| release | `1.0.0`（与 `build/project_description.json` 的 `project_version` 一致） |
| `minimum_recovery_version` | `0.1`（旧版契约声明，不能据此推断兼容 Vibe Mode 0.2） |
| 目标 | ESP32-S31，`chip_id = 32`，16 MiB flash，layout `mosaico-retained-recovery-2m-v1` |

包内三个组件（目标地址 / 大小 / 校验均与本地构建产物一致）：

| 组件 | 目标地址 | 大小 | 说明 |
| --- | --- | --- | --- |
| `partition_table.bin` | `0x8000` | 4,096 | 分区表，hash 与 `target_layout_sha256` 相同 |
| `ota_0.bin` | `0x210000` | 1,846,080 | 应用镜像（`build/wordquest.bin`） |
| `ui_apps.bin` | `0xE80000` | 1,521,552 | GSP 场景与字体资源 |

不含 `bootloader` / `recovery` / 平台身份镜像，也不动保留恢复前缀的分区；这次包和上一次在真机上通过 Vibe Mode 完成无线更新的是同一套结构。

> 更正（2026-11-01）：上表是旧版包，社区已拒绝 `esp-iris-system-update/v1`。官方 0.2 工具要求 `esp-iris-system-update/0.2`、产品契约 `esp-mosaico/v2`、ABI 2，以及匹配的 `vibe_mode` / `main_app` 分区。0.2 是破坏性升级，不能因为最低版本写了 0.1 就认定向下兼容，也不能仅改 ZIP 的 schema。此旧包保留作本机 0.1 基线，不再作为社区上传附件。新版包在独立构建副本中按官方 0.2 契约重建；本机旧设备未升级。

复检命令（本地，不连设备）：

```bash
python - <<'PY'
import zipfile, json, hashlib, os
b='firmware/wordquest-system-update.irisfw'
z=zipfile.ZipFile(b); m=json.loads(z.read('manifest.json'))
print('size', os.path.getsize(b), 'sha256', hashlib.sha256(open(b,'rb').read()).hexdigest())
print('schema', m['schema'], 'release', m['release'], 'min_recovery', m['minimum_recovery_version'])
for c in m['components']:
    print(c['kind'], c['file'], c['size'], hex(c['target_offset']),
          hashlib.sha256(z.read(c['file'])).hexdigest() == c['sha256'])
PY
```

## 2. 元数据与介绍

| 文件 | 用途 |
| --- | --- |
| `projects/wordquest/mosaico-ideas.json` | 上传元数据（标题、分类、标签、许可、封面、changelog） |
| `projects/wordquest/README.md` | 详情页富文本（含截图、玩法、操作、限制、数据来源） |
| `projects/wordquest/docs/community-promo-prompts.md` | 封面/宣传图的 AI 提示词 |
| `projects/wordquest/docs/submission/*.png` | 截图与三张成品宣传图 |

标题：**WordQuest 单词冒险 · 在像素营地里打 Boss 背单词**
分类：`display`；标签：背单词、英语学习、像素风、CET-4、CET-6、小游戏、离线、ESP-GSP

### 简介（可直接粘贴，三种长度）

- **一句话（约 30 字）**：读卡片、做选择，答对一刀砍向 Boss；砍翻它，今天的单词就背完了。
- **短简介（约 60 字）**：WordQuest 把背单词做成了像素营地回合制小游戏：看卡片、四选一、答对攻击 Boss。内置 CET-4 / CET-6 共 6,084 个词条，带遗忘曲线复习，全程离线，一块 480×480 的触摸屏就够。
- **中简介（约 120 字）**：把背单词做成能打的小游戏 —— 每个新词先出一张学习卡（单词、音标、释义、例句），合上卡片马上四选一，答对打 Boss 一下，答错 Boss 反击并把题排回队尾。两本词书（CET-4 3739 / CET-6 2345）各自独立进度，带 1/3/7 天遗忘曲线复习、错题本、生词本、词表浏览与跳过生僻词、周月报表和芯片风 BGM。全部离线，进度写进 NVS 断电不丢。

## 3. 上传

在社区网页选择新文件 **`wordquest-v0.2-system-update.irisfw`** 作为固件附件。标题、简介、截图可继续使用本目录现有素材。

原开发工作区仍使用 0.1 工具，不要在这里直接执行旧的 `project upload` 默认构建流程，否则会再次生成 `/v1` 包。需要使用 CLI 时，应使用 `release-v02/` 指定的官方 0.2 工具副本，并通过 `--bundle` 显式选择新版附件。

此次仅准备本地文件，没有登录、上传或提交审核；官方本地校验通过不等同于社区服务端已经接受。

## 4. 提交前已知项

- 词条数据来自 `kajweb/dict`（未声明许可证），README 里已如实写明；若主办方要求明确的商用授权，需要换词表。
- 截图是模拟器与真机各一部分（渲染器与资源一致），真机主页图另见 `.agents/analysis/wordquest-rounded/device-v22-final.png`。
- 没有单词发音，README 的「已知限制」里写明了原因。
