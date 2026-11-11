# L15 Intel GPU（研究スタック）解析とShim改修のベストプラクティス（2026-11-11）

作成：Claude Code（Opus 5.5）。Codexへ作業を戻すときの知識の蓄積用です。2026-11-05から11-11までに、Shim92からShim127まで改修してきた進め方を整理しました。個々の事実と証拠は、本文中で示す各docにあります。この資料は「どう進めたか」「何を守るべきか」をまとめるもので、新しい実測は含みません。

---

## 1. 前提：今のスタックの構造

```
アプリ / DWM
  └ d3d11.dll（Windows 8.1、WDDM 1.3 runtime）
      └ S07\igd10iumd64.dll   ← 自作shim（igd10umd81shim92.cpp系、現在Shim127）
          └ S07\legacy5077\igd10iumd64.dll   ← Intel 21.20.16.5077の3D UMD（原本）
              ├ igd11dxva64.dll（5077の映像UMD。System32から直接ロード。VPHAL/DXVA）
              ├ igdgmm64.dll / igc64.dll / igdusc64.dll
          （KTコールバックはshimが横取り）
  └ dxgkrnl / dxgmms1（Windows 8.1）
      └ igdkmd64.sys（Win11用31.0.101.2137を8.1向けに改変：Map99→Scan118系、C118配置）
```

- **UMDとKMDの世代が違うこと**が全問題の根本です。5077 UMDは5077 KMDとの私有ABI（コンテキスト私有データ、Escapeパケット長、パッチリストの意味、GMMレコード形式）を前提にしています。実際に動くのは2137 KMDです。shimはこのずれを**呼び出しの境界で1つずつ翻訳**します。
- shimが横取りするもの：`pfnAllocateCb/DeallocateCb/LockCb/UnlockCb/EscapeCb/CreateContextCb/DestroyContextCb/RenderCb`（`ShimCreateDevice`内の`WrappedKT`）、`OpenResource`系（Shim107）、`RetrieveSubObject`（Shim127、映像の関数表）。
- 割り当ては`Shim92::AllocBridge`（`shim92_alloc.cpp`）が5077レコード（0x4c8）と2137 GMM（0x890）を対応付けます。
- 実機上の場所：`C:\Windows\System32\L15Gfx2137S07\igd10iumd64.dll`（ライブ）、`igd10iumd64.shimNNN.dll`（直前版の退避）、`*.dll.oldXXXXXX`（使用中ファイルをリネームした残骸）、`l15-dwm-allow.flag`（shimの動作許可）。

---

## 2. 原因特定の基本手順（毎回これを回す）

1. **症状をアプリから切り離して最小の再現を作る**
   - 例：カメラ黒画面 → `l15-d3d11-vp-probe.ps1`。WebGL不可 → `BufProbe.exe`。CUAスクリーンショットAV → `l15-cua-capture-d3d-probe.ps1`。録画後クラッシュ → `patches/src/l15-dxva-decoder-probe`。
   - プローブは**各手順を実行前に書き出してflush**します。プロセスが落ちても最後の手順が残ります。
   - 1変種を1プロセスで実行します（子プロセス方式）。落ちても次の変種が走り、後始末も確実になります。
   - WinRMのsession 0でD3D11ハードウェアデバイスは作れます。Storeアプリやユーザーセッションが要る場合だけ、Flashに対話タスクとして実行させます（§7）。
2. **落ちた/失敗した場所を確定する**
   - WERダンプ（`%LOCALAPPDATA%\Microsoft\Windows\WER\ReportQueue\AppCrash_*`）を`scripts/Fetch-L15-File.py`で回収し、ホストのcdb（windbg MCP `open_cdb_dump`）で開きます。
   - 8.1のOSシンボルは取れないので、**`dps @rsp L2000`でスタックをスキャン**し、モジュール+オフセットの列から呼出し元を復元します。出力が大きいときはファイルに落としてgrepします。
   - 例外時のレジスタで分かることは多いです。`r15=8007000e`のように**直前のHRESULTがレジスタに残っている**ことがあります。
