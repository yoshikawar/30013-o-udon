# 3台通信 当日チートシート

対象はWindows/WSLメイン1台、Windows/WSL worker 1台、Mac worker 1台。各PCでこの`distributed-cpp/`ディレクトリを開いて操作する。

起動順は自由。workerを先に起動しても、メインが起動するまで自動で再接続する。公式tokenや競技サーバーのURLは、この通信基盤の`.env`へ入れない。

## 0. 全PC共通

Docker Desktopを起動する。WindowsはLinux containersとWSL integrationを有効にする。

```sh
docker info --format '{{.ServerVersion}} {{.OSType}} {{.Architecture}}'
```

想定結果はWindows/WSLが`linux x86_64`または`linux amd64`、Macが`linux arm64`。各PCで個別にイメージをbuildする。

3台を同じ有線LANへ接続する。Wi-Fiは使わない。メインPCの有線IPv4を確認し、以下では`192.168.10.10`の部分を実値へ置き換える。

共通secretを1回だけ生成し、3台の`.env`へ同じ値を設定する。

```sh
openssl rand -hex 24
```

secret、`.env`、公式tokenをGitへ追加したり、画面共有・ログへ貼ったりしない。

## 1. メイン Windows/WSL

```sh
cp .env.main.example .env
```

`.env`を編集する。

```dotenv
NODE_ID=pc-main
SEED=101
APP_DIR=apps/example
BIND_IP=192.168.10.10
HOST_PORT=39001
CLUSTER_SECRET=<3台共通secret>
EXPECTED_WORKERS=2
STARTUP_WAIT_MS=60000
JOB_TIMEOUT_MS=10000
APP_READ_TIMEOUT_MS=30000
```

`BIND_IP`にはWSLやDocker内部のIPではなく、WindowsメインPCの有線LAN IPv4を指定する。Windows Firewallでworker 2台からのTCP 39001着信を許可する。

設定確認と起動：

```sh
docker compose -f compose.main.yaml config --quiet
docker compose -f compose.main.yaml up -d --build
docker compose -f compose.main.yaml logs -f
```

待機中の正常表示：

```text
main listening; waiting for workers
```

## 2. worker Windows/WSL

```sh
cp .env.worker-wsl.example .env
```

`.env`を編集する。

```dotenv
NODE_ID=pc-worker-wsl
SEED=202
APP_DIR=apps/example
MAIN_HOST=192.168.10.10
CLUSTER_SECRET=<3台共通secret>
JOB_TIMEOUT_MS=10000
```

`MAIN_HOST`にはメインPCの有線LAN IPv4を指定する。

```sh
docker compose -f compose.worker.yaml config --quiet
docker compose -f compose.worker.yaml up -d --build
docker compose -f compose.worker.yaml logs -f
```

## 3. worker Mac

```sh
cp .env.worker-mac.example .env
```

`.env`を編集する。

```dotenv
NODE_ID=pc-worker-mac
SEED=303
APP_DIR=apps/example
MAIN_HOST=192.168.10.10
CLUSTER_SECRET=<3台共通secret>
JOB_TIMEOUT_MS=10000
```

```sh
docker compose -f compose.worker.yaml config --quiet
docker compose -f compose.worker.yaml up -d --build
docker compose -f compose.worker.yaml logs -f
```

## 4. 接続成功の判定

メインのログに、異なる2つのnode IDが表示されることを確認する。

```text
worker connected: pc-worker-wsl
worker connected: pc-worker-mac
starting application; workers=2
```

workerのログでは次を確認する。

```text
connected to main
```

サンプル疎通では、メインに両workerの`candidate`、`worker_done`、最後に`done`が表示されれば成功。

```sh
docker compose -f compose.main.yaml ps
docker compose -f compose.worker.yaml ps
```

worker Composeはホスト側ポートを公開しない。競技サーバーへの通信・公式token・POSTはメインプログラムだけが担当する。

## 5. つながらないとき

上から順に確認する。

1. 3台の`.env`で`CLUSTER_SECRET`が完全に同じか。
2. `NODE_ID`が3台で重複していないか。
3. workerの`MAIN_HOST`がメインPCの有線IPv4か。
4. メインの`BIND_IP`が同じ有線IPv4か。
5. Windows FirewallでTCP 39001が許可されているか。
6. メインのポートが公開されているか。

```sh
docker compose -f compose.main.yaml ps
```

7. workerからメインへ到達できるか。WSLとMacで実行する。

```sh
ping -c 3 192.168.10.10
```

8. 既に別プロセスが39001を使用していないか。

WSL：

```sh
ss -ltnp | grep 39001
```

Mac：

```sh
lsof -nP -iTCP:39001 -sTCP:LISTEN
```

`main unavailable; reconnecting`は、メイン未起動・停止中・IP不正・Firewall遮断時に表示される。workerは停止せず再接続する。`worker registration rejected`はsecret不一致、node ID重複、identity不正を疑う。

## 6. 再起動と停止

設定変更後：

```sh
docker compose -f compose.main.yaml up -d --build --force-recreate
docker compose -f compose.worker.yaml up -d --build --force-recreate
```

通常停止：

```sh
docker compose -f compose.main.yaml down
docker compose -f compose.worker.yaml down
```

named volumeを保持するため、通常は`down -v`を使わない。

## 7. 実プログラムへ交換するとき

各PCの実プログラムを`apps/local/`へ置き、各`.env`を次へ変更する。

```dotenv
APP_DIR=apps/local
```

その後、各PCで対応するComposeを`--build --force-recreate`付きで起動する。実プログラムはCMake installで`bin/application`を配置する必要がある。通信確認用サンプルで3台疎通を確認してから交換する。

詳細仕様は[README](README.md)を参照。
