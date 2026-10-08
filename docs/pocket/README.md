<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DuoDuo Pocket firmware (pocket v1)

This fork turns the FoloToy AI Passport into a push-to-talk accessory of the DuoDuo Pocket
phone app. Hold OK and speak: the Passport streams Opus over an encrypted BLE link, the phone
uploads one voice note per press, and the latest answer is shown on the Passport screen.

## License

| Files | License |
| --- | --- |
| Files from upstream `folotoy/ai-passport`, including the ones modified here | MIT ([`LICENSE`](../../LICENSE)) |
| New files of this fork (`main/pocket_*`, `tools/pocket/`, `tests/test_pocket_*`, `tests/pocket_check.h`, `assets/fonts/pocket_cjk_charset.txt`, `assets/images/avatar/`, the Markdown in `docs/pocket/`, the `sdkconfig.*.defaults` overlays, `components/bsp/Kconfig`, the stubs `tests/audio_stubs/driver/i2s_common.h`, `tests/bsp_stubs/driver/gpio.h`, `tests/bsp_stubs/esp_attr.h`, `tests/bsp_stubs/esp_sleep.h`) | FSL-1.1-Apache-2.0 ([`LICENSE-FSL-1.1-Apache-2.0`](../../LICENSE-FSL-1.1-Apache-2.0)); every source file carries the SPDX header |
| Noto Sans SC glyph data in `assets/fonts/pocket_cjk_20.c` and `pocket_digits_44.c` | SIL Open Font License 1.1 ([`assets/fonts/LICENSE-OFL-1.1.txt`](../../assets/fonts/LICENSE-OFL-1.1.txt)) |

| DuoDuo brand assets: the mockups in `docs/pocket/mockups/` and the DuoDuo / OpenDuo names (trademarks) in English and Chinese | No open licence; all rights reserved by openduo. A fork must replace them. |

FoloToy names and marks remain FoloToy's. Third-party components of the firmware image and their
licences: [`NOTICE`](../../NOTICE).

## Controls and screens

| Input | Action |
| --- | --- |
| Hold OK ≥ 300 ms | Talk; release sends. Shorter taps are ignored. |
| OK while the phone link is down | Nothing is recorded; the screen says the phone is not connected. |
| UP / DOWN | Scroll the shown reply one page; at its top (UP) or bottom (DOWN), step to the older or newer stored reply. Dismiss a failure notice. |
| Long-press UP | Open or close settings (pre-roll, brightness, screen off, deep sleep, new-reply screen, new-reply tone, re-pair); five rows show at once and the list scrolls. |
| OK while the screen is dark | Lights the screen and starts the press at once (no wake press). |
| UP / DOWN while the screen is dark | Only light the screen; the rest of that press is ignored. |
| OK on the pairing code | Confirm LE Secure Connections numeric comparison; DOWN rejects. |

Mockups of all 20 screens in Simplified Chinese: [`mockups/overview.png`](mockups/overview.png),
one PNG per screen in [`mockups/`](mockups/); the same set in English in
[`mockups/en/`](mockups/en/) ([`overview`](mockups/en/overview.png)). They are rendered on the host by `tools/pocket/render_mockups.py` from the
same copy (`main/pocket_strings.h`), font and avatar images as the firmware. The firmware UI is
`main/pocket_ui.c`; the upstream demo menu and screens are not linked.

## BLE link

