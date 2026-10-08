<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 多多随身固件（pocket v1）

这个分支把 FoloToy AI Passport 做成「多多随身」手机 App 的按键说话配件。按住 OK 说话：
Passport 通过加密的 BLE 链路发送 Opus 音频，手机把每次按键作为一条语音上传，最新的回答显示在
Passport 屏幕上。

## 许可证

| 文件 | 许可证 |
| --- | --- |
| 来自上游 `folotoy/ai-passport` 的文件（包括在这里修改过的） | MIT（[`LICENSE`](../../LICENSE)） |
| 本分支新增的文件（`main/pocket_*`、`tools/pocket/`、`tests/test_pocket_*`、`tests/pocket_check.h`、`assets/fonts/pocket_cjk_charset.txt`、`assets/images/avatar/`、`docs/pocket/` 中的 Markdown、各个 `sdkconfig.*.defaults` 叠加配置、`components/bsp/Kconfig`、测试桩 `tests/audio_stubs/driver/i2s_common.h`、`tests/bsp_stubs/driver/gpio.h`、`tests/bsp_stubs/esp_attr.h`、`tests/bsp_stubs/esp_sleep.h`） | FSL-1.1-Apache-2.0（[`LICENSE-FSL-1.1-Apache-2.0`](../../LICENSE-FSL-1.1-Apache-2.0)），每个源文件都带 SPDX 头 |
| `assets/fonts/pocket_cjk_20.c` 和 `pocket_digits_44.c` 中的 Noto Sans SC 字形数据 | SIL Open Font License 1.1（[`assets/fonts/LICENSE-OFL-1.1.txt`](../../assets/fonts/LICENSE-OFL-1.1.txt)） |

| 多多品牌素材：`docs/pocket/mockups/` 中的界面效果图、多多 / DuoDuo / OpenDuo 名称（商标） | 不提供开放许可，openduo 保留所有权利。分支项目必须替换。 |

FoloToy 的名称和标识归 FoloToy 所有。固件镜像包含的第三方组件及其许可证见 [`NOTICE`](../../NOTICE)。

## 按键与界面

| 输入 | 动作 |
| --- | --- |
| 按住 OK ≥ 300 ms | 说话，松开即发送；更短的轻按被忽略 |
| 手机未连接时按 OK | 不录音，屏幕提示手机未连接 |
| UP / DOWN | 当前回答翻一页；翻到顶（UP）或到底（DOWN）时切到更早或更新的已存回答；关闭失败提示 |
| 长按 UP | 打开或关闭设置（预录音、亮度、自动息屏、自动休眠、新回答亮屏、新回答提示音、重新配对）；一屏显示五行，列表可滚动 |
| 息屏时按 OK | 点亮屏幕并立即开始录音，不需要先按一下唤醒 |
| 息屏时按 UP / DOWN | 只点亮屏幕，这一次按键不做别的 |
| 配对码界面按 OK | 确认 LE Secure Connections 数字比较；DOWN 拒绝 |

全部 20 个界面的简体中文效果图：[`mockups/overview.png`](mockups/overview.png)，每个界面一张 PNG 在
[`mockups/`](mockups/)；英文版在 [`mockups/en/`](mockups/en/)（[`总览`](mockups/en/overview.png)）。效果图由 `tools/pocket/render_mockups.py` 在电脑上渲染，文案
（`main/pocket_strings.h`）、字体和头像图片与固件相同。固件界面在 `main/pocket_ui.c`；上游演示
菜单和页面不再链接。

## BLE 链路

| 项 | 值 |
| --- | --- |
| 服务 | `8bd90001-86c5-454b-b65b-3f16cffb662f`（放在主广播包里） |
| TX，设备 → 手机 | `8bd90002-86c5-454b-b65b-3f16cffb662f`，notify，要求加密且认证 |
| RX，手机 → 设备 | `8bd90003-86c5-454b-b65b-3f16cffb662f`，write without response，要求加密且认证 |
| 名称（扫描响应） | `DuoDuo Pocket XXXX`（蓝牙 MAC 末两字节） |
| 安全 | 仅 LE Secure Connections，MITM，绑定存在 NVS，数字比较用 OK 确认 |
| 广播 | 没有手机连着时一直广播：开机或断开后 30 秒内 20 ms 一次，之后 1022.5 ms 一次。启动失败时过 `ADV_RETRY_MS`（1.1 s）重试。NimBLE 还没上报就断掉的连接，会以“连接失败”事件到达，此时连接槽还没释放；广播改由 host 队列在槽释放后重新开始 |
| 配对模式 | 没有绑定时接受；已绑定的手机重新配对时接受；设置里「重新配对」会删除绑定 |
| PDU | `[type u8][flags u8][len u16 LE][payload]`，`flags bit0` 表示后面还有分片 |
| 协议版本 | 1.2（1.1 新增 `WORK`，1.2 在 `APP_STATE` 中加入语言） |

