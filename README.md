# Windows 8.1 Driver Interoperability Research

Public excerpt of a personal research project that runs a modern laptop (2020s Intel platform) on
**Windows 8.1**, an OS its vendor never supported. The work makes newer hardware drivers usable on an
older kernel/driver model through compatibility shims, binary analysis, and AI-agent-assisted
debugging.

> 日本語の概要は下にあります。

## What is in this repository

| Path | Contents |
|---|---|
| `docs/ja/methodology-gpu-shim.md` | Methodology: how a user-mode shim lets a newer Intel graphics UMD run on the Windows 8.1 WDDM 1.3 stack, plus the Ghidra/cdb workflow and how work was split between AI agents (in Japanese) |
| `docs/ja/case-shim127-dxva-decoder-crash.md` | Case study: the Camera app crashed after recording. Root cause was a hardware video-decode context on an unsupported GPU node, which caused a driver double free. The fix hides the DXVA decoder profiles. |
| `docs/ja/case-shim121-125-camera-video-processor.md` | Case study: an earlier Camera / video-processor failure analysis |
| `probes/dxva-decoder-probe/` | A small D3D11 H.264 decoder probe that reproduces the crash path, plus the cdb breakpoint scripts used to trace it (paired breakpoints before and after each call) |
| `probes/display-affinity-probe/` | A Win32 probe for window display-affinity / screen-capture behaviour |

## Highlights

- Root-causing crashes inside closed-source graphics drivers using Ghidra plus live cdb tracing
  on a machine without OS symbols (stack scanning, paired breakpoints before and after each call).
- Designing compatibility shims that work at the DDI-callback level, and keeping each fix narrow
  and reversible: every shim version can be rolled back.
- Splitting work between AI agents: analysis and hard reviews go to stronger models, and routine
  OS operations go to faster ones. No result counts as success until it is verified on the real
  machine.

## What is intentionally NOT included

- Vendor driver binaries, decompiled vendor code, and the shim sources derived from vendor
  internals. These are excluded for licensing reasons.
- Third-party projects (for example the VxKex overlay), which have their own licensing.
- Machine-specific deployment scripts, credentials, logs and crash dumps.

Script names mentioned in the documents (`scripts/...`) refer to the private research repository and
are not part of this excerpt.

## Disclaimer

This is independent research for interoperability and education. It is not affiliated with or
endorsed by Microsoft or Intel. Modifying drivers can make a system unstable, so use these notes at
your own risk.

---

## 概要（日本語）

ベンダーがサポートしていない **Windows 8.1** を、比較的新しい Intel プラットフォームのノートPCで動かす個人研究から、公開できる部分だけを抜き出したものです。

- GPU ドライバー互換 shim の設計方針と、Ghidra と cdb による解析手順（`docs/ja/`）
- 実際の不具合の事例。カメラアプリが録画後にクラッシュする件では、未対応の GPU ノードでの HW デコードがドライバー内の二重解放を引き起こしていました。
- 再現用の自作プローブと、トレース用 cdb スクリプト（`probes/`）

ベンダーのバイナリと、それに由来するコード、第三者のコード、認証情報やログは含めていません。

## License

MIT. This covers only the original documents and probe sources in this repository.