| Item | Value |
| --- | --- |
| Service | `8bd90001-86c5-454b-b65b-3f16cffb662f` (in the primary advertisement) |
| TX, device → phone | `8bd90002-86c5-454b-b65b-3f16cffb662f`, notify, encrypted + authenticated |
| RX, phone → device | `8bd90003-86c5-454b-b65b-3f16cffb662f`, write without response, encrypted + authenticated |
| Name (scan response) | `DuoDuo Pocket XXXX` (last two bytes of the BT MAC) |
| Security | LE Secure Connections only, MITM, bonding in NVS, numeric comparison confirmed with OK |
| Advertising | Whenever no phone is connected: 20 ms for 30 s after boot or a disconnect, then 1022.5 ms. A failed start is retried after `ADV_RETRY_MS` (1.1 s). A link that drops before NimBLE reported it arrives as a failed connect while its slot is still allocated; advertising restarts from the host queue once the slot is free. |
| Pairing mode | Accepted while no bond exists, or when the bonded phone re-pairs; settings → re-pair deletes the bond |
| PDU | `[type u8][flags u8][len u16 LE][payload]`, `flags bit0` = more fragments |
| Protocol version | 1.2 (1.1 adds `WORK`, 1.2 the `APP_STATE` language) |

Messages (integers little-endian; the full specification, including the phone's behaviour, is
`docs/ble-protocol.md` in the pocket-ios repository):

| Type | Name | Direction | Payload |
| --- | --- | --- | --- |
| 0x01 | `INFO` | device → phone | proto_major u8, proto_minor u8, fw_len u8, fw bytes, battery u8, charging u8, preroll u8 |
| 0x02 | `PRESS_START` | device → phone | press_id u16 |
| 0x03 | `AUDIO` | device → phone | press_id u16, seq u16, one Opus packet |
| 0x04 | `PRESS_END` | device → phone | press_id u16, packet count u16 |
| 0x05 | `STATUS` | device → phone | battery u8, charging u8 |
| 0x06 | `KEEPALIVE` | device → phone | press_id u16 |
| 0x81 | `RESULT` | phone → device | press_id u16, code u8 (0 transcribed, 1 empty, 2 ASR failed, 3 send failed, 4 no server), UTF-8 transcript |
| 0x82 | `REPLY` | phone → device | reply_id u32, final u8, UTF-8 text (replaces the text shown for that id) |
| 0x83 | `REPLY_DONE` | phone → device | reply_id u32 (0: the phone stopped waiting) |
| 0x84 | `APP_STATE` | phone → device | state u8 (0 ok, 1 server unreachable, 2 protocol major mismatch), then language u8 (0 zh-Hans, 1 en; 1.2) |
| 0x85 | `WORK` | phone → device | phase u8, optional UTF-8 label |

Link rules the phone app must follow as well:

1. Fragments of one message are consecutive PDUs of the same type; a different type or a
   malformed PDU drops the partial message.
2. Maximum reassembled phone → device message: 4096 bytes. Longer messages are cut at a UTF-8
   boundary and the screen notes that the full text is on the phone.
3. `INFO`: `proto_major u8, proto_minor u8, fw_len u8, fw bytes, battery u8, charging u8,
   preroll u8`. The firmware version string is length-prefixed.
4. Battery and charging use `0xFF` for "unknown". This board has no charge-status signal, so
   `charging` is always `0xFF`.
5. `AUDIO.seq` starts at 0 per press and counts every packet produced, including one the link
   refused, so the phone sees gaps. `PRESS_END.count` equals the number of sequence numbers used.
6. With pre-roll on, the ring packets are sent right after `PRESS_START` as seq 0..n-1.
7. `RESULT` for any press but the last confirmed one is ignored. An unknown result code is shown
   as "send failed".
8. `KEEPALIVE` is sent every 5 s from release until `REPLY_DONE`, a failed `RESULT`, the next
   confirmed press or link loss. There is no time cap.
9. `press_id` starts at a random value per boot.
10. `REPLY_DONE` with `reply_id` 0: the phone stopped waiting without a reply. Keepalives stop
    and a sending or transcript screen returns to the home screen (or the latest reply).
11. `APP_STATE` 2: protocol major mismatch. The device shows a version-mismatch screen, records
    nothing and keeps the mic off until `APP_STATE` 0/1 or the link drops.