3. **Ghidraで該当関数を読み、失敗を返しうる分岐を列挙する**（§3）
4. **実機でライブトレースして、どの分岐かを確定する**（§4）。静的解析だけで直さないでください。2026-11-11のShim127では、cdbトレース2回で「CreateGpuContext(node1)が`8007000E`」まで絞りました。
5. **shimで翻訳するか、機能を安全に閉じるかを決める**（§5）
6. **VM回帰 → 実機配置 → 同じプローブで修正を確認 → ユーザーが実アプリで確認**（§6）

> **原則：shimが拒否・未実装を返していないかを最初に疑う。** 実際の根本原因の多くはshim自身でした。
> - Shim97/99：Escape D16=15をshimが拒否していた。
> - Shim120：割り当て表が256枠で尽きていた。
> - Shim121：Escape 17/7（80→224バイト）を知らずに拒否していた。
> - Shim122：node 3を拒否していた。
> - Shim127：node 1を拒否していた。
>
> Intelのコードは失敗を`E_OUTOFMEMORY (0x8007000E)`に丸めることが多いです。「メモリ不足」は**ほぼ確実に誤誘導**です。

---

## 3. Ghidraの使い方（MCP経由）

### 3.1 準備
- Ghidra MCP（`mcp__ghidra__*`）を使います。過去に、javawが生存していても8089の待受が消えていたことがあります（handoff §1）。最初に`check_connection`で確認します。
- 解析対象の原本はリポジトリ内にあります。**実機でロードされている版と同じSHAのファイル**を選びます。
  - 5077 UMD群：`analysis/l15_data/device-backport01-20261101/Graphics-Baseline/oem12/`（`igd11dxva64.dll` 21.20.16.5077、48,282,832 bytes など）。
  - 2137 KMD：`originals/l15-videodriver-20261102/Graphics/`の原本、または`patches/build/`の改変版。
  - 同名のDLLが版違いで多数あるので、`lmvm`のFile version/Timestampと照合します。
- `import_file`（`auto_analyze: true`）。47MBのDLLでも数分で完了しました。

### 3.2 アドレス換算（最重要）
- 64bit DLLの既定ImageBaseは`0x180000000`、KMDは`0x140000000`です。**Ghidraアドレス = ImageBase + RVA**。
- cdb/WERのオフセット`igd11dxva64+0x2aa204`は、Ghidraでは`0x1802aa204`です。
- 逆方向も同じです。cdbのブレークポイントは`bu igd11dxva64+0x2b9e91`のように**RVAで書けば**ASLRに影響されません。
- 書類には常に`FUN_1802b9b70`（Ghidra名）とRVAの両方を残します。次の担当者が同じ関数に着けます。

### 3.3 よく使う操作と目的
| 目的 | 操作 |
|---|---|
| クラッシュ位置の関数を知る | `get_function_by_address`（戻り先アドレスでも可） |
| 失敗分岐とHRESULTの出所を読む | `decompile_function`。`return 0x8007000e`、`uVar = 0x8007000e`を探す |
| cdbのBP位置（呼出し直後のアドレス）を得る | `disassemble_function`。`CALL qword ptr [RSI + 0x100]`の次の命令がBP位置 |
| 呼出し関係 | `get_function_callers` / `get_function_callees` / `get_xrefs_to` |
| 定数やパケット形を探す | `search_byte_patterns`（例：ヘッダー`0x13D5`は`d5 13`、MI_BATCH_BUFFER_START `0x18800500`） |
| 文字列からの入口 | `search_strings`（"StatusQueryBuffer"、"MPEG2 ISlice…"、レジストリ設定名など） |

### 3.4 読み方のコツ
- **関数テーブル呼出し（`(**(code **)(lVar + 0x228))`）は、そのテーブルの正体を先に決めます。**
  - 5077映像UMDのMOS OSインターフェース：`+0x100`がCreateGpuContext、`+0x120`がSetGpuContext、`+0x218`がAllocateResource、`+0x228`がFreeResource、`+0x230/+0x238`がLock/Unlock（Shim127の解析で確認した範囲）。
  - 1つの呼出しの意味が分かると、周辺の呼出しの意味もまとめて分かります。
