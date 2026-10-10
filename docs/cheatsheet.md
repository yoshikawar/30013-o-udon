# 本番当日の手順

Linux / WSL / macOS のシェルで、リポジトリのルートから実行する。標準・推奨は主 PC 単体実行です。LAN worker は、事前に preflight と実runを最後まで安定確認できた場合だけ使います。

本番中は、`git pull`、build、設定変更、無計画な再起動、手動POSTをしない。dirty worktreeのままLAN workerを使わない。workerが失敗しても主PCはlocal solverへfallbackするが、worker構成としては試合終了後に要調査です。

## 1. ターミナルを開いた直後

```bash
cd ~/30013-main-lan
git status --short
git rev-parse --short=12 HEAD
```

- LAN workerを使わない主PC単体実行では、使用するbuildと動作を事前確認済みであることを確認する。
- LAN workerを使う場合は、主PC・全workerが clean worktree かつ同じ commit であることを確認する。
- dirtyならworkerを外して主PC単体実行へ切り替える。
- detached HEAD 自体は問題ない。fingerprintだけで未commit差分の同一性は確認できない。

## 2. 環境変数を設定・確認

公式接続情報は主PCだけが持つ。workerには公式tokenを設定しない。

```bash
read -rsp 'PROCON_TOKEN: ' PROCON_TOKEN
printf '\n'
export PROCON_TOKEN

: "${VENUE_BASE_URL:?VENUE_BASE_URL is not set}"
: "${PROCON_TOKEN:?PROCON_TOKEN is not set}"
```

LAN workerを使う場合だけ、主PCとworker全台で同じworker secretを設定する。

```bash
read -rsp 'HEXA_LAN_WORKER_SECRET: ' HEXA_LAN_WORKER_SECRET
printf '\n'
export HEXA_LAN_WORKER_SECRET
```

- 単体実行では `HEXA_LAN_WORKER_SECRET` は不要。
- token / secretをコマンド履歴、Git、チャット、スクリーンショットへ出さない。
- workerは公式APIへ接続しない。

## 3. 新しいrun保存先を作る

```bash
RUN_ID="venue-$(date +%Y%m%d-%H%M%S)"
RUNTIME_ROOT="$HOME/hexa-runtime/$RUN_ID"
mkdir -p "$RUNTIME_ROOT/session" "$RUNTIME_ROOT/log"
```

- 過去runのsession directoryを再利用しない。
- 後続確認では同じ `RUN_ID` と `RUNTIME_ROOT` を使う。

## 4A. 主PC単体で実行する（標準・推奨）

```bash
set -o pipefail

./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" \
  --token-env PROCON_TOKEN \
  --max-get-retries 200 \
  --execute \
  --session-dir "$RUNTIME_ROOT/session" \
  --log-dir "$RUNTIME_ROOT/log" \
  2>&1 | tee "$RUNTIME_ROOT/log/client-output.log"
```

- `--execute` がないとPOSTしない。
- `--max-get-retries 200` は、開始前の403待機で既定値8回の `polling-limit` を避けるための推奨値。
- `waiting-for-match` は正常状態なので停止しない。
- `day0 precompute` 完了後も、試合開始までそのまま待つ。
- `result=Success` までターミナルを閉じない。

## 4B. LAN workerを使う場合（任意・条件付き）

次の条件をすべて満たす場合だけ使用する。

- 主PC・全workerが clean worktree かつ同一commit
- 主PC・全workerで同じworker secret
- 全workerのpreflightが成功
- 同一構成で実runを最後まで完走済み
- Wi-Fi、スリープ、省電力、Firewall、本番networkを確認済み

OS別設定、network、WSLミラーモードの詳細は [lan-worker.md](lan-worker.md) を参照する。ここでは本番構成と実行順を示す。

### 今回のworker構成

| worker index | 機器 | endpoint | worker起動時の指定 |
| --- | --- | --- | --- |
| 0 | Mac | `192.168.10.2:39001` | `--worker-index 0 --worker-count 2` |
| 1 | PC3 WSL（Windowsミラーモード） | `192.168.10.3:39001` | `--worker-index 1 --worker-count 2` |

主PCの `--lan-worker` は Mac（`.2`）→ PC3 WSL（`.3`）のindex順に指定する。順序がworker indexと異なると照合に失敗する。PC3はWSLミラーモードを前提とし、WSL内でも `192.168.10.3:39001` で待ち受ける。古いWindows portproxy設定が残っている場合は、事前に削除する。

worker起動 → 主PC preflight → 主PC auto の順に実行する。

### worker起動

Mac:

```bash
./build-release/hexa_udon worker \
  --listen 192.168.10.2:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 0 --worker-count 2 \
  --run-id "$RUN_ID" --worker-log "$HOME/hexa-runtime/$RUN_ID-worker-mac.jsonl"
```

PC3 WSL:

```bash
./build-release/hexa_udon worker \
  --listen 192.168.10.3:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 1 --worker-count 2 \
  --run-id "$RUN_ID" --worker-log "$HOME/hexa-runtime/$RUN_ID-worker-pc3.jsonl"
```

