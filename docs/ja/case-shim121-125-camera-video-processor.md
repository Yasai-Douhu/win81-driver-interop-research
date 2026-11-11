# L15 実機8.1 カメラのエラー調査（2026-11-08）

結論: カメラ本体、usbvideo、KS、Media Foundationの映像取得は正常でした。失敗しているのはGPUの映像処理です。研究用GPUドライバー（Intel UHD Graphics - L15 2137 research 0.2.3、`oem22.inf` 31.0.101.6140）で、D3D11の `ID3D11VideoDevice::CreateVideoProcessor` が `0x8007000E`（E_OUTOFMEMORY）を返します。カメラアプリが使うWinRT MediaCaptureは、撮影時にこのGPU映像処理を使うため、同じ `0x8007000E` で失敗します。

## 切り分け（すべて実機。読み取りと試験撮影のみで、設定は変更していない）

| 層 | 試験 | 結果 |
|---|---|---|
| デバイス | `USB\VID_5986&PID_212B&MI_00` Integrated Camera | Status OK、エラー0、`usbvideo.inf` 6.3.9600.17217（Microsoft）。KSCATEGORY_CAPTURE/VIDEO/VIDEO_CAMERAのインターフェースは登録済み。プライバシー設定（DeviceAccess Global）はAllow |
| WinRT列挙・初期化 | `scripts/l15-camera-capture-probe.ps1` | `DeviceInformation` で1台を検出。`MediaCapture.InitializeAsync` は成功。形式はMJPG×9、YUY2×9 |
| WinRT撮影 | 同上 `CapturePhotoToStreamAsync`（JPEG/BMP） | **YUY2 320×240でもMJPGでも `0x8007000E`**。MJPGデコーダー（mfmjpegdec 6.3.9600.18617）は無関係 |
| MF（GPUなし） | `scripts/l15-camera-mfreader-probe.ps1` SourceReader、D3Dマネージャーなし | YUY2 640×480を15回読み、14フレーム取得（614,400 bytes/枚）。ADVANCED_VIDEO_PROCESSING有効時も同じく成功（CPU処理） |
| D3D11の基本 | `scripts/l15-d3d11-video-probe.ps1` | HWデバイス（FL 11.0、VIDEO_SUPPORT）は作成可。NV12/YUY2/BGRAのテクスチャ（通常、SHARED、DECODER bind）も作成可。`ID3D11VideoDevice` と `ID3D10Multithread` も取得可 |
| D3D11の映像処理 | `scripts/l15-d3d11-vp-probe.ps1` | Enumeratorの作成は可。NV12/YUY2/BGRAは入出力とも対応（flags=0x3）、Caps device=0x1E、maxIn=8。**`CreateVideoProcessor` → `0x8007000E`** |

上の試験はWinRMのsession 0で実行しました。ユーザーのカメラアプリ（Microsoft.MoCamera 6.3.9600.16384、17:57起動）が表示したエラーの種類は確認していません。ただし、同じAPIが同じHRESULTで失敗することは確認しています。

## 根本原因（2026-11-08 解析）

`l15-d3d11-vp-probe.ps1` で、プロセス単位のshimログ（`L15_GPU2137_SHIM92_LOG`）を有効にして実機で実行しました。`CreateVideoProcessor` の最中にShim120が `Shim92EscapeRejected Bytes 80 D0=64 D8=17 D16=7` を記録し、`E_NOTIMPL` を返していました。これが `0x8007000E` として返っています。

| 項目 | 5077 KMD（UMDが想定する相手） | 2137 KMD（実機で動いているもの） |
|---|---|---|
| Escape 17の入口 | `FUN_14010a180`（表 `DAT_140a7d300`[17]） | `FUN_1403d9d80`（表 `DAT_1429ecd60`[17]） |
| 必要なパケットサイズ | 0x50（80バイト） | **0xE0（224バイト）**。ヘッダーd0はサイズ−16 |
| サブコード表 | adapter+0x1550 → +0x77840、13個 | +0x3c38 → +0xb08b00、19個 |
| サブコード7の処理 | `FUN_14010a3b0` | `FUN_1403da020` |
| 処理内容 | 名前付きイベント `\BaseNamedObjects\IGFXKMDNotifyBatchBuffersComplete` を呼び出し元プロセスへ `ZwDuplicateObject` し、+0x18に返す。+0x20は対象プロセス（-1）、+0x14は状態 | 同じ。オフセットも同じ |
| 前提 | adapter+0x140 bit 0x2000000 | adapter+0x8c bit 4、hContextが必要 |

