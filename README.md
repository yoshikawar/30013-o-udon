# hexa-udon

高専プロコン 2026 競技部門「ヘキサうどん」の C++20 クライアントです。公式 API との通信・Session の保存と復旧は o-udon の実装、種別決めと毎日の計画は solver（補給の時期と場所も計画に入れる LNS ＋ 焼きなまし）で行います。

## 1. ビルド

必要なもの: C++20 対応コンパイラ（GCC 13 で確認）、CMake 3.20 以上、nlohmann_json 3.11.3 以上、libcurl。

```bash
# Ubuntu / WSL
sudo apt install -y cmake nlohmann-json3-dev libcurl4-openssl-dev
```

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
ctest --test-dir build-release --output-on-failure
```

## 2. 実行

token と URL は環境変数で渡し、README や Git に実値を書きません。

```bash
export VENUE_BASE_URL='https://<venue-host>'
export PROCON_TOKEN='<official-token>'
```

接続を確認する:

```bash
./build-release/hexa_udon check --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN
```

試合に出る（`--execute` を付けたときだけ POST します。付けなければ dry-run）:

```bash
./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN --execute \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log
```

練習場（procon37arena.online）では `practice.sh` が build・待機・Session の退避・`auto --execute` をまとめて行います。

### 1 試合の流れ

1. `GET /setting` を受け取ったら、solver が種別（どの車を補給車にするか）を決めて `POST /agent` します。
   持ち時間は盤の大きさごとの締切（16×16 / 24×24 / 32×32 で 60 / 90 / 120 秒）までの残りから `--safety-seconds` を引いた分で、その 8 割で選びます（`--kind-ms` で上書きできます）。
2. 種別を出したら、締切（startsAt が分かればその時刻）まで別のスレッドで 1 日目を計画しておきます。1 日目の道路は全部空いていて、朝の状態も種別で決まるためです。1 日目はこの解から始めるので、1 日目の最初の提出から良い計画を出せます（回答時間が短くなる）。途中から入り直したときはしません。
3. 毎日、まず全員が待つ計画を出します（計画ができるまでの保険）。
4. solver が日の締切の `--safety-seconds`（既定 3 秒）前までの残り時間の 85% で計画し、系列・玉・翌日に効く項が良くなるたびに `--interim-ms`（既定 3000）ごとに出し直します。最後の計画が直前に出したものと同じなら出し直しません（回答時間で負けないため）。
   1 日の計画は、乱数の種だけを変えた焼きなましを `--threads` 本同時に回し、一番良い解を使います（種によって行き着く解が少し違い、良い方を選ぶと玉が増えるため。1 日 3 秒の試験で 8 本は 1 本より 1 試合 +3.9 玉、32×32 で +9 玉）。
5. 提出する計画はすべて、提出の前に solver のシミュレーター（公式ルールの再現）で確かめます。

### 主な option

| option | 意味 |
| --- | --- |
| `--execute` | POST する（無ければ dry-run） |
| `--types 0,0,0,1` | 種別を手で指定する（solver の種別決めをしない） |
| `--kind-ms N` | 種別決めの持ち時間（既定: 締切から自動） |
| `--interim-ms N` | 良くなった計画を出し直す間隔。0 なら最後の計画だけ |
| `--safety-seconds N` | 日の締切の何秒前までに提出を終えるか（既定 3） |
| `--threads N` | 1 日の計画を同時に回すスレッドの数（既定: 論理スレッド数の半分。全部使うと通信や OS の処理が遅れるため） |
| `--seed N` | LAN worker に渡す乱数の種のもと |
| `--session-dir DIR` / `--log-dir DIR` | Session と OperationLog の置き場所（既定 `run/session`, `run/log`） |

`hexa_udon --help` で全 option を表示します。

## 3. LAN worker（任意）

別の PC（または同じ PC の別プロセス）で solver を乱数の種を変えて回し、主 PC の計画より公式の点（総系列 → 日別系列 → 玉）が高ければそれを使います。worker は公式 API・token に触れず、worker 専用 secret で loopback / private LAN だけで通信します。

```bash
export HEXA_LAN_WORKER_SECRET='<worker-only-secret>'
# worker
./build-release/hexa_udon worker --listen 127.0.0.1:39001 --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 0 --worker-count 1
# 起動を確かめる
./build-release/hexa_udon worker-preflight --listen 127.0.0.1:39001 --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 0 --worker-count 1
# 主 PC
./build-release/hexa_udon auto --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN --execute \
  --lan-worker 127.0.0.1:39001
```

worker は日の締切の 1 秒前（`--lan-worker-timeout-ms` で上限を指定可）まで、自分の `--threads` 本で計画します。返ってきた計画は、依頼と同じ条件で解いたか（requestId、入力、盤、乱数の種、build、点など）を照合し、主 PC のシミュレーターで確かめてから比べます。照合に失敗した・時間切れの worker は使いません。

複数の PC をつなぐ手順（Windows は WSL、Mac、ネットワークとファイアウォールの設定）は [docs/lan-worker.md](docs/lan-worker.md)、よく使うコマンドは [docs/cheatsheet.md](docs/cheatsheet.md) にまとめています。

## 4. 異常時と復旧

- POST の前に失敗した提出は `submissionAttempted=false`、POST したが結果が分からないものは `null` として Session に残し、`null` が出たら `RecoveryRequired` で止まります（自動で送り直しません）。
- `RecoveryRequired` のあとは同じ Session で `auto` を再実行せず、`show-state` と公式の状態を照らし合わせてから `recover`（既定は dry-run）を使います。

```bash
./build-release/hexa_udon show-state --session-dir /secure/runtime/session
./build-release/hexa_udon recover --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log
```

- Session / log / token / secret / build 成果物は Git に入れません。Session と log の directory は 0700、ファイルは 0600 で作ります。

## 5. 実装の場所

| 領域 | 場所 |
| --- | --- |
| solver の型と宣言 | `include/hexa_udon/solver.hpp` |
| 問題（大域変数）と o-udon の型との変換 | `src/core/problem.cpp` |
| 1 日のシミュレーション（公式ルールの再現） | `src/simulator/simulator.cpp` |
| 最短経路（最速 / 燃料最小） | `src/pathfinding/router.cpp` |
| 1 日の解（訪問の並びと補給）の組み立てと評価 | `src/planner/day_planner.cpp` |
| 1 日の計画（LNS ＋ 焼きなまし） | `src/optimizer/optimizer.cpp` |
| 種別決め | `src/planner/prematch_type_selector.cpp` |
| 試合の流れ・提出・LAN worker の採否 | `src/app/auto_client.cpp` |
| LAN worker | `src/app/lan_worker.cpp` |
| HTTP・rate limit・Retry-After | `src/protocol/` |
| Session・Recovery・polling・永続化 | `src/session/` |
