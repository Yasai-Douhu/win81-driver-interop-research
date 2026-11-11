# Camera crash after recording on L15 (2026-11-11)

## Symptom

In the Windows 8.1 Camera app, recording a video and stopping it crashes the app (WER `MoBEX`, `c0000005`, `PCH_EF_FROM_ntdll+0x9079A`). The saved clip looks like a still image.

## Check before the investigation: Shim125 is no longer used

This was checked after the reboot (boot time 2026-11-10 23:45). Every process that loads the GPU UMD loads Shim126 (`5D74307E…`) plus the inner 5077 UMD (`legacy5077\igd10iumd64.dll`, `B07A70C3…`). This covers DWM, explorer, WWAHost and Obsidian.

The check script is `scripts/l15-boot-shim-check.py`.

## Analysis

**The dump.** The WER report from 23:46, after the reboot and therefore with Shim126, is in `analysis/l15_data/camera-record-crash-20261110/`.
- The crash thread is a worker thread of the Microsoft H.264 decoder (`msmpeg2vdec`). It runs D3D11 `CreateVideoDecoder` → 5077 `igd11dxva64`.
- The request is for `DXVA2_ModeH264_VLD_NoFGT` at 1280×720 NV12.
- The decoder is used for the preview of the recorded clip.

**Reproduction without Camera.** `patches/src/l15-dxva-decoder-probe` creates a hardware D3D11 device and calls `CreateVideoDecoder` with the same parameters. It crashes in the same function (`igd11dxva64+0x2aa1a3`), also under WinRM in session 0.

**cdb trace on L15** (Windows 8.1 SDK cdb placed under ProgramData; no system change):

- `FUN_1802a8430` (type 1, decode) → `FUN_1802b9b70` (H.264 init).
- `FUN_1802bb5b0` → OS interface `+0x100`, CreateGpuContext for context 2 (VIDEO) on **node 1 (VCS)**, returns `0x8007000E`.
- `b9b70` frees its state on failure (`FUN_1802b9f20`). Then the caller `FUN_180295630` frees the same state again through `FUN_1802a8370` → `b9f20`. This double free causes an AV through a freed function table (under cdb it appears as heap corruption `c0000374`).

**Cause.** The shim's `ShimCreateContextCb` accepts only node 0 (RCS) and node 3 (VECS, Shim122). It returns `E_NOTIMPL` for node 1 (VCS, the video decode engine). Intel's driver reports that as `E_OUTOFMEMORY` and then crashes in its own cleanup bug. Every hardware H.264 decoder creation therefore crashed.

The clip that looks like a still image is probably a side effect of the crash: the app may have died before it finished writing the file. Whether the Intel hardware encoder MFT (`mfx_mft_h264ve`, also needs VCS) is involved has not been checked.

## Fix: Shim127

Shim127 is Shim126 plus one change. It wraps `RetrieveSubObject(D3D11_1DDI_VIDEO_FUNCTIONS)` and makes the video device report no decoder profiles:
- `GetVideoDecoderProfileCount` returns 0;
- `CheckVideoDecoderFormat` returns FALSE;
- `GetVideoDecoderConfigCount` returns 0;
- `CreateVideoDecoder` returns `E_NOTIMPL`.

Media Foundation then uses its software decoders and encoders. The video processor (VECS) and the camera preview path are unchanged. Placing `l15-video-decode-allow.flag` next to the shim keeps the driver's table unchanged.

- Source: `patches/src/l15-gpu2137-umdshim127`.
- Build: `scripts/Build-L15-GPU2137-UmdShim127.py` (VS2013 `/W4 /WX`). DLL SHA `D4E930AE…D2E1`.
- Deploy: `scripts/Deploy-L15-GPU2137-Shim127.py install|rollback|status`. Shim126 is kept as `igd10iumd64.shim126.dll`. DWM keeps Shim126 until the next restart.

**L15 result.** Installed (`LiveIs=Shim127`, `Kept126=true`). The decoder probe now gets `profiles=0` and exits with code 0, so the crash is gone.

**User confirmation (2026-11-11).** Recording and stopping in the Camera app no longer crashes, and the saved clip plays back normally.

**Still unverified.**

- Behaviour after the restart in which DWM also loads Shim127.

**Real fix (future).** Support node 1 (VCS) contexts and their command submission in the shim, which would allow hardware decode and encode. This is a separate task with a risk of GPU hangs and TDR on the real machine.