- 2137 KMDはバッチ完了時に、このイベントを `KePulseEvent` で通知します（`FUN_140461140`）。通知されるのは、エスケープで受け取ったプロセスが1つ以上あり（+0xb121bc>1）、かつ送信経路が+0xb121b8を立てた場合だけです。
- つまりUMDとKMDの機能は対応しています。食い違っているのはパケット長（80バイトと224バイト）だけです。Shim120はこのエスケープを知らないため拒否していました。
- 5077 KMDは、複製に失敗しても新しいUMDには成功を返し、ハンドルを−1にします。

## Shim121（作成・VM回帰試験済み、実機未適用）

- `patches/src/l15-gpu2137-umdshim121`：Shim120に、`sz==80 && d0==64 && d8==17 && d16==7` の処理を1つだけ追加しました。80バイトを0xE0バイトへ詰め直し（d0=0xD0）、実KMDへ転送したあと、+0x14以降を80バイトのパケットへ書き戻します。KMDが拒否した場合と、VmMockの場合は、5077 KMDと同じ形（成功、ハンドル−1）で返します。結果はshimログに `Shim121Escape17Sub7` として、HContext、HRESULT、KmdStatus、Fallback、Handleを記録します。
- ビルド：`scripts/Build-L15-GPU2137-UmdShim121.py`（VS2013 /W4 /WX）。DLL `4D97BC00B149B0875A230CD827688424A2C33E81EF5B965804601F634A2BF15C`。
- VM回帰試験：`scripts/Run-L15-GPU2137-Shim121VmTest.py reg1 121` で合格しました。Render21はPass、BufProbeの5種類はすべて4096個を保持、拒否0、vm3dum64_loaderのSHAとACLは復元済み、表示err=0です（`analysis/l15_data/camera-vp-20261108/shim121-vm-reg1/`）。
- VMではvideo processor経路を検証できません。VmMockでは、5077 UMDの映像経路が `CreateDevice` 直後（GetCaps 137の後）にプロセスごと落ちます。Escape 17に到達する前で、Shim120でも同じでした（vp1〜vp3）。vp3はプロセス内で実行したため後始末が走らず、VMをスナップショット `Pre-MSIX81-Unified221-t01` へ戻しました（`vp3-fetch/fetch.json`）。このためVP探査は既定で無効にし、引数 `vp` を付けたときだけ子プロセスで実行するようにしました。
- 実機への適用：`scripts/Deploy-L15-GPU2137-Shim121.py install`。Shim120と同じく、KDNET Run下で実行します。Shim120は `S07\igd10iumd64.shim120.dll` に保持され、`rollback` で戻せます。適用後は `l15-d3d11-vp-probe.ps1` で `CreateVideoProcessor` とBltを確かめ、続いてカメラアプリで撮影を確認します。
- 未確定：KMDの前提ビット（+0x8c bit 4）が実機で立っているかは不明です。立っていなければKMDは拒否し、Fallback（ハンドル−1）になります。その場合にUMDが映像処理を続けられるかは、実機で確認するしかありません。`CreateVideoProcessor` の後に、別の未対応エスケープが出る可能性もあります。

## 実機適用（2026-11-08 18:50〜18:58）