消息（整数为小端序；完整规范，包括手机端的行为，见 pocket-ios 仓库的 `docs/ble-protocol.md`）：

| 类型 | 名称 | 方向 | 负载 |
| --- | --- | --- | --- |
| 0x01 | `INFO` | 设备 → 手机 | proto_major u8、proto_minor u8、fw_len u8、固件版本字节、电量 u8、充电 u8、预录音 u8 |
| 0x02 | `PRESS_START` | 设备 → 手机 | press_id u16 |
| 0x03 | `AUDIO` | 设备 → 手机 | press_id u16、seq u16、一个 Opus 包 |
| 0x04 | `PRESS_END` | 设备 → 手机 | press_id u16、包数 u16 |
| 0x05 | `STATUS` | 设备 → 手机 | 电量 u8、充电 u8 |
| 0x06 | `KEEPALIVE` | 设备 → 手机 | press_id u16 |
| 0x81 | `RESULT` | 手机 → 设备 | press_id u16、结果码 u8（0 已转写，1 没听到，2 转写失败，3 发送失败，4 没连上服务器）、UTF-8 转写文本 |
| 0x82 | `REPLY` | 手机 → 设备 | reply_id u32、final u8、UTF-8 文本（替换该 id 当前显示的文本） |
| 0x83 | `REPLY_DONE` | 手机 → 设备 | reply_id u32（0 表示手机不再等待） |
| 0x84 | `APP_STATE` | 手机 → 设备 | 状态 u8（0 正常，1 服务器不可达，2 协议主版本不一致），之后是语言 u8（0 简体中文，1 英文；1.2） |
| 0x85 | `WORK` | 手机 → 设备 | phase u8、可选的 UTF-8 标签 |

手机 App 也必须遵守的链路规则：

1. 同一条消息的分片是连续的同类型 PDU；中途出现别的类型或格式错误的 PDU，会丢弃未拼完的消息。
2. 手机 → 设备的消息拼装后最大 4096 字节。超长部分在 UTF-8 字符边界截断，屏幕注明完整内容在手机上。
3. `INFO`：`proto_major u8, proto_minor u8, fw_len u8, fw 字节, battery u8, charging u8,
   preroll u8`。固件版本字符串带长度前缀。
4. 电量和充电状态用 `0xFF` 表示未知。这块板子没有充电状态信号，所以 `charging` 恒为 `0xFF`。
5. `AUDIO.seq` 每次按键从 0 开始，每产生一个包就加一，链路拒收的包也占号，手机能看到缺口。
   `PRESS_END.count` 等于用掉的序号数。
6. 打开预录音时，缓存里的包紧跟 `PRESS_START` 发送，序号 0..n-1。
7. 不是最近一次确认按键的 `RESULT` 会被忽略。未知的结果码按「发送失败」显示。
8. 从松开到收到 `REPLY_DONE`、失败的 `RESULT`、下一次确认的按键或断链为止，每 5 秒发一次
   `KEEPALIVE`。不设时间上限。
9. `press_id` 每次开机从随机值开始。
10. `REPLY_DONE` 的 `reply_id` 为 0：手机不再等回答。停止 `KEEPALIVE`，发送中或转写界面回到首页
    （有回答时回到最新回答）。
11. `APP_STATE` 2：协议主版本不一致。设备显示版本不匹配界面，不录音、关麦，直到收到 `APP_STATE`
    0/1 或断链。
12. `APP_STATE` 语言（1.2）：设备立即用手机的界面语言显示自己的文案，重绘当前界面，并保存在 NVS
    （`pocket/lang`；默认简体中文）。缺少这个字节（1.0/1.1 的手机）或取值未知时保持当前语言。来自大脑的
    文本（转写、回答、`WORK` 标签）按原样显示，不翻译。

