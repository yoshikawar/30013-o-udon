# LAN worker：複数の PC で 1 日の計画を並列に解く

## 何をするか

主 PC は毎日、日の始めに各 worker へ「今日の盤・朝の状態・乱数の種」を送り、自分でも計画を解く。worker は受け取った条件で solver を回し（乱数の種は worker ごとに違う）、日の締切の 1 秒前までに計画を返す。主 PC は返ってきた計画を

1. 依頼と同じ条件で解いたか照合する（requestId、入力のハッシュ、盤、乱数の種、worker の番号、**build の版**、protocol の版、行動のハッシュ、点）
2. 自分のシミュレーター（公式ルールの再現）で確かめる
3. 自分の計画より公式の点（総系列 → 日別系列 → 玉）が**高いときだけ**使う

照合・確認に失敗した worker、時間切れの worker は使わない（主 PC の計画のまま）。worker は公式の API・token に触れない。

効果の目安: 種を変えた解を多く集めるほど、一番良い解が良くなる。1 台の中でも `--threads` で並列に回しているので、worker を足すと「主 PC のスレッド数 ＋ worker のスレッド数の合計」本から選ぶことになる。練習場の 75 日で測った例では、1 本 → 8 本で 75 日 +9 玉、16 本で +10 玉（伸びはだんだん小さくなる）。

## 準備（全部の PC）

1. **clean worktreeで同じ commit を build する**。主 PC は worker の build の版（CMake を実行したときの git の commit）が自分と同じか照合し、違えば使わない。build fingerprint は git commit 由来で、未commit変更は識別しません。主 PC・全workerをclean worktreeかつ同一commitに揃えることが必須です。未commitのアルゴリズム変更を含む状態では、同じcommit表示でもbinaryの内容が異なり得るため、worker比較の安全な検証になりません。
   ```bash
   git pull
   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
   cmake --build build-release --parallel 4
   ```
   `git pull` のあとは必ず `cmake -S . -B ...` からやり直す（`cmake --build` だけでは版が更新されないことがある）。
2. **worker 専用の secret を決めて、全部の PC で同じ値を入れる**（公式の token とは別のもの）。
   ```bash
   export HEXA_LAN_WORKER_SECRET='<適当な長い文字列>'
   ```
3. **同じ LAN（同じ Wi-Fi・ハブ）につなぎ、各 PC の IP を調べる**。worker が待てるのは `127.0.0.1` か LAN のアドレス（`10.x.x.x` / `172.16〜31.x.x` / `192.168.x.x`）だけ（`0.0.0.0` やグローバル IP は不可）。
   - Linux / WSL: `ip -4 addr`、macOS: `ipconfig getifaddr en0`、Windows: `ipconfig`

## worker を起動する

```bash
./build-release/hexa_udon worker \
  --listen <この PC の LAN の IP>:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index <0 から順の番号> --worker-count <worker の台数> \
  --threads <この PC で使うスレッド数> \
  --run-id "$RUN_ID" --worker-log "$HOME/hexa-runtime/worker-$RUN_ID.jsonl"
```

- `worker=ready` と出たら待ち受けている。Ctrl+C で止める
- `--threads` を省くと論理スレッド数の半分。worker 専用の PC なら全部使ってもよい（例: 16 スレッドの PC で `--threads 14`）
- `--worker-index` / `--worker-count` は主 PC の `--lan-worker` を書く順番と合わせる（preflight で確かめる）
- `--run-id` と `--worker-log` は省いてよい（付けると worker ごとの診断が JSONL に残る）
- 1 台の worker は依頼を 1 つずつ処理する。1 台に 2 つの worker を立てるより、1 つの worker の `--threads` を増やす方がよい

## 主 PC から確かめて、試合に出る

```bash
# worker ごとに確かめる（secret の値は表示しない）
./build-release/hexa_udon worker-preflight --listen 192.168.1.12:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET --worker-index 0 --worker-count 2
./build-release/hexa_udon worker-preflight --listen 192.168.1.13:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET --worker-index 1 --worker-count 2
```

`worker-preflight=ok` と、主 PC と同じ `buildFingerprint` が出れば使える。ただし、これはclean worktreeかつ同一commitに揃えた上での確認です。違う版が出たらその PC で build し直す。未commit変更がある場合は fingerprint が同じでも安全性を確認できないため、本番利用しないでください。