- 手順：KDNET Run `7bde2079` を起動（Power79、Ready、bugcheckなし）→ `Deploy-L15-GPU2137-Shim121.py install` → `dwmtest`（DWMを968から1704へ再起動、表示err=0）→ `Stop-L15-KDNET-Observer.py` で通常起動に戻しました。
- 戻した後の状態：起動は18:55:26、BCDにdebugの行はありません。S07のUMDはShim121（`4D97BC00…F15C`）、表示err=0、DWMは動作中、電池は99%（AC）です（`analysis/vxkex-merge-2466/physical/script-l15-boot-state-s121-185816.json`）。Shim120は `S07\igd10iumd64.shim120.dll` に保持しており、`rollback` で戻せます。
- `l15-d3d11-vp-probe.ps1`（`script-l15-d3d11-vp-probe-s121-185247.json`）の結果：
  - **Escape 17/7は成功しました**：`Shim121Escape17Sub7 HContext=true HRESULT=0 KmdStatus=0 Fallback=false Handle=0x770`。2137 KMDの前提ビットは立っており、実際のイベントハンドルが返りました。
  - **それでも `CreateVideoProcessor` は `0x8007000E` のままです**。新しい停止点は、その直後の `Shim92CreateContextUnsupported Node=3 Affinity=0 Flags=0` です。5077 UMDは、video processor用にGPUノード3（映像エンジン、VEBoxと推定）のコンテキストを作ろうとします。shimは、ノード0（3D）の32→96バイト変換しか実装していません。
- カメラはまだ使えません。Shim121は、ノード0の動作と表示に関してはShim120と同等です（VM回帰試験合格、実機のDWM再起動も正常）。

## 次の課題：映像エンジン（ノード3）のコンテキスト

1. 5077 UMDのノード番号と、2137 KMDのノード番号の対応を調べます。2137 KMDのノード数とエンジンの並び（QueryAdapterInfoのノード表）を確認し、5077がいう「3」が2137のどのノードに当たるかを確定させます。
2. ノード3用のコンテキスト私有データ（5077の32バイト → 2137の96バイト）を変換します。ノード0の `priv96[0x24]=1 / [0x54]=3` に当たるエンジン別の値は、2137 KMDのCreateContext（`FUN_1403751e0` 付近）から決めます。
3. 映像エンジンのコマンドバッファの変換（`AdaptCommand5077` はノード0の3D命令とpatch list用）と、VEBox/SFC命令内のGPUアドレスの扱いを決めます。
4. VMのVmMockでは映像経路を検証できません（上記のとおりプロセスが落ちる）。毎回、実機での試行が必要です。

規模は、Shim95〜99で3Dの描画を通したときと同程度の新規作業と見込みます。

## ノード3解析とShim122（2026-11-08 19:00〜19:14）

- 2137 KMDの解析：CreateContext（`FUN_140350320` → `FUN_1403541c0` → `FUN_140352eb0`）は、UMDから渡されたノード番号をそのままエンジン番号（ctx+0x40）として使います。エンジン表 `FUN_1403664c0` は0〜0xCで、ノード1・3・4は同じメディア系の扱いです。私有データ+0x54を調べるのはノード5のときだけです。
- `patches/src/l15-gpu2137-umdshim122`：Shim121に次を追加しました。
  - ノード3（VECS）を、ノード0と同じ32→96バイト変換で通します。
  - コンテキスト表を8枠にしました。
  - CreateContextとRenderのログにNodeを記録し、ノード3の32バイトの私有データは16進で記録します。
  - DLL `EB407380164A11F68843B18050C33BF08061418C7605547DBCACCA3098998297`。VM回帰試験は合格です（`shim122-vm-reg1`：Render21 Pass、Hold×5=4096、拒否0、復元一致）。
- 実機（KDNET Run `e98d74d3`、bugcheckなし）：
  - `l15-d3d11-vp-create-probe.ps1` で **`CreateVideoProcessor` が S_OK** になりました。ログは `Shim92CreateContext Node=3 HRESULT=0` です。ノード3の私有データは `…00000000 6012/E806 0000 00000001 …` で、+0x0Cは毎回変わり、+0x13=1です。
  - 新たに3件の拒否が出ています：Escape 6（336バイト、d0=320）のサブコード8・2・3。作成は止まりません。
  - Bltを含む `l15-d3d11-vp-probe.ps1` では、`VideoProcessorBlt` が3つの入力形式すべてで `0x26ABDCD0` を返しました。HRESULTとしては意味のない値です。ノード3へのRender（コマンド送信）は1件も記録されていません。Bltで画素が実際に変換されたかは、まだ確かめていません。
