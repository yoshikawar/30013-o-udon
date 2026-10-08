# hexa-udon 本番 Docker 3台通信 当日チートシート

対象は Windows/WSL の main 1台、Windows/WSL worker 1台、Mac worker 1台。今回の本番コードは worker が TCP 39001 を待受し、main が各 worker へ接続する。旧 `cluster_bridge` サンプルとは通信方向が逆なので、以下はすべて `infrastructure/distributed-cpp/apps/test/` で実行する。

公式 API、公式 token、競技 POSTを扱うのは main だけ。worker には worker 専用 secret だけを置く。

## 0. 全PC共通

Docker Desktopを起動する。WindowsはLinux containersとWSL integrationを有効にする。

```sh
cd infrastructure/distributed-cpp/apps/test
docker info --format '{{.ServerVersion}} {{.OSType}} {{.Architecture}}'
git rev-parse --short=12 HEAD
```

3台で同じcommitをcheckoutし、最後のコマンドで表示された12文字を全 `.env` の `SOURCE_REVISION` に設定する。値が異なるworkerの候補はmainが採用しない。

3台を同じ有線LANへ接続する。以下の例ではmainを `192.168.10.10`、WSL workerを `192.168.10.20`、Mac workerを `192.168.10.30` とする。Docker/WSL内部IPではなく、各PCの有線IPv4へ置き換える。

worker専用secretを1回だけ生成し、3台の `.env` に同じ値を設定する。

```sh
openssl rand -hex 24
```

`.env`、secret、公式tokenをGit、画面共有、ログへ出さない。

## 1. worker 1: Windows/WSL

```sh
cp .env.worker.example .env
```

```dotenv
SOURCE_REVISION=<3台で同じ12文字>
BIND_IP=192.168.10.20
HOST_PORT=39001
HEXA_LAN_WORKER_SECRET=<3台共通のworker専用secret>
WORKER_INDEX=0
WORKER_COUNT=2
RUN_ID=competition
```

Windows Firewallではmain PC (`192.168.10.10`) からのTCP 39001着信だけを許可する。

```sh
docker compose -f compose.worker.yaml config --quiet
docker compose -f compose.worker.yaml up -d --build
docker compose -f compose.worker.yaml logs -f
```

正常なら `worker=ready` と表示される。

## 2. worker 2: Mac

```sh
cp .env.worker.example .env
```

```dotenv
SOURCE_REVISION=<3台で同じ12文字>
BIND_IP=192.168.10.30
HOST_PORT=39001
HEXA_LAN_WORKER_SECRET=<3台共通のworker専用secret>
WORKER_INDEX=1
WORKER_COUNT=2
RUN_ID=competition
```

```sh
docker compose -f compose.worker.yaml config --quiet
docker compose -f compose.worker.yaml up -d --build
docker compose -f compose.worker.yaml logs -f
```

正常なら `worker=ready` と表示される。worker 1/2の `WORKER_INDEX` は重複させず、どちらも `WORKER_COUNT=2` にする。

## 3. main: Windows/WSL

```sh
cp .env.main.example .env
```

```dotenv
SOURCE_REVISION=<3台で同じ12文字>
VENUE_BASE_URL=https://<当日の公式host>
PROCON_TOKEN=<公式token>
HEXA_LAN_WORKER_SECRET=<3台共通のworker専用secret>
LAN_WORKER_1=192.168.10.20:39001
LAN_WORKER_2=192.168.10.30:39001
SEED=30013
SAFETY_SECONDS=3
EXECUTE=false
```

`LAN_WORKER_1` は `WORKER_INDEX=0`、`LAN_WORKER_2` は `WORKER_INDEX=1` のPCに合わせる。まず `EXECUTE=false` のままにする。この状態でも公式APIへのGETは行うが、POSTはしない。

```sh
docker compose -f compose.main.yaml config --quiet
docker compose -f compose.main.yaml build
```

## 4. POSTなしのworker疎通確認

main PCから各workerへpreflightする。これは公式APIへ接続せず、競技POSTもしない。

```sh
docker run --rm --env-file .env --entrypoint hexa_udon hexa-udon:local \
  worker-preflight --listen 192.168.10.20:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET --worker-index 0 --worker-count 2

docker run --rm --env-file .env --entrypoint hexa_udon hexa-udon:local \
  worker-preflight --listen 192.168.10.30:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET --worker-index 1 --worker-count 2
```

両方で次の形式が出れば通信・secret・役割番号は一致している。`buildFingerprint` も3台の `SOURCE_REVISION` と一致することを確認する。

```text
worker-preflight=ok protocolSchemaVersion=1 buildFingerprint=<SOURCE_REVISION> workerIndex=0 workerCount=2 ... secretConfigured=true
```

## 5. mainのdry-run

公式APIのGETと計画処理まで確認するが、まだPOSTはしない。

```sh
docker compose -f compose.main.yaml up
```

終了または `Ctrl-C` 後、ログに `warning=lan-worker`、`claim-mismatch`、`strict-revalidation-failed` がないことを確認する。dry-runの結果だけで実LAN本番完走を推定しない。

## 6. 本番実行

公式画面、時刻、token、URL、2台のworker、保存volumeを確認してからmain PCの `.env` だけを変更する。

```dotenv
EXECUTE=true
```

```sh
docker compose -f compose.main.yaml up
```

`EXECUTE=true` は競技POSTを行う。mainの多重起動は禁止。同じSessionで `RecoveryRequired` が出たら自動再送せず、人手で公式状態と照合する。

## 7. 状態確認・停止・再起動

```sh
docker compose -f compose.main.yaml ps
docker compose -f compose.worker.yaml ps
docker compose -f compose.main.yaml logs --tail=200
docker compose -f compose.worker.yaml logs --tail=200
```

通常停止ではnamed volumeを残す。`down -v` はSessionや状態を削除するため使用しない。

```sh
docker compose -f compose.main.yaml down
docker compose -f compose.worker.yaml down
```

sourceや `.env` を変更した場合だけ再buildする。

```sh
docker compose -f compose.main.yaml up --build --force-recreate
docker compose -f compose.worker.yaml up -d --build --force-recreate
```

## 8. つながらないとき

1. workerのログが `worker=ready` か。
2. 3台の `SOURCE_REVISION` とworker専用secretが一致するか。
3. `WORKER_INDEX` が0/1、`WORKER_COUNT` が両方2か。
4. mainの `LAN_WORKER_1/2` の順序がindex 0/1と一致するか。
5. workerの `BIND_IP` がそのPCの有線IPv4か。
6. mainからworkerのTCP 39001へ到達できるか。
7. worker側Firewallがmain PCからの着信を許可しているか。
8. 39001が別プロセスに使われていないか。

WSL:

```sh
ss -ltnp | grep 39001
```

Mac:

```sh
lsof -nP -iTCP:39001 -sTCP:LISTEN
```

詳細は本番コードの [`README.md`](apps/test/README.md) を参照する。
