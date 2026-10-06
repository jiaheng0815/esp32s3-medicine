# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 构建与烧录

`pio` **不在 PATH 里**（这台机器的现状，不是配置错误），必须用完整路径：

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d "G:\ck\ck\esp32s3cam-st7735"
```

- 只用 `pio run` 编译验证：约 7~11 秒，不占串口、不动设备
- `pio run -t upload` 才烧录
- **改 `platformio.ini` 会触发全量重建，约 9.5 分钟**——排障时别随手改它
- 偶发链接失败 `libXXX.a: No such file or directory` 是构建竞态，**重跑即可**，别去改代码

## 架构

### 一条主线，两个分支

```
OV3660 --(RGB565)--> PSRAM 帧缓冲
                       ├─ 取景: buildView() → viewBuf → ST7735S 屏   (17FPS)
                       └─ 拍照: 切模式 → frame2jpg 软件编码 → base64 → HTTPS → 智谱 glm-4.6v

INMP441 --(I2S RX @16k)--> PSRAM WAV 缓冲 --(multipart)--> 智谱 glm-asr-2512
                                                            └─> medHandleUtterance() 纯本地判断
                                                                  └─> GLM-TTS --(I2S TX @24k)--> MAX98357A
```

**取景和拍照对同一份摄像头数据做了不同的处理，本项目绝大多数坑都出在这个不对称上。**

语音这条路上几个刻意的不对称（都是踩过才知道的，见下面的坑）：
- **I2S 分时切换**：录音和播报共用 BCLK/WS 两线，任何时刻只有一个方向在跑
- 录音的松手检测在**独立 task** 里（主循环只有 17Hz，而且拍照时干脆不跑）
- ASR 的转写文字在响应**顶层 `text`**，不是 `choices[0].message.content`

### 文件分工

- `main.cpp` — 屏幕、取景、按键状态机（三手势）、运行模式开关（`IS_TEST`）、中文上屏
- `vision.cpp` — 摄像头引脚 + 取景/拍照两套模式参数、软件 JPEG、智谱视觉 API
- `voice.cpp` — **语音链路**：I2S 录音(INMP441) → ASR 上传 → 本地判断 → TTS 播报(MAX98357A)，含电平条和静音检测
- `hwtest.cpp` — 麦克风/屏幕/功放的**基础测试**（屏幕上直接画声波），`HW_TEST` 开关触发
- `httpio.cpp` — 共享 HTTPS 传输层（循环写满 / 读响应头 / chunked 解码 / 取 JSON 字段），拍照和语音共用
- `med.cpp` — 药品记忆：NVS 读写、名字模糊匹配、**纯本地**的时间判断（不调 API）
- `webui.cpp` — 常驻网页：用药记录增删、文本输入（麦克风坏时的降级入口）、照片回看 `/photo`
- `netcfg.cpp` — WiFi 凭据(NVS) + AP 配网
- `secrets.h` — API Key，被 `.gitignore` 忽略

### 运行模式

`main.cpp` 顶部的 `IS_TEST`：`1` = 纯识图（关掉药品、网页、语音），`0` = 正常模式。两种模式都验证过编译。

### 硬件资源约束

| 资源 | 约束 |
|---|---|
| 内部 RAM | 紧张。TLS 握手要 40~50KB **连续**内存 |
| PSRAM | 8MB (R8N16)，拍照峰值约 2MB |
| NVS | **只有 20KB**，且 `putString` 单值上限 4000 字节 |
| 可用 GPIO | `3, 38, 39, 40, 41, 42, 45, 46`（**22-25 在 S3 上不存在**；3/45/46 是 strapping 脚）|
| 任务分配 | `loopTask` 在 core 1；`cam_task` + lwIP 在 core 0 |

## ⚠️ 踩过的坑（改代码前必读）

以下都是实测踩出来的，**光看代码看不出来**：

### 1. 字节序：`frame2jpg` 默认大端，不要加 bswap

`esp32-camera` 的 `to_jpg.cpp` 里写着 `static bool rgb565_big_endian = true` —— 它按大端读 RGB565；而本项目摄像头也是高字节在前（`main.cpp` 的 `CAM_MSB_FIRST=1`，屏幕颜色正常就是证据）。**两边本来就一致。**

曾误判成「取景翻了、JPEG 没翻」而加了 `bswap16`，把正确的数据搅成位错乱（大块品红/青/绿 + 彩色噪点，像热成像）。`vision.cpp` 的 `SHOT_SWAP_BYTES` **必须保持 0**。

### 2. 拍照路径需要手动 180° 旋转

摄像头输出相对正立是**上下左右全反**的。取景路径在 `buildView()` 里靠反向取采样点抵消了它，**但拍照路径没有** —— `frame2jpg` 是直接读 `fb->buf`。

症状：**屏幕看着正常，发给模型的图是倒的**，药盒上的字全倒着，模型读不出来。`SHOT_ROT180` 必须保持 `1`。

### 3. OV3660 的硬件 JPEG 完全不可用

`esp32-camera` 的 ov3660 驱动里 `sensor_fmt_jpeg[]` 写的是 YUV422/YUYV 配置（连驱动自己的注释都这么标），传感器压根没切进 JPEG 模式。实测 JPEG QVGA/VGA/SVGA **全部 8 秒超时零帧**，而 RGB565 连 VGA 都稳定。

所以拍照是**拍 RGB565 再用 `frame2jpg_cb()` 软件编码**。

### 4. JPEG 输出缓冲必须够大，否则静默腰斩

缓冲满时 `frame2jpg_cb` 中途返回 0、编码器中止，你拿到一段**残缺的 JPEG**，而 API 只回一句「图片输入格式/解析错误」——从错误信息完全看不出是缓冲不够。SVGA + 最高质量能到 530KB+，所以 `SHOT_JPG_CAP` 给到 1MB。

### 5. AWB 需要收敛时间

传感器是拍照前才 `init` 的，自动白平衡从零开始调整。立刻拍会得到未校正的色彩 —— **整体蒙一层黄绿**（Bayer 阵列里 G 占 50%，采样量是 R/B 的两倍，白平衡一没配平先溢出的就是它）+ 偏暗。`SHOT_WARMUP_FRAMES` 丢前几帧就是为这个。

**注意区分**：取景路径颜色正常，是因为它长期运行、AWB 早收敛了 —— **不是**两条路径的处理方式不同。

### 6. 中文在 C++ 里必须用双引号

`'次'` 用单引号会被当成 multi-character constant（UTF-8 三字节 `E6ACA1`），截断成一个字节。**编译只报 warning，运行时静默失效** —— 最阴的一类 bug。一律写 `"次"`。

### 7. `gdma_disconnect: no peripheral is connected` 是无害噪音

esp32-camera 在 ESP32-S3 上的已知问题：驱动用直接写寄存器的方式连 DMA，却在 deinit 时调 `gdma_disconnect()`。每次切换摄像头模式出现两条，属正常。

### 8. ASR 的转写文字在顶层 `text`，拿错了不报错只给空串

智谱 ASR 的响应 schema 是 `{id, created, request_id, model, text}`，而 `/chat/completions`
是 `{... choices[0].message.content}`。两个接口不一样。

直接复用 `httpExtractContent()` 会**永远拿到空串**，而且：HTTP 200、没有异常、没有警告 ——
症状就是「一切正常但结果是空的」，比报错难查得多。

`httpio.cpp` 里现在有两个入口：`httpExtractStringField(json, key)` 是通用的，
`httpExtractContent()` 只是 `httpExtractStringField(json, "content")` 的一行包装。

### 9. 功放的 SD 不能直接接 3V3 —— 录音时会自激

录音时 I2S 工作在 RX 模式，**BCLK(41)/WS(42) 是一直在翻转的**（主模式要产生时钟），
而这时 DIN(39) 不被驱动、处于悬空。MAX98357A 看到有时钟就当有数据，把悬空脚的噪声
放大出来 → 被 INMP441 录回去，形成自激。

所以功放使能必须走 **GPIO38**，由 `voice.cpp` 的 `ampOn()` 保证只有播报时才拉高。

**症状**：录音电平条一直显示有信号、回放能听到持续底噪或啸叫 —— 很容易误判成
"麦克风底噪大"或者"环境太吵"，然后去调静音阈值，方向全错。

### 10. 播放单声道时 MAX98357A 会把音量减半

MAX98357A 的 SD 拉高时工作在 `(L+R)/2` 模式。如果 I2S 只往左声道发数据、右声道恒为 0，
混音后输出电平掉 6dB。

**症状**：能响，但明显偏小 —— 很容易误判成"功放模块不行"或"5V 供电不足"。

`playPcm()` 的做法：TX 配 `I2S_CHANNEL_FMT_RIGHT_LEFT`，分块把 mono 采样复制成 L/R 交错。
（SDK 的 `I2S_CHANNEL_FMT_ALL_LEFT` 会打开硬件 `tx_mono`+`tx_chan_equal` 让硬件自己复制，
省掉这次拷贝，但那是 HAL 内部行为；用 RIGHT_LEFT + 手动展开是任何版本都成立的写法。）

### 11. 录音缓冲的 PCM 起点不能是 44 字节

ESP32-S3 的 GDMA 访问 PSRAM 要求 **16 字节对齐**（`SOC_GDMA_PSRAM_MIN_ALIGN`），
而 44 不是 16 的倍数。

`voice.cpp` 把 WAV 头做成 **64 字节**：标准 44 字节 + 一个 20 字节的 `JUNK` 对齐块
（`JUNK` 是 RIFF 里合法存在的填充块，标准解析器会跳过）。

⚠️ 这 20 字节**不能是"随便塞的垃圾"** —— 那样 `data` chunk 声明的位置和实际不符，
解析方（包括自己回放用的 `wavParse`）会算错 PCM 起点，表现成开头几声咔哒 +
末尾丢一截，看着像编码问题，完全指不到对齐上去。所以要用一个真的 chunk 把
`data` 老老实实推到 56 偏移。

### 12. 麦克风读数恒定（全 0 / 全 FF）**先查接线**，别急着换硬件

2026-10-06 实测踩过：第一次接线很乱，`HW_TEST` 读出来**恒定 `FFFF`**（全 1）。
把线重新插整齐之后，`rms` 立刻从 `1` 跳到 `32~438`、`peak` 到 3143 —— **一个零件都没换**。

三种"坏"的读数长得几乎一样，光看数值分不出来：

| 读数 | 可能是 |
|---|---|
| 全 `0x0000` | 数据线被拉低 / 没有驱动 |
| 全 `0xFFFF` / `0xFFFE` | 数据线被拉高 / 芯片没工作（高阻被上拉）|
| 乱跳但说话时不变大 | 时钟问题 / 麦克风本身坏 |

**接线混乱或接触不良的症状，和"麦克风烧了"完全一致。** 所以排查顺序固定：

1. **把每根线重插一遍**（插到底）—— 最快的验证，成本 2 分钟
2. `HW_TEST` 看原始数据（`全0 / 全FF / 其他` 三列 + 前 6 个采样的 hex）
3. 才怀疑模块本身

> ⚠️ **别照抄网上的声道配置。** 社区帖子里说「L/R 接 GND 要用 `ONLY_RIGHT`」，
> 本机实测**是 `ONLY_LEFT` 才有数据**（`ONLY_RIGHT` 下全 `FFFE`）。
> 这种结论只能两边都试一次才算数 —— `HW_TEST` 现在是固定的 `ONLY_LEFT`。

### 13. TFT 不是线程安全的 —— 语音 task 和主循环会抢 SPI

语音的进度提示（`Recording` / `Speaking...`）是**在语音 task 里**通过 `onVoiceUi`
回调画到屏幕上的，而 `setup()` / `loop()` 也在画。`Adafruit_SPITFT` 没有任何同步。

**症状极具迷惑性**：设备**不重启**、串口**没异常**、连 `[FPS]` 都不打了。
真实原因是语音 task 卡死在 SPI 写入里 → `voiceBusy()` 永远为真 → 主循环每帧
直接 `return`（连 FPS 统计都跳过）。**看着像死机，其实是两个任务把 SPI 状态搞乱了。**

修法：`main.cpp` 的 `tftLock()/tftUnlock()` 把**所有**屏幕操作串行化 ——
`showStatus` / `drawLevelBar` / `drawVoiceResult` / 取景的 `pushView`+`drawFps`。

- 锁**必须带超时**（3 秒）：拿不到就跳过这一帧。宁可少画一帧，不能把任务挂死
- 锁要在 `setup()` 里**显式创建**，不能靠懒初始化 —— 两个任务同时第一次调用会
  各自建一个 mutex，等于没锁

### 14. TTS 的 WAV 比想象中大得多 —— 512KB 缓冲会被撑爆

2026-10-06 实测：一句 6 秒的「语音测试成功，你好世界，hello world」，
GLM-TTS 返回的音频**撑满 512KB 还没完**（共 162 个 chunk）。

按「24kHz 单声道 = 48KB/s」估算，6 秒只有 288KB —— 实际大了一倍以上，
说明返回的是**立体声**（或更高采样率）。**别用理论值估缓冲大小，实测为准。**

`voice.cpp` 的 `TTS_BUF_BYTES` 现在是 **2MB**（够 11 秒的 44.1kHz 立体声）。

⚠️ **伴随的代码坑**：`readChunkedToBuffer()` 在缓冲满时会**空转** ——
`want = cap - out` 变成 0，`c.read(ptr, 0)` 恒返回 0，内层 `while (got < sz)`
永远出不来，只能等超时。症状同样是「设备不重启但彻底没反应」。
修法要**两层都加**：`if (want == 0) break;` + 内层循环里的超时检查。

### 15. 播放开头会"嘟嘟"一阵 —— DMA 缓冲是未初始化的内存

现象：喇叭先放一段像**摩斯电码**的嘟嘟/咔咔声，然后才是正常语音。

三个因素叠在一起：

1. `i2s_driver_install()` 之后，DMA 环形缓冲是**未初始化的随机内存**，
   而 I2S 一启动就立刻开始"播放"它 —— 24kHz 下随机数据听着就是一阵嘟嘟声
2. 环形缓冲 = `dma_buf_count × dma_buf_len` = 8 × 256 帧 ≈ **8KB**；
   只灌一小段静音（原来 20ms = 1.9KB）**盖不住**，剩下的垃圾照样漏出来
3. 功放要是先 `ampOn(true)`，这些垃圾就被直接放大了

**正确顺序**：先把整个环形缓冲灌满静音（灌 10KB > 8KB）→ **再**开功放。

三处都要这么写：`voice.cpp` 的 `playWavBuffer()` / `testTone()`、
`hwtest.cpp` 的 `testSpeaker()`。

## 调试工具

代码里内置了几个开关，排障时比猜快得多：

| 开关 | 位置 | 用途 |
|---|---|---|
| `SHOT_PREVIEW` | `vision.h` | 保留最近一张照片；**手机浏览器直接看发给模型的原图**。TEST 模式走临时网页，正常模式走常驻网页的 `/photo` |
| `BENCH_CAPTURE` | `main.cpp` | 开机依次试各种拍照模式组合，打印结果后停住 |
| `IS_TEST` | `main.cpp` | 退回纯识图模式（同时关掉语音、用药记忆、网页）|
| `HW_TEST` | `main.cpp` | **最基础的硬件自检**：屏幕闪色 + 中文、1kHz 测试音、**屏幕上实时画声波**。不用串口、不用按键 —— 首次接线先跑这个 |
| `MIC_SELFTEST` | `voice.h` | **完整音频链路自检**：测试音 + 按住 BOOT 录音 + ASCII 波形 + 喇叭回放（HW_TEST 通过后再用它）|
| `ASR_HOTWORDS_JSON` | `voice.cpp` | 热词表格式开关（JSON 数组 / 空格分隔）。服务端回 400 抱怨 hotwords 时切一下 |
| `CAM_ROT180` / `TFT_TAB_AUTO` | `main.cpp` | 画面方向 / 面板 tab |

**`SHOT_PREVIEW` 和 `MIC_SELFTEST` 是最有用的两个** —— 前面这些坑（字节序、图像倒置、JPEG 腰斩、麦克风没信号）全都是靠「亲眼看到图 / 亲耳听到声音」才定位的。**遇到颜色/方向/清晰度/声音问题，先打开它们看现象，别靠推理。**

另外每次拍照串口会打印**中心像素的原始字节**（如 `FF FF -> R=255 G=255 B=255`）。**平均值会骗人**（R/B 通道错位时若数值接近，平均值几乎不变），原始字节骗不了人。

## 当前状态

- ✅ 取景 17FPS、拍照识别、用药记忆、网页录入 —— 全部可用
- ✅ 语音链路（录音 → GLM-ASR → 本地判断 → GLM-TTS 播报）—— **代码完成、编译通过，等接线上机验证**
- 📋 原始设计稿 `C:\Users\heng\.claude\plans\shimmering-crafting-cake.md`
- 📋 本次实现计划 `C:\Users\heng\.claude\plans\glittery-swimming-bonbon.md`

**上机验证顺序**：

1. `voice.h` 的 `MIC_SELFTEST=1` 烧录 → 放 1kHz 测试音验功放 → 按住 BOOT 录音看 ASCII 波形 + 喇叭回放验麦克风
2. 验过之后改 `MIC_SELFTEST=0`，`IS_TEST` 也改成 0，进正常模式
3. 按住 BOOT 说「阿莫西林吃了」→ 看串口 ASR 转写、听播报、看屏幕上的中文结果

**语音部分的关键点**（改动前先看这些）：

- ASR 的转写文字在响应**顶层 `text` 字段**，**不是** `choices[0].message.content` —— 用 `httpExtractContent()` 会永远拿到空串而且不报错。要用 `httpExtractStringField(json, "text")`
- 用 `file` 分片而不是 `file_base64`，能省掉 427KB 的 base64 缓冲
- **录音必须独立 task**：主循环只有 ~17Hz，拍照时会阻塞 5~35 秒，根本采样不到松手动作
- **功放的 SD 必须走 GPIO38，不能直接接 3V3** —— 否则录音时自激（见「踩过的坑」第 9 条）
- **屏幕其实能显示中文**：`cnfont.h` 的 `cnDraw()` 有 7445 字点阵库。（`showStatus()` 注释里那句"字体只有 ASCII"只是它自己用内置字体，不代表屏幕不能画中文）
- 采样率不匹配：麦克风 16kHz、GLM-TTS 返回 24kHz，共用引脚时必须分时切换配置
- Arduino 下**用不了** `esp-sr`（AEC/降噪属 ESP-IDF 生态），所以坚持「按住说话」的对讲机模式 —— 天然没有回声问题，不需要 AEC
- 按键手势：短按拍照 / **按住 ≥1.2 秒录音** / **双击配网**。配网从「长按 3 秒」挪到双击是被迫的：1.2 秒录音阈值一定先触发，而且「一天两次早晚各一次」要说 4~8 秒，会把 3 秒阈值撞上