- 撤収：実機は `Deploy-L15-GPU2137-Shim122.py rollback` でShim121へ戻しました。Bltの動作が分からないまま、日常のアプリが映像処理の経路に入らないようにするためです。そのあと通常起動へ戻しました（19:10:58起動、debugなし、Shim121、表示err=0、DWM動作中、電池99%）。

### 次の確認事項

1. Bltの戻り値 `0x26ABDCD0` の正体：プローブの呼び出し方の誤りか、5077 UMDが返している値かを切り分けます。入力に既知の値を入れ、Flushして出力を読み戻す確認が必要です。
2. Escape 6（336バイト）のサブコード2・3・8の意味：5077 KMDとの対応を解析します。
3. ノード3へ実際にコマンドを送るときの変換（`AdaptCommand5077` がVECSのバッファにも当てはまるか）を確かめます。

## 修正の方向（初回報告時点）

1. **GPUドライバーの修正（本命）**: 研究用ドライバー0.2.3のユーザーモードドライバーで、video processorの作成が失敗しています（DXVA-HD／D3D11 VPのCreateVideoProcessorDevice経路）。cdbで `CreateVideoProcessor` の内側を追い、失敗を返している関数とその条件を特定します。GPU goalは現在一時停止中のため、再開の判断が必要です。
2. **代替（ドライバーを触らない）**: D3D映像処理を使わず、CPU経路で動くデスクトップアプリ（DirectShowやMF SourceReaderの系統）なら使える可能性があります。ストアのカメラアプリとMediaCaptureを使うアプリは、1が解決するまで動きません。
3. 標準のMicrosoft Basic Display Adapterに戻すと、D3D映像処理が提供されないため、MediaCaptureの別の経路になります。この方法は試していません。GPU研究の状態を壊すため推奨しません。

## 2026-11-08 19:25 Shim122 実機再試験（KDNET下）
- `VideoProcessorBlt` の `0x26ABDCD0` はプローブの誤りでした。vtable番号55は `EncryptionBlt` で、正しい番号は53です。`l15-d3d11-vp-probe.ps1` を修正しました。
- 修正後の結果：NV12・YUY2・NV12-decoder の各Bltが `S_OK` を返し、ノード3へのRenderも `S_OK` で送信されました。TDRやbugcheckは出ていません。
- `l15-camera-capture-probe2.ps1`（BMPの画素統計を追加した版）の結果：
  - 4件の撮影すべてが成功しました。以前の `0x8007000E` は解消しています。
  - ただし、BMPはどれも全画素0でした（mean=0、sd=0、distinct=1、alphaも0）。出力面に何も書き込まれていない状態で、**カメラ映像はまだ正常ではありません**。
  - 撮影中に、Escape 6（336バイト）のサブコード2・3・4・8が計20回拒否されています。
- 試験後の状態：実機はShim121に戻し、KDNETを停止して通常起動に戻しました（19:30起動、debugなし、Display err=0）。Shim122はローカルにだけ残しています。

## 2026-11-08 21:45 根本原因の確定 — VP出力面はGPUに書かれていない
Opus 4.8 で読み戻しプローブ `scripts/l15-d3d11-vp-readback-probe.ps1` を作成し、Shim123下・KDNET Readyで実機検証しました（証跡 `analysis/vxkex-merge-2466/physical/script-l15-d3d11-vp-readback-probe-rb1-214501.json`）。

**手法**: BGRA出力面を既知色 (R=0.2,G=0.4,B=0.6,A=1.0 → BGRA 153,102,51,255) で `ClearRenderTargetView`。ステージングへ `CopyResource`+`Map` して読み戻し（preBlt）。次に非ゼロのYUY2入力で `VideoProcessorBlt` を実行し、再度読み戻し（postBlt）。

**結果（決定的）**:
- preBlt pixel0 BGRA = `153,102,51,255`, mean=140.6（クリア色を正しく読み戻せた＝読み戻し経路は正常）。
- Blt は `S_OK`。
- postBlt pixel0 BGRA = `153,102,51,255`, mean=140.6 — **preBltと1バイトも違わない**。
- ノード0・ノード3のRenderはすべて `S_OK`、TDRもbugcheckもなし（KDはReadyのまま）。