- 失敗パスの**後始末が二重になっていないか**を見ます。Intelコードには「内側でcleanup → 外側でも同じcleanup」という二重解放がありました（`FUN_1802b9b70`と`FUN_1802a8370`がともに`FUN_1802b9f20`）。このため、失敗するとHRESULTではなくクラッシュになります。**クラッシュ箇所は真の原因ではなく、二次被害であることが多い**です。
- 5077 KMDと2137 KMDの**同じ機能の入口を並べて比較**します（camera-diagnosis §根本原因の表。Escape 17の入口、パケット長、サブコード表、前提ビット）。差がパケット長や1ビットだけ、ということがよくあります。
- 解析した構造体オフセット（`priv96[0x24]=1 / [0x54]=3`、DriverId bit26、GMM `+0x470/+0x478`など）は、shimのソースコメントに**根拠の関数名付き**で残します（`ShimCreateContextCb`のコメントが例）。

---

## 4. 実機のライブ観測

### 4.1 ユーザーモード：cdb（KD不要。Shim127で確立）
- L15にはデバッガがありません。**Windows 8.1 SDKのx64 cdb**（`cdb.exe dbgeng.dll dbghelp.dll symsrv.dll srcsrv.dll`）を`C:\ProgramData\l15-dxva-probe\cdb\`へコピーして使います。システム設定は変更しません。バイナリはリポジトリにコミットしません。
- 実行：`scripts/Run-L15-DecoderTrace.py <コマンドファイル>`（broker経由。プローブとcdbを転送し、`-cf`で実行し、ログを回収）。
- コマンドファイルの型（`patches/src/l15-dxva-decoder-probe/trace2.txt`）：
  ```
  .symopt+ 0x4
  sxe -c ".printf \"AV\\n\"; r; k 40; q" av
  bu igd11dxva64+0x2bb764 ".printf \"CreateGpuCtx ctx=%x node=%x\\n\", @edx, @r8d; gc"
  bu igd11dxva64+0x2bb76a ".printf \"CreateGpuCtx ret=%08x\\n\", @eax; gc"
  g
  q
  ```
  - **呼出し前のBP**で引数（rcx/rdx/r8/r9）を見て、**呼出し直後のBP**で`@eax`を見る、という対で置きます。
  - 失敗点が分かったらそこで`q`すると、ログが短く済みます。
  - 未ロードのモジュールに対する`bu`の警告（"Unable to resolve…from module X"）は無害です。
  - デバッガ下ではヒープが厳密になるので、二重解放はAVではなく`c0000374`（ヒープ破損）として見えます。
- 段階的に絞ります。1回目は上位関数の戻り値を一斉に取り、2回目は失敗した関数の内部の呼出しだけを取ります。トレース2回で特定できました。

### 4.2 カーネル：KDNET
- KMD側の分岐、Patch/DMA内容、PTE walk、TDR/MarkDeviceの監視はKDNETで行います。手順は`docs/claude-kdnet-guide-20261106.md`と`claude-code-kdnet-troubleshooting-20261106.md`を参照してください。
- **kdを先に待機させてから起動します。後付け接続は不可**です（handoff §7）。
- KDの揮発編集（`eb igdkmd64+0x36d6e7 b0 01`など）は切り分け専用です。効果を確認したら**必ず恒久版（KMDバイナリ＋PE checksum＋署名）へ移し、KD編集なしで再現**して完了とします（§9→§10の流れ）。
- BPは同時32個まで。`bc`とBPが同時に入るとstaleなint3で止まるので、先に`go`を確認します。

### 4.3 shimのトレースログ
- `C:\ProgramData\L15ShimLog`が存在するときだけ書きます（常時ログは11-07から停止中。`Set-L15-ShimLog119.py on/off`）。プロセス単位なら環境変数`L15_GPU2137_SHIM92_LOG`を使います。
- 失敗の第一報はたいてい`Shim92EscapeRejected …`や`Shim92CreateContextUnsupported Node=…`です。新しいshimを作るときは**拒否・翻訳の行を必ずログに出す**ようにします（Shim121の`Shim121Escape17Sub7`、Shim127の`Shim127VideoDecodeHidden`）。

### 4.4 画素・読み戻しで「成功」を証明する
- DDIのS_OKは成功の証拠ではありません。
  - Shim122ではBltがS_OKなのに出力は無変化でした。
  - 2026-11-08 21:45の読み戻しプローブ：既知色でクリア → Blt → 読み戻しで、1バイトも変わらないことを示しました。
- 判定は読み戻しSHA、画素統計（sd/distinct）、`frame-*.png`のスキャンアウト取得などで行います。

---

## 5. shimの改修方針

### 5.1 翻訳か閉鎖か
| 状況 | 方針 | 例 |
|---|---|---|
| 5077と2137で機能は同じ、形だけ違う | **翻訳**（パケット詰め直し、ビット変換、レコード差し替え） | Shim121 Escape 80→0xE0、Shim125 DriverId bit26クリア、Shim107 OpenResource、Shim108 Format 0x7e→0x17 |
| shim内部の制約 | **制約を取り除く** | Shim120 256枠→チャンク確保、Shim126 シャドウをByteSizeへ |
| 下層に機能がない（エンジン未対応など）、翻訳に大きなリスクがある | **機能を安全に閉じる**。上位が正規のフォールバックへ進む返し方を選ぶ | Shim127 デコーダープロファイル0件（MFがソフトウェアへ） |

- 閉じるときは、**失敗を早く、正規の形で返します**。Intelの深い失敗パスに入らせると、二重解放などの二次被害が出ます。Shim127は`CreateVideoDecoder`より手前の列挙（ProfileCount=0）で止めました。
- 閉じた機能は、ファイルフラグで元に戻せるようにします（`l15-video-decode-allow.flag`）。
- **架空のGPU VA、ダミー成功、Fence置換は禁止**です（development-goal）。Shim125も「VAを作る」のではなく「2137 KMDに実VAを解決させる」修正でした。

### 5.2 変更の作法
- **1版 = 1変更。** 直前版のディレクトリを`patches/src/l15-gpu2137-umdshimNNN`へ丸ごとコピーし、差分は目的の1点だけにします。調査用には、**挙動を変えないログ版**を挟みます（Shim124）。
- ビルドは`scripts/Build-L15-GPU2137-UmdShimNNN.py`（VS2013 x64、`/W4 /WX`）。`build.json`にDLLとソースのSHAを残します。配置スクリプトはこのSHAとの一致を確認します。
- ソースのコメントには、根拠（Ghidraの関数名、オフセット）と、なぜそうするかを書きます。
- DDIの型はWindows Kits 8.1の`d3d10umddi.h`で確認します。
  - 例：映像DDIは`D3D11_1DDI_DEVICEFUNCS`ではなく、`RetrieveSubObject(D3D11_1DDI_VIDEO_FUNCTIONS)`で渡る`D3D11_1DDI_VIDEODEVICEFUNCS`にあります。
  - 推測で関数表の位置を決めないでください。

### 5.3 配置（Deploy-L15-GPU2137-ShimNNN.py）
- `install`の条件は、ライブUMDが**直前版のSHAと一致する**ことです。
  - 直前版を`igd10iumd64.shim<直前>.dll`へ退避します。
  - ライブファイルは`.old<rand>`へリネームしてから新版を置きます（使用中でも置換できる）。
  - 置いた後のSHAが不一致なら、自動で直前版へ戻します。
- `rollback`は退避版のSHAを照合してから戻します。`status`はライブ・退避・DWMの読み込み・Display状態を出します。
- KDが必要か、DWMを再起動するかは版ごとに判断します。
  - ユーザーモードのCPU処理だけの変更（Shim126/127）はKD不要、DWM再起動なしです。新しく起動したプロセスから新版になり、DWMは次回起動から新版になります。
  - 表示経路に関わる変更はKDNET下で`dwmtest`（DWMを再起動して確認）を行ってきました。
- **再起動後に、全プロセスで旧版が消えたことを確認します**。`scripts/l15-boot-shim-check.py`が、各プロセスの読み込みモジュールとSHAを列挙します。

---

## 6. 検証の順序と判定

1. **VM回帰**（`Run-L15-GPU2137-ShimNNNVmTest.py`、vmrun版）：Render21 Pass、BufProbe 4096保持、拒否0、vm3dum64_loaderのSHAとACLの復元。
   - **VMはVmMockなので、映像・Open・Aperture経路は通りません。** 合格は「既存の3D経路を壊していない」という意味だけです。
2. **実機配置**（§5.3）。
3. **同じ最小プローブで修正を確認**（クラッシュ → exit 0、プロファイル0件、postBltの変化など）。
4. **ユーザーが実アプリで確認**（カメラ、Obsidian、Codex CUAなど）。最終判定はこれです。
5. 文書化：`docs/`に症状・切り分け・原因（Ghidra名/RVA付き）・修正・SHA・rollback・未検証項目を書きます。memory（`l15-gpu-handoff-state.md`）の現行版を更新します。

> **VMの結果を実機の成功と扱わない。CLIのSUCCESSだけで成功としない。** 証拠ファイルの中身を自分で照合します。

---

## 7. Antigravity（Gemini）に任せた範囲

方針の出典：`docs/antigravity-routine-prompt.md`、memory `delegate-routine-to-flash`。

| 任せたもの（Gemini 3.8 Flash medium/medium または high/high） | 任せなかったもの（Claude/Codex） |
|---|---|
| 既存スクリプトを1本だけ実行する定型タスク（VM試験2件、Install/Rollbackの実行、Status取得） | ABI・構造体オフセットの決定、Ghidra解析、根本原因の判断 |
| ユーザーセッション（Session 4）での一回限りの対話タスク（Storeアプリ、`l15-immersive-window-probe.ps1`、CUAプローブ） | shimのソースの変更、ビルド成果物の採否 |
| KDNET起動など手順が決まった運用、ログの整理 | 実機に変更を入れるかどうかの判断、結果の判定 |
| 難しいABI・寿命のコードレビュー（Gemini 4 Argon high/high に限定） | 証拠の最終照合（CLI SUCCESSや空のresponseは未実施として扱う） |

- 依頼の型：**TASK 1件＝対象・実行する既存コマンド1つ・入力・新規の出力先・成功条件**。permissions.allowはそのコマンド1件だけをセッション限定で許可します。
- 資格情報、KDNETキー、無関係なログはプロンプトに入れません。資格情報はCredUIブローカー（`scripts/l15_argon_winrm_broker90.py`、メモリのみ）から子プロセスのstdinへ渡ります。brokerの`run`は`scripts/*.py`だけを実行できます。
- 結果は`denied_actions`とstderrまで確認し、証拠ファイルを自分で開いて照合します。
- 同じ対象（同じ実機・同じVM）への変更は、所有者を1つにします。並行させません。
- 実績：
  - Shim96期のVM試験2件（`Run-Antigravity-Routine96.py`）。
  - CUA Immersive02/03とKxCom Capture01/02のL15とVMへの配置・ロールバック。
  - Session 4での対話プローブ。
  - 解析とshimの改修はすべてClaude側で行いました。

---

## 8. ケース集（原因を特定した道筋）

| 版 | 症状 | 決め手になった観測 | 原因 | 修正 |
|---|---|---|---|---|
| Shim96 | Draw不可、BB_START先0 | Ghidraでパッチ関数の3分岐（`FUN_18001f2f0`）を比較 | SKU +0x48 bit12でVA前提の経路が選ばれていた | bit12クリア、ヘッダー+0x20移動 |
| Shim97→99 | EUが命令を取れない | KDでSBA DW15=0、Escape D16=15をshimが拒否 | Instruction Buffer Sizeが0。97ではページ数とバイト数を取り違えた | `0xFFFFF000`を返す |
| Scan118 KMD | Map無効、Swizzle拒否 | KMD Map DDIが`+0x8a8`=0で何もしない、`AcquireSwizzlingRange` | WDDM2前提のKMD | KMD 2箇所を恒久パッチ |
| Shim106/107 | DWMが4秒周期で落ちる、LogonUIが1行だけ | GMM Create Reject=11、スキャンアウト全面取得 | Depthフラグが残っていた、OpenResourceの私有データ形式の違い | blobからDepth除外、Open経路の差し替え |
| Shim108 | R/B反転 | `PLANE_CTL_A` bit20 | Primaryの形式が0x7e | 0x17へ |
| Shim111 | 背景の縦筋 | Lockトレース（Flags 0x0276） | AcquireApertureの線形窓がない | Y tileの線形シャドウ（detileとtile） |
| Shim120 | Obsidianで「WebGL非対応」 | BufProbeで250個で`8007000E`、`Reject:13` | 割り当て表が256枠固定 | チャンク確保 |
| Shim121–125 | カメラ黒画面 | Escape拒否 → node3拒否 → 読み戻しで無変化 → パッチのログでbit26全立ち | 映像UMDのパッチがbit26=1で、2137 KMDが全部スキップ | Escape 17/7翻訳、node3、`DriverId &= ~0x04000000` |
| Shim126 | CUAスクリーンショットでAV | プローブで「初期データ付きCreateTexture2Dのみ」と特定、ダンプで`rep stosb` | 5077はaux/CCSをmain面の後ろに置く。シャドウがMainSizeしかない | シャドウをByteSize、末尾は線形コピー |
| Shim127 | 録画停止後にカメラがクラッシュ | ダンプのスタックをスキャン（msmpeg2vdec→CreateVideoDecoder）、デコーダープローブで再現、cdbトレース2回 | VCS（node1）のコンテキストをshimが拒否 → Intelが`8007000E` → 二重解放 | デコーダープロファイル0件（ソフトウェアへ） |

共通する教訓：
- **最小プローブを作るまでが一番の近道です。** アプリで試行錯誤しないでください。
- 「成功したDDI」と「実際に書かれた画素」を分けて確認します。
- 1ビット・1オフセットの違いが原因であることが多いです。大きな再設計（GPU VAバックエンドの新規実装、PPGTTの新設）は、**比較解析で小さな差が見つからないときだけ**行います（Shim96、Shim125では、どちらも新規実装は不要でした）。

---

## 9. 安全・運用の規則（守ってきたこと）

- 原本、スナップショット、復旧経路を必ず保持します（S07の退避版、`Recovery118.ps1`、VMスナップショット）。BLOCKEDになった版は再導入しません。
- 秘密情報はモデル、argv、docs、ログ、gitに入れません。
- Defenderなどのセキュリティ設定は変更しません。アプリの同意（elicitation）を自動で承諾しません。
- 実機を変更する前に、VMで確認できる範囲を確認します。実機作業の後は通常起動・Display err=0・DWM稼働を確認してから返却します。
- コミットとプッシュはユーザーの指示があったときだけ行います（`scripts/git-push-main.py`、main→origin、`--no-tags`）。ダンプ、`out/*.exe`、cdbバイナリ、`scripts/gen`、解析ファイルはコミットしません。
- 電源：ACは必須ではありません。残量、試験時間、復旧の余裕で判断します（AGENTS.md 2026-11-04）。

---

## 10. 現状と残課題（2026-11-11時点）

- ライブUMD：**Shim127** `D4E930AE…D2E1`（退避：`igd10iumd64.shim126.dll`）。KMD Scan118系、igfx Start=3。
- 動作確認済み：D3D11描画、DWM、S3/S4、実輝度、WebGL（Supermium/Obsidian）、カメラ撮影と録画（ソフトウェアMF経由）、Codex CUAのスクリーンショット。
- 未対応・未検証：
  1. **ハードウェア動画デコード・エンコード（VCS、node 1）**。
     - 2137 KMDはノード番号をそのままエンジン番号に使います。ノード1・3・4はメディア系です（camera-diagnosis §ノード3解析）。
     - node 3と同じ32→96の私有データ変換で通る可能性はあります。ただし、MFXコマンドの翻訳とパッチの扱いは未確認です。
     - GPUハングやTDRの恐れがあるため、**KDNET下で最小デコーダープローブから**始めます。
     - 有効化した版では`l15-video-decode-allow.flag`で検証してから、既定の動作を変えます。
  2. Intelハードウェアエンコーダーの影響。`mfx_mft_h264ve`が録画に使われていたかは確認していません。
  3. X tile（Tile==1）のAcquireAperture、CDDのBlt Present（`C000000D`）、Escape 6（336B）のサブコード群、1時間以上の連続負荷、外部出力。

関連資料：`handoff-claude-code-20261105.md`（§7〜11.9）、`l15-camera-diagnosis-20261108.md`、`cua-l15-immersive-apps-20261110.md`（Shim126）、`l15-camera-record-crash-20261111.md`（Shim127）、`antigravity-routine-prompt.md`、`claude-kdnet-guide-20261106.md`。