只有手机在当前连接上写入 TX 的 CCCD 之后，链路才算就绪并发送 `INFO`。TX 的订阅状态不随绑定保存：如果已绑定的手机
重连时恢复保存的 CCCD，加密一建立就会算作已订阅，而此时 iOS 还没有重新发现服务、重新订阅，这段时间发出的通知会被
CoreBluetooth 丢弃。

链路就绪期间每收到一条 `APP_STATE`，设备也会回发一次 `INFO`。手机 App 重新启动（重装，或被系统回收后状态恢复）时，
BLE 连接可能一直保持，新的 App 进程会重新订阅，但 CCCD 的值没有变化，设备收不到任何事件。手机在链路就绪后会发送
`APP_STATE`，设备据此补发 `INFO`。

### 工作状态（协议 1.1）

`WORK`（0x85，手机 → 设备）：`phase u8`（0 空闲，1 已收到，2 思考，3 调工具），后接可选的 UTF-8
标签。

| 情况 | 设备行为 |
| --- | --- |
| 等待中的按键收到 `RESULT` 0 | 工作界面：转写文字，阶段标签和三个轮流亮起的圆点。收到 `WORK` 之前按「已收到」显示。 |
| `WORK` 1 / 2 / 3 | 标签分别为 收到了 / 在想 / 在查，圆点动画运行。 |
| `WORK` 0 | 圆点停止，保留上一个标签；继续等待并发 `KEEPALIVE`，直到 `REPLY_DONE`。 |
| `WORK` 阶段值大于 3 | 按「思考」显示（以后的次版本）。 |
| 不在等待中，或屏幕正显示回答时收到 `WORK` | 只记录或忽略，界面不变。 |
| 等待结束后才到的 `RESULT` 0 | 忽略（没有等待就不显示工作界面）。 |
| 等待结束 | `REPLY_DONE`（任意 id）、失败的 `RESULT`、下一次确认的按键、断链、`APP_STATE` 2。 |

本固件解析但不显示标签（v1 为空）。动画每 `POCKET_WORK_ANIM_STEP_MS`（400 ms）点亮下一个圆点，
由一个 LVGL 定时器驱动；不在工作界面或阶段为空闲时定时器暂停。

## 息屏与待机

没有按键、也没有新内容，过了息屏时间屏幕就熄灭；BLE 保持连接，芯片自动进入 light sleep。

| 情况 | 屏幕 |
| --- | --- |
| 息屏时间内没有按键也没有新内容（设置里可选 15 秒、30 秒（默认）、60 秒、从不） | 熄灭：背光关闭，面板进入睡眠（显存保留） |
| 正在说话；多多正在回答：发送中、`RESULT` 0、收到了 / 在想 / 在查、`REPLY` 还在流式输出（final 0） | 常亮，不倒计时 |
| 回答安静下来：`REPLY_DONE`（任意 id）、`WORK` 空闲且没有正在输出的文字、`RESULT` 失败、下一次按键、断开连接 | 开始倒计时 |
| 显示配对码；设置菜单打开 | 常亮，不倒计时 |
| 接着 USB 主机（收到 USB 帧起始包） | 常亮，不倒计时；息屏时检测到会点亮屏幕。拔掉后开始倒计时。充电器不发帧起始包，检测不到 |
| 亮屏时有按键、新的 `REPLY` 或其他新内容 | 倒计时重新开始 |
| 息屏时来了新回答（某个 `reply_id` 的第一条 `REPLY`，且尚未存储），并且“新回答亮屏”打开 | 点亮屏幕并重新倒计时 |
| 息屏时连接断开、配对请求、`RESULT` 失败、`APP_STATE` 1 或 2 | 点亮屏幕并重新倒计时 |
| 息屏时同一回答的后续 `REPLY` 文字，或已存储的回答再次发来 | 保持熄灭（文字照常更新） |
| 息屏时连接恢复、电量变化、收到 `WORK` | 保持熄灭 |

设置保存在 NVS（`pocket/scr_off`，是 `POCKET_SCREEN_OFF_LEVELS_S` 的下标）。没有配对记录、正在广播的
等待配对界面也会熄灭，收到配对请求时点亮。