12. `APP_STATE` language (1.2): the device shows its own copy in the phone's UI language at once,
    redraws the current screen and keeps it in NVS (`pocket/lang`; default Simplified Chinese). A
    missing byte (a 1.0/1.1 phone) or an unknown value keeps the current language. Text from the
    brain (transcript, reply, `WORK` label) is shown as sent, never translated.

The link is ready, and `INFO` is sent, only after the phone writes the TX CCCD on the current
connection. The TX subscription is not stored with the bond: a CCCD restored on a bonded reconnect
would count as subscribed as soon as encryption is up, before iOS has rediscovered services and
re-subscribed, and CoreBluetooth drops what is notified in that window.

`INFO` is also sent in answer to every `APP_STATE` received while the link is ready. A relaunched
phone app (reinstall, or a memory kill followed by state restoration) can subscribe again on a
connection that stays up; the CCCD does not change, so the device sees no event. The phone sends
`APP_STATE` after its link is ready, and that triggers the `INFO` it needs.

### Work status (protocol 1.1)

`WORK` (0x85, phone → device): `phase u8` (0 idle, 1 received, 2 thinking, 3 tool), then an
optional UTF-8 label.

| Situation | Device behaviour |
| --- | --- |
| `RESULT` code 0 for the waiting press | Working screen: transcript, phase label with three animated dots. Before any `WORK` the phase is "received". |
| `WORK` 1 / 2 / 3 | Label "received" / "thinking" / "looking it up" (`POCKET_STR_WORK_*` in `main/pocket_strings.h`); the dots animate. |
| `WORK` 0 | The dots stop; the last label stays; the wait and keepalives continue until `REPLY_DONE`. |
| `WORK` phase above 3 | Shown as thinking (a later minor). |
| `WORK` while not waiting, or while a reply is on screen | Recorded or ignored; the screen does not change. |
| `RESULT` code 0 after the wait ended | Ignored (no working screen without a wait). |
| Wait end | `REPLY_DONE` (any id), a failed `RESULT`, the next confirmed press, link loss, `APP_STATE` 2. |

The label is parsed and not shown by this firmware (empty in v1). The animation lights one dot
every `POCKET_WORK_ANIM_STEP_MS` (400 ms) through an LVGL timer that is paused whenever the
working screen is not shown or the phase is idle.

## Standby

The screen goes dark after the screen-off delay without a key or new content, and the chip enters
automatic light sleep while the BLE link stays up.

| Situation | Screen |
| --- | --- |
| No key and no new content for the delay (settings: 15 s, 30 s default, 60 s, never) | Dark: backlight off, panel in sleep mode (GRAM kept). |
| Talking; DuoDuo answering: sending, `RESULT` 0, received / thinking / looking it up, `REPLY` text still streaming (final 0) | On, no countdown. |
| The answer goes quiet: `REPLY_DONE` (any id), `WORK` idle with no streaming text, a failed `RESULT`, the next press, link loss | The countdown starts. |
| Pairing code shown; settings menu open | On, no countdown. |
| USB host connected (USB start-of-frame packets seen) | On, no countdown; seen while dark, it lights the screen. Unplugging starts the countdown. A charger sends no start-of-frame packets and is not seen. |
| Key press, new `REPLY`, other new content while lit | Restarts the countdown. |
| New answer (first `REPLY` of a `reply_id` not stored yet) while dark, with the new-reply screen setting on | Lights the screen and restarts the countdown. |
| Link lost, pairing request, failed `RESULT`, `APP_STATE` 1 or 2 while dark | Lights the screen and restarts the countdown. |
| Later `REPLY` texts of the same answer, or an answer already stored, while dark | Stays dark (the text is updated). |
| Link restored, battery change, `WORK` while dark | Stays dark. |

The setting is stored in NVS (`pocket/scr_off`, an index into `POCKET_SCREEN_OFF_LEVELS_S`).
The waiting screen of an advertising device with no bond also goes dark; a pairing request lights it.

### Power management

