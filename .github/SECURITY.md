English | [简体中文](/.github/SECURITY.zh_CN.md)

# Security Policy

Do not disclose unpatched security vulnerabilities, credentials, tokens, or
directly exploitable details in public issues, pull requests, discussions, or
chat.

## DuoDuo Pocket firmware

Report issues in the pocket firmware (`main/pocket_*`, the BLE link, pairing, stored replies)
through the private vulnerability reporting form of the repository that publishes this fork, not
the upstream FoloToy form. Issues in unchanged upstream code go to FoloToy as described below.

Security model of the pocket firmware:

| Area | Design |
| --- | --- |
| Radios | Bluetooth LE only. The pocket image does not link or start Wi-Fi. |
| Pairing | LE Secure Connections only (legacy pairing is compiled out), MITM protection, numeric comparison: the six digits shown on the Passport are confirmed with OK and on the phone. |
| Pairing window | Pairing is accepted only while no bond exists, or when the bonded phone pairs again. Settings → re-pair deletes the bond and the stored replies. A new phone is refused while a bond exists. |
| GATT access | Both pocket characteristics require an encrypted, authenticated link with a 16-byte key. The TX subscription is not stored with the bond. |
| Input | Phone messages are reassembled into a fixed 4096-byte buffer; longer messages are cut at a UTF-8 boundary, malformed fragments drop the partial message, invalid UTF-8 is shown as a placeholder glyph. |
| Data at rest | Bonding keys and settings live in NVS; the newest replies (up to 16 KB of text) live in the `history` flash partition. Flash encryption and secure boot are not enabled, so anyone with the device and a USB cable can read them. |
| Audio | The microphone records only while OK is held on a ready link, plus the optional 320 ms pre-roll that runs while the screen is lit and the phone is linked. |
| Identity | The device advertises with its public Bluetooth address; the name carries its last two bytes. The device is therefore trackable while it advertises. |
| Console | The release image logs link and state events over USB; it does not log transcripts or replies. |

## Reporting a vulnerability

The preferred channel is GitHub's private vulnerability reporting form:

<https://github.com/FoloToy/ai-passport/security/advisories/new>

If that form is unavailable, open a public issue containing only a request for a
private security contact. Do not include vulnerability details or reproduction
materials in that issue; wait for a maintainer to provide a private channel.

## What to include

Please provide as much of the following as possible:

- affected version, commit, or distribution channel;
- affected board, component, and configuration;
- reproduction steps, a minimal PoC, or logs, after removing credentials and
  personal data;
- potential impact, exploitation requirements, and any suggested mitigation.

This repository contains firmware for a wearable AI device. Security issues may
involve the ESP32-C3 firmware, the audio input, wireless capabilities, or
protocol handling. If the issue also involves a separately maintained server or
service, identify the affected endpoint and client version so that we can route
it to the appropriate maintainers.

## Response process

We will acknowledge the report, reproduce it, assess its impact, and update the
report when a fix or mitigation can be disclosed. Our target cadence:
**acknowledgement with an initial assessment within 5 business days**, and a
**90-day coordinated disclosure window** for confirmed vulnerabilities
(adjustable in coordination with the reporter). If you hear nothing for more
than 7 days, please ping us through the other channel (email or GitHub).

## Contributor notes

- Do not put real user data, access tokens, private keys, or internal endpoints
  in issues, test fixtures, or commits.
- If you accidentally commit sensitive information, report it privately
  immediately. Deleting the file from the working tree does not invalidate
  secrets that may exist in Git history.
- Use public issues for ordinary bugs, documentation problems, and feature
  requests. Do not use the security channel for those topics.