### 电源管理

| 项 | 设置 | 依据 |
| --- | --- | --- |
| CPU 频率 | 持有锁时 160 MHz，否则 40 MHz；没有锁时自动 light sleep（`main/pocket_power.c`） | 160 MHz 是固件测量时的频率；40 MHz 是晶振频率，不用 PLL 时的最低频率 |
| 锁 `pocket_screen`（`ESP_PM_CPU_FREQ_MAX`） | 亮屏期间持有 | 背光 LEDC 和 LCD SPI 在 light sleep 中停止；LVGL 速度和以前一样 |
| 锁 `pocket_audio`（`ESP_PM_CPU_FREQ_MAX`） | codec 唤醒期间持有 | I2S DMA 在 light sleep 中停止；编码耗时是在 160 MHz 下测的 |
| BLE | 控制器 modem sleep，低功耗时钟用主晶振，light sleep 时晶振保持供电 | 板子没有 32 kHz 晶振（GPIO0/GPIO1 是按键分压和 LCD CS）；内部 136 kHz RC 达不到连接所需的 500 ppm |
| 按键唤醒 | GPIO0（按键分压节点）作为数字输入，低电平唤醒芯片；没有按键按下时 5 ms 的按键轮询停止 | 每个按键都把节点拉到 595 mV 以下，低于 ESP32-C3 的 VIL 上限 825 mV。改用定时唤醒的话，每个轮询周期都要唤醒芯片，还会多出最多一个周期的延迟 |
| light sleep 时的引脚 | ESP-IDF 把引脚隔离（`ESP_SLEEP_GPIO_RESET_WORKAROUND`）；背光和 LCD CS 保持原来的驱动电平 | 背光使能或片选悬空时状态不确定 |
| light sleep 时 CPU 断电 | 关闭（`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n`） | 能省约 100 uA，但要占 1.68 KB 内存。当初最大空闲块只有约 4 KB，正好一个回答消息的大小；回收内存后最大空闲块约 34 KB，这项选择待定 |
| 预录音 | 息屏时暂停；从息屏状态按下时没有预录音 | 持续采集会一直持有音频锁，芯片无法睡眠 |
| USB 主机 | 接着主机时不进 light sleep（`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`）；亮屏或接着主机时，应用每 `POCKET_USB_POLL_MS`（1 s）读一次 `usb_serial_jtag_is_connected()` | ESP-IDF 在 light sleep 时关掉 USB Serial/JTAG 引脚；睡过几次后 macOS 还留着这个设备，但串口没了，要重新插拔才回来，所以接在主机上睡眠的设备没法烧录和看日志 |

息屏时，除了 BLE 连接事件、30 s 一次的电量查询和空闲的 LVGL 任务（port 默认最长睡 500 ms），没有别的
周期性唤醒：应用任务睡到模型的下一个截止时间（`pocket_model_tick_wait_ms`），LVGL 用 `esp_timer`
取时间而不是 port 的 5 ms tick 定时器，录音计时的定时器只在录音界面运行，编码任务阻塞在队列上。

接着 USB 主机时芯片保持唤醒、屏幕常亮，串口不会掉。light sleep 的表现只能在充电器或电池供电时观察；
调试固件会在亮屏 3 s 后打印一行 `POCKET: standby`（息屏时长、light sleep 占比、连接断开次数、第一次
按键）。请在亮屏时插线：芯片睡眠时出现的主机可能枚举不出串口。

## 回答历史

回答在重启和 deep sleep 之后仍然保留；RAM 里只放屏幕上正在显示的那一条。

| 项目 | 行为 |
| --- | --- |
| 存储 | 分区 `history`（data 子类型 0x40，64 KB，位于 `0x7f0000`），只追加的环形记录，每条带 CRC（`main/pocket_hist.c`）。factory 应用分区到这里为止（8,257,536 B）。 |
| 容量 | 保留最新的若干条，文字合计不超过 16 KB（`POCKET_HIST_BUDGET_BYTES`），至少四条 4 KB 的完整回答；更早的被淘汰。 |
| 磨损 | 在写入位置前方按环形顺序擦除扇区：16 个扇区每写入 64 KB 回答各擦一次。不写 NVS。 |
| 何时存储 | 回答变为最终（`REPLY` final 1，或对应 id 的 `REPLY_DONE`），每个 `reply_id` 只存一次。按住说话期间等松开再写（擦写 Flash 会让 CPU 停顿）。 |
| 显示 | 开机后显示最新一条已存回答。在已存回答顶部按 UP、底部按 DOWN，切到更早（从末尾显示）或更新的一条；标题栏显示位置（`2/5`）。仍在流式输出的回答不能切走。 |
| 重新配对 | 删除配对时一并擦除历史。 |
| 掉电 | 写到一半掉电的记录 CRC 不通过，会被跳过；之后从下一个扇区继续写。 |