| Item | Setting | Basis |
| --- | --- | --- |
| CPU clock | 160 MHz while a lock is held, 40 MHz otherwise, automatic light sleep when no lock is held (`main/pocket_power.c`) | 160 MHz is the clock the firmware was measured at; 40 MHz is the crystal, the lowest clock without the PLL. |
| Lock `pocket_screen` (`ESP_PM_CPU_FREQ_MAX`) | Held while the screen is lit | The backlight LEDC and the LCD SPI stop in light sleep; LVGL keeps its old speed. |
| Lock `pocket_audio` (`ESP_PM_CPU_FREQ_MAX`) | Held while the codec is awake | I2S DMA stops in light sleep; the encoder timing was measured at 160 MHz. |
| BLE | Controller modem sleep, main crystal as the low-power clock, crystal powered in light sleep | The board has no 32 kHz crystal (GPIO0/GPIO1 are the key ladder and LCD CS); the internal 136 kHz RC is outside the 500 ppm a connection needs. |
| Button wake | GPIO0 (the key ladder pad) as a digital input; a low level wakes the chip; the 5 ms button poll stops while no key is down | Every key pulls the pad to <= 595 mV, below the ESP32-C3 VIL max of 825 mV. A periodic tick instead would wake the chip every poll period and add up to one period of latency. |
| Pins in light sleep | ESP-IDF isolates pins (`ESP_SLEEP_GPIO_RESET_WORKAROUND`); backlight and LCD CS keep their driven levels | A floating backlight enable or chip select is undefined. |
| CPU power-down in light sleep | Off (`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n`) | Saves ~100 uA but takes 1.68 KB of RAM. Chosen when the largest free block was ~4 KB, the size of one reply message; since the RAM reclaim the largest block is ~34 KB, so this choice is open. |
| Pre-roll | Paused while the screen is dark; a press from dark has no pre-roll | Continuous capture would hold the audio lock and keep the chip awake. |
| USB host | No light sleep while a host is connected (`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`); the app reads `usb_serial_jtag_is_connected()` every `POCKET_USB_POLL_MS` (1 s) while lit or connected | ESP-IDF switches the USB Serial/JTAG pad off in light sleep; after some sleeps macOS keeps the device but loses its serial port until a replug, so a sleeping device on a host cannot be flashed or monitored. |