**確定した結論**:
1. `VideoProcessorBlt` は全DDIが成功を返し命令も投入されるのに、**GPUは出力サーフェスを一切書き換えていない**（実質ノーオペ）。
2. カメラの黒画像は、MediaCaptureが用意するゼロ初期化の出力面にVPHALが何も書かないため mean=0 のまま残る、という同一現象です。本プローブでは事前にクリアしたので「クリア色のまま」として同じ事実が見えました。
3. **Argon候補B（Lock/TileYの読み戻し経路）は否定**されました。読み戻しは正常です。Escape6とProductFamilyも前節で否定済みです。
4. 残る整合的な説明は、既知の未解決事項（`docs/next-steps.md` 項目5）です。5077 UMDは本来 `FUN_18087f6a0` でGMMの `+0x478/+0x470` からGPU VAを命令バッファへ直接埋めますが、現ブリッジではこのVAが0のままです。命令は投入・「完了」されますが、surface-state/出力先が実アロケーションの実VAを指さないため、EUカーネル/コピーエンジンは走っても実際の出力面には何も書かれません。3Dドロー（phys95s4）はこれでTDRしましたが、VECS/コピー経路はTDRせず「成功」扱いで完了する点だけが異なります。

**次の一手（Shim124方針）**: GPU VAバックエンドの実装、すなわち命令バッファ/パッチが参照するサーフェスの実GPU仮想アドレスを、GMMまたはKMDのマッピングから取得して正しく埋める経路の設計です。架空VAは禁止で実マッピングが必要、という既存の制約がそのまま最大の課題として残ります。

**実機状態**: Shim121へロールバック、KDNET停止、通常起動（21:47、debugなし、Display err=0、電池99%）。

## 2026-11-08 22:10 修正着手：GPU VAバックエンドの設計分析（Shim124）
Ghidra で 5077 3D UMD (`igd10iumd64.dll`) のアドレッシング方式を解析しました。

- `FUN_18087f6a0(param_1)`：GMMリソース情報の `+0x478`（0なら`+0x470`）を読み、`(va<<0x10)>>0x10`（下位48bit）を返す。= **アロケーションごとのGPU仮想アドレスをGMMリソースから直接取得**する関数。
- この関数は **11か所から呼ばれ**、戻り値を命令バッファへ直接埋め込む。つまり5077 UMDは「永続GPU VA（ソフトピン）を命令に直接書く」方式。
- UMD内に GMM `+0x470/+0x478` への正当な書き込みは**存在しない**（`+0x470/+0x478` ストアの全件は、CRTロケール内部とディスパッチテーブルのオフセット偶然一致で、GMMリソースとは無関係）。→ このVAは **UMDの外（ネイティブ5077のKMDまたはGMM）が設定**している。
- 現ブリッジ（5077 UMD → 2137 KMD / Win8.1 dxgkrnl WDDM1.3）では、このVAを誰も設定しないため **0のまま**。GPUはVA0を参照し、出力面に何も書かない。これが黒画像の機械的原因（前節の読み戻し実証と一致）。

### 設計上の分岐点（ここはユーザー判断が要る）
WDDM1.3 の dxgkrnl は本来、命令バッファ内のアドレスを **PatchLocationList で提出時にパッチ**する方式（永続VAを前提にしない）。一方5077 UMDは永続VA（ソフトピン）前提で直接埋め込む。両者の整合に2つの道があります。

- **方式A：2137 KMDにソフトピンVAを割り当て・報告させ、GMM `+0x470/+0x478` に実VAを書く。** 5077 UMDの設計に最も忠実。ただし2137はWin10(WDDM2.x)KMDで、Win8.1 dxgkrnl(1.3)はGpuMmu/永続VA経路を駆動しない。KMDが8.1配下で永続VAを返せるか、専用Escapeの有無をKMD側で確認する必要がある。未確認で、使えない可能性がある。
- **方式B：UMDが直接埋めるVA箇所を、提出時にWDDM1.3のパッチエントリへ変換する。** `FUN_18087f6a0` 由来の埋め込みオフセットを特定し、対応アロケーションを参照する PatchLocationList を shim 側で生成して dxgkrnl にパッチさせる。どの埋め込みがどのアロケーションかの対応付けが課題。確認には提出1回分の命令バッファ＋パッチリストのダンプ解析が要る。