## 提醒与空闲省电

来了新回答（某个 `reply_id` 的第一条 `REPLY`，且尚未存储；包括没有按键等待时手机主动转发的回答），
屏幕点亮并播放一声短提示音。同一回答的后续文字不会重复提醒；已存储的回答再次发来时安静显示。两种提醒
各有一个开关（NVS `pocket/alert_scr`、`pocket/alert_tone`，默认都开）。按住说话期间不播提示音。提示音
由程序生成（两个正弦音：880 Hz 70 ms，再 1320 Hz 110 ms，输出音量 70%），经 ES8311 播放，不占 RAM；
打开这个开关时会试播一次。

| 阶段 | 何时 | 做什么 |
| --- | --- | --- |
| 链路空闲 | 息屏、没有按键、没有在等回答 | 设备请求 15–30 ms 连接间隔、外设延迟 15、4 s 监督超时（射频约每 0.5 s 醒一次，而不是每 30 ms；手机发来的消息最多晚约 0.5 s）。亮屏、按键或等回答时请求延迟 0、超时 2 s。取值依据 Apple 配件设计指南。 |
| Deep sleep | 在“自动休眠”设定时间内没有事件：30 分钟、1 小时（默认）、4 小时、从不（NVS `pocket/deep_slp`） | 事件指：按键动作，收到 `REPLY`、`WORK` 或 `RESULT`，插上或拔下 USB 主机。按住说话、等回答、显示配对码或接着 USB 主机时不会休眠。BLE 连接断开。任意按键（按键分压把 GPIO0 拉低）重新启动固件；这一下只负责开机和重连，不会录音。 |

## 内存

| 项目 | 设定 | 依据 |
| --- | --- | --- |
| LVGL 内存池 | 20 KB | 实测峰值 16,016 B（配对码界面）+ 25%，向上取整到 1 KB。其中约 2.6 KB 是分配器开销，新界面要用内存探针（`lv_max`）实测。 |
| 绘制缓冲 | 20 行（9.6 KB），`CONFIG_BSP_LVGL_DRAW_BUFFER_LINES` | 4 KB 回答界面整屏重绘约 104 ms（40 行时约 68 ms）。 |
| 优化级别 | `-Os` | 比 `-Og` 少 11 KB IRAM 代码；Opus 编码仍是每 20 ms 帧 4.4–4.6 ms（`-Og` 时 4.5–4.6 ms）。 |
| 回答副本 | 除 BLE 重组缓冲外，RAM 里只有一份（屏幕缓冲） | 大的手机消息直接从重组缓冲读取，拷贝完即释放。 |

内存探针固件（`sdkconfig.memprobe.defaults`）按脚本依次记录堆、LVGL 内存池、刷新耗时和任务栈：空闲、
设置、编码、4 KB 回答、存入六条回答、在历史里来回翻、重启一次。

## 音频

Opus 16 kHz 单声道，20 ms 帧，complexity 0，VBR，关闭 DTX 和 FEC（`esp_audio_codec` 2.5.0）。
不开预录音时，空闲期间 codec 休眠，按下时唤醒。开预录音时，手机连着并且亮屏就一直编码，保留最近
320 ms 的 Opus 包。

单核上的任务优先级：麦克风 7 > BLE 发送 6 > 编码 5 > LVGL 4 > 应用 3 > 统计 2。

## 界面语言与文字显示

- 文案：简体中文和英文，都在 `main/pocket_strings.h`（`POCKET_STR_<ID>` 和 `POCKET_STR_<ID>_EN`）；
  由手机选择（`APP_STATE`，见上文第 12 条）。英文界面用 DuoDuo 这个名字。广播名称
  `DuoDuo Pocket XXXX` 两种语言相同。