Nothing wakes the chip periodically while the screen is dark, except the BLE connection events, the
30 s battery poll and the idle LVGL task (the port's 500 ms maximum sleep): the application task
sleeps until the next model deadline (`pocket_model_tick_wait_ms`), LVGL takes its time from
`esp_timer` instead of the port's 5 ms tick timer, the recording-clock timer runs only on the
recording screen, and the encoder blocks on its queues.

A USB host keeps the chip awake and the screen on, so the console stays up. Light-sleep behaviour
can only be observed on a charger or battery; the debug image logs one `POCKET: standby` line (dark
time, light-sleep share, link drops, the first key) 3 s after the screen lights. Plug the cable in while the
screen is lit: a host that appears while the chip sleeps may not enumerate the serial port.

## Reply history

Replies survive reboot and deep sleep; RAM holds only the reply on screen.

| Item | Behaviour |
| --- | --- |
| Storage | Partition `history` (data subtype 0x40, 64 KB at `0x7f0000`), an append-only ring of CRC-checked records (`main/pocket_hist.c`). The factory app partition ends there (8,257,536 B). |
| Budget | The newest replies whose texts total at most 16 KB (`POCKET_HIST_BUDGET_BYTES`), at least four full 4 KB replies; older ones drop out. |
| Wear | Sectors are erased ahead of the write position, round the ring: each of the 16 sectors is erased once per 64 KB of replies. NVS is not written. |
| Stored when | A reply becomes final (`REPLY` final 1, or `REPLY_DONE` for its id), once per `reply_id`. During a press the write waits for the release (flash erase stalls the CPU). |
| Shown | The newest stored reply after boot. UP at the top / DOWN at the bottom of a stored reply steps to the older (shown at its end) / newer one; the header shows the position (`2/5`). A reply still streaming is not stepped away from. |
| Re-pair | Erases the history together with the bond. |
| Power loss | A record cut by power loss fails its CRC and is skipped; writing resumes at the next sector. |

## Alerts and idle power

A new answer (the first `REPLY` of a `reply_id` that is not stored; any answer, also one the phone
forwards with no press pending) lights the screen and plays a short tone. Partial texts of the same
answer do not repeat the alert; an answer already stored is shown quietly. Each alert has its own
setting (NVS `pocket/alert_scr`, `pocket/alert_tone`; both on by default). No tone plays during a
press. The tone is generated (two sine notes, 880 Hz for 70 ms then 1320 Hz for 110 ms, output
volume 70 %) through the ES8311 and costs no RAM; turning the setting on plays it once.

| Stage | When | What |
| --- | --- | --- |
| Link idle | Screen dark, no press, no reply wait | The device requests peripheral latency 15 at a 15-30 ms interval and a 4 s supervision timeout (radio wake every ~0.5 s instead of every 30 ms; a phone message waits up to ~0.5 s longer). A lit screen, a press or a reply wait requests latency 0 and a 2 s timeout. Values follow Apple's accessory guidelines. |
| Deep sleep | No event for the deep-sleep setting: 30 min, 1 h (default), 4 h, never (NVS `pocket/deep_slp`) | Events: a button edge, a `REPLY`, `WORK` or `RESULT`, a USB host plugged or unplugged. A press, a reply wait, the pairing code or a USB host keep the device awake. The BLE link drops. Any key (the ladder pulls GPIO0 low) boots the firmware again; that press only boots and reconnects, it never records. |

## Memory

| Item | Setting | Basis |
| --- | --- | --- |
| LVGL pool | 20 KB | Measured peak 16,016 B (pairing code screen) + 25 %, rounded up to 1 KB. ~2.6 KB of it is allocator overhead, so measure new screens with the memory probe (`lv_max`). |
| Draw buffer | 20 lines (9.6 KB), `CONFIG_BSP_LVGL_DRAW_BUFFER_LINES` | A full redraw of the 4 KB reply screen takes ~104 ms (40 lines: ~68 ms). |
| Optimisation | `-Os` | 11 KB less IRAM code than `-Og`; Opus encode stays at 4.4-4.6 ms per 20 ms frame (4.5-4.6 ms at `-Og`). |
| Reply copies | One in RAM (the screen's buffer) besides the BLE reassembly buffer | Large phone messages are read in place from the reassembly buffer and released after the copy. |

The memory probe image (`sdkconfig.memprobe.defaults`) logs heap, LVGL pool, refresh time and task
stacks through a scripted sequence: idle, settings, encoding, a 4 KB reply, six more replies, a
walk through the history and one restart.

## Audio

Opus 16 kHz mono, 20 ms frames, complexity 0, VBR, DTX off, FEC off (`esp_audio_codec` 2.5.0).
Without pre-roll the codec sleeps while idle and wakes on the press. With pre-roll the encoder
runs while the phone is linked and the screen is lit, and keeps the last 320 ms as Opus packets.

Task priorities on the single core: mic 7 > BLE TX 6 > encoder 5 > LVGL 4 > app 3 > stats 2.

## UI languages and text

- Copy: Simplified Chinese and English, both in `main/pocket_strings.h` (`POCKET_STR_<ID>` and
  `POCKET_STR_<ID>_EN`); the phone selects one (`APP_STATE`, item 12 above). English uses the
  name DuoDuo. The advertised name, `DuoDuo Pocket XXXX`, is the same in both languages.
- Font: Noto Sans SC 2.004, 20 px, 4 bpp, 7667 glyphs: printable ASCII, all of GB2312, common
  punctuation, Latin-1, euro sign and U+25A1. About 1.5 MB of flash.
- Inventory: `assets/fonts/pocket_cjk_charset.txt`, regenerated by `tools/pocket/gen_fonts.sh`.
  The ten inventory code points absent from Noto Sans SC are listed by the coverage check.
- Missing-glyph policy: any code point the font cannot draw, and any invalid UTF-8, is shown as
  U+25A1 (□) and counted in the log. Nothing is dropped silently.
- Checks: `tests/test_pocket_glyphs.py` (inventory, all UI copy, mixed sample text, a negative
  case) on the host; `pocket_ui_check_strings()` with `lv_font_get_glyph_dsc()` on the device at
  boot.

## Build, test and flash

```sh
./tools/pocket/validate_macos.sh --static     # repository checks + host tests (macOS)
./tools/pocket/validate_macos.sh --firmware   # build, merge, verify (ESP-IDF 5.5.3 active)
# Debug image with heap/CPU/audio/link statistics on the console:
idf.py -B build-debug -D SDKCONFIG=build-debug/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.debug.defaults" build
```

On Linux, `./tools/validate.sh` runs unchanged. Flash with an application-only write, which
keeps NVS (bonds and settings):

```sh
idf.py -B build-debug -p <PORT> app-flash monitor
```

The merged image (`build/FoloToy-AI-Passport-full.bin` at `0x0`) resets NVS and therefore the
pairing. A device flashed before the `history` partition existed needs the partition table once
(`idf.py -B build-debug -p <PORT> partition-table-flash`, which writes `0x8000` only and keeps NVS),
then `app-flash`.

Release image: `sdkconfig.defaults` only (`idf.py -B build-release -D SDKCONFIG=build-release/sdkconfig build`,
or the gate's `--firmware`). Debug and probe images add overlays on top of it. They are for
development only and must never ship: they log heap and link details, and the probes inject fake
messages or cycle deep sleep.

| Overlay (on top of `sdkconfig.defaults`) | Kconfig | What it does |
| --- | --- | --- |
| `sdkconfig.debug.defaults` | `POCKET_DEBUG_STATS` | Heap, CPU, audio and link statistics on the console; one `POCKET: standby` line after each dark period. |
| + `sdkconfig.memprobe.defaults` | `POCKET_DEBUG_MEMPROBE` | 30 s after boot: timed full redraws, the settings, 10 s of encoding, an injected 4 KB reply, six more injected replies, a history walk and one restart, with heap and LVGL pool marks. |
| + `sdkconfig.flowprobe.defaults` | `POCKET_DEBUG_FLOWPROBE` | 30 s after boot: a reply wait without recording, then `RESULT` 0, `WORK` 1 and 3 and `REPLY_DONE` 0 injected through the real receive path. Sends nothing to the phone. |
| + `sdkconfig.sleeptest.defaults` | `POCKET_DEBUG_IGNORE_USB_HOST`, `POCKET_DEBUG_DEEP_SLEEP_S`, `POCKET_DEBUG_DEEP_SLEEP_TIMER_S` | Darkens the screen on a USB host, deep-sleeps after 45 s without an event and wakes by timer after 20 s, so deep sleep and the boot after it can be followed on the console. It cycles forever: flash it while the device is awake and replace it afterwards. |

Build each in its own directory (`build-debug`, `build-memprobe`, `build-flowprobe`, `build-sleeptest`)
with `SDKCONFIG_DEFAULTS` listing the overlays in order, as in the debug command above.
Replies injected by a probe are never written to the `history` partition, so a normal image
flashed afterwards does not show them; the history walk covers the replies already stored.

## Regenerating assets

```sh
tools/pocket/gen_fonts.sh          # pinned Noto Sans SC download, lv_font_conv 1.5.3, coverage check
python3 tools/pocket/gen_avatars.py   # needs rsvg-convert and Pillow
python3 tools/pocket/render_mockups.py
```
