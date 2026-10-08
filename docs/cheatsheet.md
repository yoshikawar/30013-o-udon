# よく使うコマンド

Linux / WSL / macOS のシェルで、リポジトリのルートで実行する。token・secret は環境変数で渡し、画面やファイルに書かない。

## ビルドとテスト

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
ctest --test-dir build-release --output-on-failure
./build-release/hexa_udon --help
```

- 初めての PC では先に依存を入れる（Ubuntu / WSL: `sudo apt install -y cmake nlohmann-json3-dev libcurl4-openssl-dev`、macOS: `brew install cmake nlohmann-json curl`）
- `git pull` したら `cmake -S . -B build-release ...` からやり直す（build の版 = その時点の commit が埋め込まれ、LAN worker の照合に使われる）

## 練習場（procon37arena.online）

```bash
export PROCON_TOKEN='<練習場の token>'
./practice.sh
```

`practice.sh` は build（`build/`）→ `/setting` が出るまで最大 180 秒待つ → 前の試合の Session を `run/session-previous-<時刻>` に退避 → `auto --execute` を実行する。出力は `run/client-output.log` にも残る。1 試合ごとに起動し直す。

スレッド数を変える・LAN worker を使うときは `practice.sh` の `run_client` の行に `--threads N` や `--lan-worker HOST:PORT` を足す。

## 本番

```bash
export VENUE_BASE_URL='https://<会場のサーバー>'
export PROCON_TOKEN='<本番の token>'

./build-release/hexa_udon check --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN   # 接続確認
./build-release/hexa_udon auto --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log                     # dry-run（POST しない）
./build-release/hexa_udon auto --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN --execute \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log                     # 本番
```

よく付ける option:

| option | 使いどころ |
| --- | --- |
| `--threads N` | 1 日の計画のスレッド数（既定は論理スレッド数の半分） |
| `--kind-ms N` | 種別決めの持ち時間を指定する（回戦の締切と盤の大きさが合わないとき。例: 1 回戦の 32×32 は締切 60 秒なので `--kind-ms 55000`） |
| `--types 0,0,0,1` | 種別を手で決める |
| `--lan-worker HOST:PORT` | LAN worker を使う（何台でも繰り返して書く。[lan-worker.md](lan-worker.md)） |
| `--safety-seconds N` | 日の締切の何秒前までに提出を終えるか（既定 3） |

## 試合のあとに見る・止まったとき

```bash
./build-release/hexa_udon show-state --session-dir run/session                  # 日ごとの系列・玉・最後の位置と燃料
grep -E 'daily-end|warning|RecoveryRequired|result=' run/client-output.log      # 日ごとの結果と警告
./build-release/hexa_udon recover --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --session-dir run/session --log-dir run/log                                   # 復旧の確認（既定は dry-run）
```

- `RecoveryRequired` で止まったら、同じ Session で `auto` をやり直さない。`show-state` と公式の状態を見比べてから `recover` を使う
- `unknown token` / 認証エラーは token の入れ間違いか、token が変わった

## LAN worker（くわしくは [lan-worker.md](lan-worker.md)）

```bash
export HEXA_LAN_WORKER_SECRET='<worker 専用の secret>'
# worker 側（自分の LAN の IP で待つ）
./build-release/hexa_udon worker --listen 192.168.1.12:39001 --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 0 --worker-count 2 --threads 8
# 主 PC から worker が動いているか確かめる
./build-release/hexa_udon worker-preflight --listen 192.168.1.12:39001 --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 0 --worker-count 2
# 主 PC
./build-release/hexa_udon auto --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN --execute \
  --lan-worker 192.168.1.12:39001 --lan-worker 192.168.1.13:39001
```