**次アクションの候補**：方式Bの可否判定のため、1回の Blt 提出について「`FUN_18087f6a0` が書く命令バッファ内オフセットが、既存 PatchLocationList で覆われているか」を確認する解析が決定的。覆われていれば dxgkrnl が正VAで上書きするはずで別原因、覆われていなければ方式Bが本筋。

## 2026-11-08 22:25 修正方針の確定 — パッチDriverIdのbit26変換（方式B、想定より小さい）
`FUN_18007e9e0` と 2137 KMD パッチルーチン `FUN_14045fcc0` の解析で、修正は「GPU VAバックエンドの新規実装」ではなく**パッチリストの変換**で足りる見込みと判明しました。

### 事実
1. 5077 UMD の BB_START 発行（`FUN_18007e9e0` のWDDM1.3分岐）: オペコード `0x18800500`(MI_BATCH_BUFFER_START)を書き、直前にアロケーション追加・パッチ追加のコールバックを呼び、`FUN_18087f6a0` のGPU VA(=GMM由来、現状0)を直接埋める。
2. 2137 KMD パッチルーチン `FUN_14045fcc0`: パッチエントリは24バイト（6×uint）。`DriverId`(entry[2]) の **bit26(0x04000000)=0 のときだけパッチを実行**。実行時は `FUN_1403a7960(adapter, alloc, ...)` で**そのアロケーションの実GPU VAを解決**し、`AllocationOffset`(entry[3]) を足して `PatchOffset`(entry[4]) 位置へ書く（`0x600000` ビットで64/32bit切替）。
3. 5077 UMD は全パッチを **bit26=1** で出す（5077 KMDでは「埋め込み済み＝スキップ」の意味で、直接VAが有効だったため正しかった）。2137 KMDではこれが全スキップされ、直接VA(0)のまま残る。

### 結論
**2137 KMD は提出時に実GPU VAを解決できる**（架空VA不要・実マッピングの要件を満たす）。必要なのは、shim の Render パスで 5077 形式パッチの `DriverId` bit26 を落とし（必要なら 64/32bit幅ビットも整合）、2137 KMD にパッチさせること。BB_STARTや各サーフェスアドレスが実VAで埋まり、GPUが正しい出力先へ書くはず。

### Shim124 実装計画
1. まず**読み取り専用でパッチ実体をログ**：Render時に各パッチの `AllocationIndex/DriverId/AllocationOffset/PatchOffset` を記録し、bit26の分布と対象オフセット（BB_STARTのアドレス欄=命令バッファ内オフセットに合致するか）を確認する（shimへの低リスクな追記。メモリ改変なし）。
2. 確認後、Renderパスで該当パッチの `DriverId &= ~0x04000000`（および幅ビットの整合）を適用。`AdaptCommand5077` のヘッダー移動(+0x20)に伴う `PatchOffset += 0x20` は既存のまま。
3. VM回帰 → 実機KDNETで読み戻しプローブ（postBltがクリア色から変化するか）→ カメラプローブ。TDR/bugcheck監視。巻き戻しはShim121。

### 未確認（実装前に要確定）
- 5077 UMD が付けるパッチの `DriverId` 正確なビット構成（bit26以外に幅/種別ビットが混在するか）。→ 計画1のログで確定。
- 直接埋め込みVA(0)とパッチの二重経路が競合しないか（パッチが後勝ちで上書きするなら問題なし）。

## 2026-11-08 22:12 実機でパッチ実体を採取 — bit26がビデオ経路だけ全立ち（Shim124ログ版）
Shim124（Renderパスに読み取り専用パッチログを追加しただけの版、DLL `051FF17E…`）を実機KDNET下で配備し、読み戻しプローブを実行（`script-l15-d3d11-vp-readback-probe-p124-220914.json`）。postBltは依然クリア色のまま＝出力未書き込み（想定どおり、ログ版は挙動不変）。