```bash
./build-release/hexa_udon auto --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN --execute \
  --lan-worker 192.168.1.12:39001 --lan-worker 192.168.1.13:39001 \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log
```

- `--lan-worker` は台数分繰り返す。主 PC も自分の `--threads` で解き続ける
- `--lan-worker-timeout-ms N`（最大 59000）で worker に使わせる時間の上限を決められる。省くと日の締切の 1 秒前まで
- 毎日の終わりに `warning=lan-worker ...` が出たら、その worker は使われていない（`claim-mismatch` = 版や条件の食い違い、`read-timeout` = 時間切れ・通信の失敗など）
- worker の計画を使った日は Session の診断に `candidateSource` が `worker` と残る

## OS ごとの注意

### Windows（ゲーミング PC など）

hexa_udon は Linux 用の通信を使うので、**WSL2（Ubuntu）の中で build・起動する**。WSL2 は既定では Windows とは別の IP を持ち、ほかの PC から直接届かない。次のどちらかで外から届くようにする（OSの版、実機のネットワーク、Firewall、WSL設定に依存するため、記載の手順だけでは動作を保証しない。未検証の手順は実機で `worker-preflight` と実runを確認してから本番利用する）。

**A. ミラーモード（Windows 11 22H2 以降）**: WSL が Windows と同じ IP を使う。
1. `%UserProfile%\.wslconfig` に次を書き、PowerShell で `wsl --shutdown` してから WSL を開き直す
   ```ini
   [wsl2]
   networkingMode=mirrored
   ```
2. 管理者の PowerShell で、待ち受けるポートを開ける
   ```powershell
   New-NetFirewallHyperVRule -Name hexa-udon-worker -DisplayName "hexa-udon worker" -Direction Inbound `
     -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol TCP -LocalPorts 39001
   New-NetFirewallRule -DisplayName "hexa-udon worker" -Direction Inbound -Protocol TCP -LocalPort 39001 -Action Allow
   ```
3. WSL の中で `--listen <Windows の LAN の IP>:39001` で起動する

**B. ポートの転送（ミラーモードが使えないとき）**: Windows の 39001 番に来た通信を WSL へ流す。
1. WSL の中で IP を調べる（`ip -4 addr show eth0` の `172.x.x.x`）。WSL を再起動すると変わる
2. 管理者の PowerShell で
   ```powershell
   netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=39001 connectaddress=<WSL の IP> connectport=39001
   New-NetFirewallRule -DisplayName "hexa-udon worker" -Direction Inbound -Protocol TCP -LocalPort 39001 -Action Allow
   ```
3. WSL の中で `--listen <WSL の IP>:39001` で起動し、主 PC からは `--lan-worker <Windows の LAN の IP>:39001` を指定する
4. 使い終わったら `netsh interface portproxy delete v4tov4 listenaddress=0.0.0.0 listenport=39001`

ネットワークの種類が「パブリック」だと受け付けないことがあるので、会場の LAN は「プライベート」にする。

### macOS

```bash
brew install cmake nlohmann-json curl
```

そのまま build・起動できる可能性があるが未検証。OSの版、ネットワーク、Firewallに依存するため、実機で `worker-preflight` と実runを確認してから本番利用する。初回の起動で「受信接続を許可しますか」と出たら許可する。

### Linux

ファイアウォール（`ufw` など）を使っていれば 39001/tcp を開ける（例: `sudo ufw allow from 192.168.1.0/24 to any port 39001 proto tcp`）。

## 本番前の確認リスト

- [ ] 主 PC・全workerが clean worktree で同じ commit を build した（preflight の `buildFingerprint` が全部同じ。未commit変更がある状態は不可）
- [ ] 全部の PC で `HEXA_LAN_WORKER_SECRET` が同じ
- [ ] 全部の worker で `worker-preflight=ok`
- [ ] OS別のネットワーク設定を実機で確認し、preflight成功後に実runも確認した
- [ ] 練習場で worker をつないで 1 試合出て、`warning=lan-worker` が出ないか、出ても主 PC の計画で最後まで出せることを確かめた
- [ ] 主 PC・worker とも、スリープ・自動更新・省電力を切った
