# 社区投稿素材：封面与宣传图提示词

投稿需要 1 张封面 + 若干详情图。仓库里已经放了三张可以直接用的成品：

| 文件 | 用途 | 尺寸 |
| --- | --- | --- |
| `docs/submission/cover.png` | 封面（营地背景 + 标题 + 真机界面） | 1080×1080 |
| `docs/submission/promo-learn-fight-win.png` | 详情图一：学 → 战 → 胜 | 1080×1080 |
| `docs/submission/promo-features.png` | 详情图二：词表 / 设置 / 双词书 / 打卡 | 1080×1080 |

下面是可以替换成像素质感更好的 AI 图的提示词。**不要让模型画文字**（中文几乎必崩），标题和标注最后自己叠。

---

## 0. 统一风格锚（每条提示词都带上）

```
16-bit pixel art, crisp hard-edged pixels, limited palette: warm gold #E8B84B, deep pine green #14231A, cream #F6F1E3, ember orange #D97A2B, dusk purple #2A2350. Dithering only on gradients, no anti-aliasing, no blur, no 3D render look, no photorealism. Cozy camp-and-forest theme, small chibi adventurer with a sword.
```

中文版（用于支持中文提示词的模型）：

```
16-bit 像素艺术，硬边像素、无抗锯齿，限定配色：暖金 #E8B84B、深松绿 #14231A、米白 #F6F1E3、火橙 #D97A2B、暮紫 #2A2350；只在渐变处使用抖动。不要 3D、不要写实、不要模糊。营火与森林主题，迷你冒险者持剑。
```

负面提示（所有图通用）：

```
text, letters, watermark, logo, extra limbs, modern UI, smartphone, flat vector, 3D render, low contrast, muddy colors, oversaturated, blurry, jpeg artifacts
```

---

## 1. 封面（方图 1080×1080，最重要）

**方向 A：营地之夜（与 App 主界面同构，最稳）**

```
Square 1:1 game cover art, 16-bit pixel art. A cozy campsite at dusk in a stylized forest: a small tent on the left, a crackling campfire in the center, a chibi adventurer holding a sword and a scroll standing to the right, a treasure chest half hidden in the grass at the bottom right. Behind, distant blue-purple mountains and a warm orange sunset sky with a few stars. Composition: the upper third is open sky for the title, the middle third holds the campsite, the lower third is path and grass where a device screen will be composited. Rich rim light from the campfire on the character. Crisp pixels, no text.
```

**方向 B：词汇怪兽（突出“打 Boss 背单词”的梗）**

```
Square 1:1 pixel art cover. A giant moss-and-bark monster covered in glowing English letters — ABANDON, ABILITY, ABROAD — towers over a tiny chibi adventurer who holds up a glowing word card like a shield. Campfire sparks and floating letter particles in the air. Dark forest at night, warm gold light from the letters. Bold silhouette, strong value contrast, room at the top for a title. 16-bit palette, crisp pixels, no readable text other than the letter shapes.
```

**方向 C：真机在营地（产品感最强）**

```
Square 1:1 pixel art illustration with a photographic-free mockup: a small handheld game console with a square 480×480 screen held in a hand wearing a green sleeve, standing in front of the same cozy campsite at dusk. The screen shows an abstract pixel UI (gold button bar, green panels) — no readable text. Warm campfire glow on the device edge, forest bokeh in the background. 16-bit pixel art style for the scene, crisp device edges.
```

> 叠字：WORDQUEST 用 Press Start 2P（仓库里已有子集 `ui/fonts/PressStart2PSub.ttf`），副标题「单词冒险」用等线/DengXian，金色 #E8B84B，加 2px 深色描边。

---

## 2. 横版头图（1600×900，用于项目页顶部）

```
Wide 16:9 pixel art banner, 16-bit style. Panoramic dusk campsite: tent far left, campfire center-left, chibi adventurer center-right facing a large moss monster at the far right, mountains and a warm gradient sky behind. Leave the left 40% relatively calm and dark for a title block. Strong horizontal composition, deep pine green shadows, gold highlights, ember particles. Crisp pixels, no text.
```

---

## 3. 详情图：三张场景插画（1080×1080 或 1080×1350）

**3.1 学习（安静、信息感）**

```
Pixel art scene, 16-bit: a close-up of a wooden camp table with a weathered word card lying on it — the card has a decorative border and blank lines where text would go — a quill, an ink pot, a small lantern, and a folded map. Warm lantern light, deep green background, fireflies. Flat lay composition, crisp pixels, no readable text.
```

**3.2 战斗（动作、冲击）**

```
Pixel art battle scene, 16-bit: a chibi adventurer mid-slash with a bright golden arc of light, facing a huge moss-and-bark monster; impact sparks, floating debris, dark forest background, strong warm rim light. Dynamic diagonal composition, screen-shake energy. Crisp pixels, no text.
```

**3.3 胜利（奖励、呼吸感）**

```
Pixel art victory scene, 16-bit: the moss monster collapsed peacefully in the grass with closed eyes, a beam of golden light from the sky, the chibi adventurer raising a sword in silhouette, a round leaf-and-star medal floating in the air, moon and the same dusk sky. Calm, rewarding mood. Crisp pixels, no text.
```

---

## 4. 详情图：功能图解（1080×1080）

自己截图 + 排版更准（仓库 `docs/submission/promo-features.png` 就是这个思路）。若要用 AI 生成背景：

```
Pixel art panel background, 16-bit: a dark pine-green wooden notice board with pin holes and rope corners, empty in the middle, warm gold trim, hanging on a forest wall with a lantern at the top. Leave the center 70% empty for UI screenshots. Crisp pixels, no text.
```

---

## 5. 尺寸与导出

| 用途 | 尺寸 | 备注 |
| --- | --- | --- |
| 方图封面 | 1080×1080 | 平台最常见的封面比例 |
| 横版头图 | 1600×900 | 项目页顶部 |
| 详情长图 | 1080×1350 或 1080×1080 | 每张一个卖点 |
| 真机截图 | 480×480 | 直接从设备导出，**不要放大到 1080 再用**，会糊 |

导出：PNG（截图、UI 排版） / JPEG q92（纯插画）。文字与 UI 一律最后叠加，别交给模型。