採取したパッチ実体（`Shim124PatchSummary/Patch`）：
- **ノード0（3D/RCS）レンダー**: `Bit26Set=0`。DriverId例 0x00000000 / 0x000C0000 / 0x00000006 / 0x000087FA / 0x0000C7CE。2137 KMD `FUN_14045fcc0` はこれをパッチする。→ 3Dは正常。
- **ノード3（VECS/ビデオ）レンダー**: `Bit26Set` が全件（20/12/13/14）。DriverId例 0x0400CFFA / 0x0400CFEA / 0x04000016 / 0x04000002 / 0x04002002 / 0x0400C7CA / 0x0400C7CE / 0x04000000。**全パッチで bit26(0x04000000) が立つ** → 2137 KMD は全スキップ → ビデオのBB_START/サーフェスVAが一切パッチされず0のまま → 出力面に書かれない。

ビデオのDriverIdは3D側の種別値に `0x04000000` を付けただけ（例 0x0400C7CE ↔ 0x0000C7CE、0x04000002 ↔ 0x00000002、0x04002002 ↔ 0x00002002）。幅ビット `0x600000` は全件0＝64bitパッチ。

**確定した修正**: Renderパスで、bit26が立っているパッチの `DriverId &= ~0x04000000` を適用する（3Dは元々bit26=0なので無影響、ビデオだけが対象）。これで2137 KMDが実GPU VAをパッチし、VECS/SFCが正しい出力先へ書くはず。Shim125として実装・KDNET下で読み戻しプローブ検証（postBltがクリア色から変化するか）。

## 2026-11-08 22:15 修正成功 — Shim125（bit26クリア）で実機カメラが動作
Shim125（= Shim124のログ + Renderパスで `DriverId &= ~0x04000000`、DLL `0C427F85E78DE5C9D2D9693F891368F22A9147151C6EF4197644C3312938D528`）を実機KDNET下で配備し検証。

- **読み戻しプローブ（p125）**: preBlt `153,102,51,255` → **postBlt `114,147,255,255` mean=192.7** にクリア色から変化。VideoProcessorBltが実機で出力面を書くようになった。
- **カメラ撮影プローブ（p125、`l15-camera-capture-probe2.ps1`）**: 4件すべて成功し、**BMP画素が実画像**に：
  - YUY2 320x240 → BMP: mean=76.1, **sd=47.2, distinct=212**（以前は0/0/1）
  - MJPG 640x360 → BMP: mean=154.8, **sd=74.4, distinct=234**
  - JPEGも 11231 / 40540 バイトと実画像相当（以前は 1827 / 4307）。
- **安定性**: TDR/bugcheckなし。KDは終始Ready。dwmtestでDWMをShim125下で再起動しても `err=0`・正常。
- **実機最終状態**: KDNET停止、通常起動（22:17、debugなし、Display err=0、電池99%）。**ライブUMDはShim125（`0C427F85…`）のまま** = 再起動後もカメラ動作。復旧用に `igd10iumd64.shim121.dll`（Shim121）を退避保持。

### 根本原因（最終）
5077ビデオUMD(VPHAL/VECS)は全パッチを `DriverId` bit26(0x04000000)=1（ソフトピン＝埋め込み済み・パッチ不要の意味）で出すが、埋め込むGPU VAはGMM由来で現ブリッジでは0。2137 KMD `FUN_14045fcc0` はbit26=1を全スキップするため、ビデオのGPU VAが0のまま残り、VECS/SFCが出力面を書けなかった。3D(node0)はbit26=0で正常だった。修正はshimのRenderパスでbit26を落とし、2137 KMDに実VAを解決・パッチさせること。

### 新規ファイル
- Shim124（ログ版）: `patches/src/l15-gpu2137-umdshim124/`、`scripts/Build-/Deploy-L15-GPU2137-UmdShim124.py`
- Shim125（修正版）: `patches/src/l15-gpu2137-umdshim125/`、`scripts/Build-/Deploy-L15-GPU2137-Shim125.py`
- 読み戻しプローブ: `scripts/l15-d3d11-vp-readback-probe.ps1`
- `scripts/Run-L15-GPU2137-Shim121VmTest.py` は 124/125 を許可。