- 字体：Noto Sans SC 2.004，20 px，4 bpp，7667 个字形：可打印 ASCII、GB2312 全部字符、常用标点、
  Latin-1、欧元符号和 U+25A1。约占 1.5 MB Flash。
- 字符清单：`assets/fonts/pocket_cjk_charset.txt`，由 `tools/pocket/gen_fonts.sh` 生成。清单里
  Noto Sans SC 本身没有的 10 个码位，由覆盖检查列出。
- 缺字策略：字体画不出的码位和非法 UTF-8 一律显示为 U+25A1（□），并记日志计数，不会悄悄丢掉。
- 检查：电脑上跑 `tests/test_pocket_glyphs.py`（清单、全部界面文案、混排样例、一个反例）；设备开机时
  `pocket_ui_check_strings()` 用 `lv_font_get_glyph_dsc()` 逐字检查。

## 编译、测试与烧录

```sh
./tools/pocket/validate_macos.sh --static     # 仓库检查 + 主机测试（macOS）
./tools/pocket/validate_macos.sh --firmware   # 编译、合并、校验（需先激活 ESP-IDF 5.5.3）
# 带堆/CPU/音频/链路统计的调试固件：
idf.py -B build-debug -D SDKCONFIG=build-debug/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.debug.defaults" build
```

Linux 上直接运行 `./tools/validate.sh`。烧录时只写应用分区，NVS（绑定和设置）保留：

```sh
idf.py -B build-debug -p <PORT> app-flash monitor
```

合并镜像（`build/FoloToy-AI-Passport-full.bin`，写到 `0x0`）会重置 NVS，配对也会丢失。在加入 `history`
分区之前烧过的设备，需要先写一次分区表（`idf.py -B build-debug -p <PORT> partition-table-flash`，只写
`0x8000`，NVS 保留），再 `app-flash`。

发布版固件只用 `sdkconfig.defaults`（`idf.py -B build-release -D SDKCONFIG=build-release/sdkconfig build`，
或门禁脚本的 `--firmware`）。调试和探针固件在它之上叠加配置，仅用于开发，绝不能发布：它们会输出堆和
链路细节，探针还会注入假消息或循环进入 deep sleep。

| 叠加配置（在 `sdkconfig.defaults` 之上） | Kconfig | 作用 |
| --- | --- | --- |
| `sdkconfig.debug.defaults` | `POCKET_DEBUG_STATS` | 在串口输出堆、CPU、音频和链路统计；每次息屏结束后输出一行 `POCKET: standby`。 |
| + `sdkconfig.memprobe.defaults` | `POCKET_DEBUG_MEMPROBE` | 开机 30 s 后：整屏重绘计时、设置、10 s 编码、注入 4 KB 回答、存入六条回答、翻看历史、重启一次，并记录堆和 LVGL 内存池。 |
| + `sdkconfig.flowprobe.defaults` | `POCKET_DEBUG_FLOWPROBE` | 开机 30 s 后：不录音直接进入等待回答，再经真实的接收路径注入 `RESULT` 0、`WORK` 1 和 3、`REPLY_DONE` 0。不向手机发送任何内容。 |
| + `sdkconfig.sleeptest.defaults` | `POCKET_DEBUG_IGNORE_USB_HOST`、`POCKET_DEBUG_DEEP_SLEEP_S`、`POCKET_DEBUG_DEEP_SLEEP_TIMER_S` | 接着 USB 主机也会息屏，45 s 没有事件就进入 deep sleep，20 s 后由定时器唤醒，方便在串口上观察 deep sleep 和之后的启动。它会一直循环：请在设备醒着的时候烧录，用完换回正常固件。 |

每种固件用单独的构建目录（`build-debug`、`build-memprobe`、`build-flowprobe`、`build-sleeptest`），
`SDKCONFIG_DEFAULTS` 按顺序列出叠加配置，写法同上面的调试固件命令。

## 重新生成素材

```sh
tools/pocket/gen_fonts.sh          # 下载固定版本的 Noto Sans SC，lv_font_conv 1.5.3，覆盖检查
python3 tools/pocket/gen_avatars.py   # 需要 rsvg-convert 和 Pillow
python3 tools/pocket/render_mockups.py
```