PC3で古いportproxy設定を削除する場合（管理者PowerShell）:

```powershell
netsh interface portproxy delete v4tov4 listenaddress=0.0.0.0 listenport=39001
```

### 主PCからpreflight

```bash
./build-release/hexa_udon worker-preflight \
  --listen 192.168.10.2:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 0 --worker-count 2

./build-release/hexa_udon worker-preflight \
  --listen 192.168.10.3:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 1 --worker-count 2
```

両方で `worker-preflight=ok` を確認する。出力の次を確認する。

- protocol version
- build fingerprint
- worker index / count
- evaluator identity
- profile identity
- `secretConfigured`

1台でもpreflightに失敗した場合、そのworkerは使わない。必要ならworkerをすべて外して主PC単体で実行する。

### worker付き主PC実行

```bash
set -o pipefail

./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" \
  --token-env PROCON_TOKEN \
  --max-get-retries 200 \
  --execute \
  --lan-worker 192.168.10.2:39001 \
  --lan-worker 192.168.10.3:39001 \
  --lan-worker-timeout-ms 55000 \
  --session-dir "$RUNTIME_ROOT/session" \
  --log-dir "$RUNTIME_ROOT/log" \
  2>&1 | tee "$RUNTIME_ROOT/log/client-output.log"
```

- `--lan-worker` は必ず `.2`、`.3` の順。
- `--lan-worker-timeout-ms 55000` はworkerのreply余裕を確保する設定。
- `warning=lan-worker` が出ても主PCはlocal solverへfallbackする。試合中にautoを再起動しない。

## 5. 試合中の見る場所

| 出力 | 意味と対応 |
| --- | --- |
| `waiting-for-match` | 正常。停止しない |
| `daily-start` | 当日の処理開始 |
| `post=solver-interim success` | OfficialScoreが厳密改善した途中候補を提出 |
| `post=solver success` | 最終候補を提出 |
| `daily-end ... score=[...]` | 当日の最終OfficialScore |
| `candidateSource=wait` | safe-waitが残った。試合は継続するが終了後に要調査 |
| `warning=lan-worker ...` | worker障害。主PCは継続し、終了後にworkerログを確認 |
| `polling-limit` | GET再試行上限。通常は `--max-get-retries 200` を確認 |
| `warning=daily-post-outcome-unknown` / `warning=safe-wait-outcome-unknown` | 日の提出が送れたか不明。止めずに次の提出で上書きするので、そのまま待つ。終了後に `operations.jsonl` で原因を確認 |
| `RecoveryRequired` | 種別の POST が不明など。同じsessionで再実行しない。状態確認へ進む |
| `result=Success` | 完走 |

公式順位用OfficialScoreは「総系列数 → 日別系列数合計 → 玉数」の順で比較される。回答時間を短縮するため、試合中にプロセス停止・手動提出をしない。

## 6. 異常時の判断

- worker preflight失敗: そのworkerを外して主PC単体へ切り替える。
- `warning=lan-worker`: autoを止めない。終了後に原因を調べる。
- token missing: auto開始前ならtokenを設定してから実行する。
- 日の提出の結果不明（`warning=...-outcome-unknown`）: autoは止まらずに続く。止めない。
- `RecoveryRequired`: 自動再送や同じsession directoryでの再実行をしない。`show-state` と公式状態を確認し、必要なら `recover` をdry-runで確認する。
- `polling-limit`: `--max-get-retries 200` の指定を確認する。ただし本番中に無計画な設定変更・再起動をしない。
- `result=Failed`: session/logを保全し、原因を確認してから次の判断をする。

## 7. 試合終了後

```bash
./build-release/hexa_udon show-state \
  --session-dir "$RUNTIME_ROOT/session"

grep -E 'daily-end|warning=|polling-limit|RecoveryRequired|result=' \
  "$RUNTIME_ROOT/log/client-output.log"
```

workerを使った場合は、主PC側のSessionに残るworker観測も確認する。

```bash
jq -r '
  .dailyPlanningDiagnostics[]?
  | .day as $day
  | .workerObservations[]?
  | [$day, .workerIndex, .adoption, (.rejectionReason // "")]
  | @tsv
' "$RUNTIME_ROOT/session/session.json"
```

- `worker`: worker候補を採用。
- `local-retained`: workerが応答したが主PC案を維持。
- `connect-failure`、`read-timeout`、`reply-eof`: worker通信失敗。
- run directoryは次の解析まで保全する。

## 8. ビルドとテスト（本番前のみ）

本番当日に行わず、事前に完了させる。

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
ctest --test-dir build-release --output-on-failure
./build-release/hexa_udon --help
```

`practice.sh` は既定で公式の試合（`http://172.28.0.10:8080`、`VENUE_BASE_URL` で変えられる）につなぎ、`--arena` で練習場につなぐ（token は `PROCON_ARENA_TOKEN`）。`--base-url URL`・`--wait 秒` 以外の引数はそのまま `auto` に渡すので、`./practice.sh --threads 8 --lan-worker 192.168.1.12:39001` のように付けられる。token は `.env`（`.env.example` をコピー）に書いておける。
