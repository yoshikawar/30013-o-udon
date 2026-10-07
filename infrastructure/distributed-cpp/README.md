# 別プロジェクト用 C++ 分散実行基盤

今のhexa-udonとは独立した通信・起動基盤です。このディレクトリ全体を3台へコピーして利用できます。既存mainのLAN workerで確認済みの責務分離を参考にしていますが、production source、公式API、Session、worker protocolには接続しません。この段階の目的は、メイン1台とworker 2台のTCP接続、識別、疎通確認までです。

Mac 1台＋Windows/WSL 2台を想定しています。各PCに別のC++アプリを配置し、初回設定後は各PCで次を実行します。

メインでは`docker compose -f compose.main.yaml up -d --build`、workerでは`docker compose -f compose.worker.yaml up -d --build`を使います。メインイメージだけがTCP 39001をlistenでき、workerイメージにはメイン用実行ファイルと公開ポートがありません。

状態確認は同じ`-f`指定で`docker compose ps`と`docker compose logs`を実行します。停止は`docker compose -f <使用したファイル> down`です。named volumeを残すため通常は`down -v`を使いません。

## 3台の構成

```text
PC1 / Windows・WSL：次期メインアプリ＋焼きなまし＋公式提出
  中継がTCP 39001を受信待ち
          ↑ 接続・結果返信          ↑ 接続・結果返信
PC2 / WSL：次期焼きなまし    PC3 / Mac：現行解法（接続は未実装）
  SEED=202                    SEED=303
```

WSLの2台で次期焼きなまし本体を別シードで動かし、Macの現行解法を補助にします。競技サーバーへ接続・提出するのはメインPCだけです。workerへ公式tokenを配布しません。メインアプリは自分の計算とstdinの受信を並行させ、届いた改善候補を共通の検証・採点器で評価します。次期本体と現行解法の接続はまだ未実装です。

## 初回のネットワーク設定

1. 3台を同じスイッチにつなぎ、同一IPv4サブネットで互いに到達できるようにします。スイッチだけではIPアドレスは配られません。DHCPのあるルーターをつなぐか、OS側で重複しない固定IPを設定します。
2. Mac/WindowsでDocker Desktopを起動します。WindowsはLinux containersとWSL integrationを有効にし、WSLターミナルで操作します。
3. メインPCの有線LAN IPv4アドレスを確認し、下記の`.env`へ設定します。サンプルの`192.168.10.10`は仮の値です。WSL内やDocker内のIPを接続先にしません。
4. メインPCのファイアウォールで、2台のworkerからのTCP 39001着信を許可します。workerは外向き接続だけで、WSL側の受信ポート転送は不要です。
5. アドレスを固定またはDHCP予約しておくと、以後の設定変更を減らせます。

Docker Desktopの公開ポートを使う構成です。Composeのサービス名が別PCでも名前解決できるとは扱いません。LAN自動検出、host networking、WSL mirrored networkingには依存しません。[Docker公式のDesktopネットワーク説明](https://docs.docker.com/desktop/features/networking/)と[Composeネットワーク説明](https://docs.docker.com/compose/how-tos/networking/)を参照。

## 各PCの設定

メインPCではこのディレクトリで実行します。

```sh
cp .env.main.example .env
docker compose -f compose.main.yaml config --quiet
docker compose -f compose.main.yaml up -d --build
```

`.env`の`BIND_IP`を実際の有線LAN IPへ変更し、`CLUSTER_SECRET`を自分たちの共通のランダム文字列（16〜256文字、改行なし）に変更します。サンプルのsecretのままでは起動しません。`SEED=101`がメインアプリへ渡されます。

WSL workerでは次を実行します。

```sh
cp .env.worker-wsl.example .env
docker compose -f compose.worker.yaml config --quiet
docker compose -f compose.worker.yaml up -d --build
```

Mac workerでは次を実行します。

```sh
cp .env.worker-mac.example .env
docker compose -f compose.worker.yaml config --quiet
docker compose -f compose.worker.yaml up -d --build
```

両workerとも`MAIN_HOST`をメインPCの有線LAN IPへ、`CLUSTER_SECRET`をメインと同じ値へ変更します。node IDは3台で一意にします。シードは0〜18446744073709551615です。

起動順は自由です。workerはメインが来るまで外向き接続を再試行します。メインは既定で最大60秒、2台の登録を待ってから疎通確認アプリを起動します。待機終了時に不足していても起動し、接続済みworkerだけを確認します。

サンプルは1回の依頼を送って終了します。終了後workerに再接続待ちが表示されるのは想定どおりです。実アプリの寿命は実アプリ自身が決めます。

`.env`はGit管理外です。共通secretはこの独立基盤専用であり、公式tokenを使わないでください。通信は平文TCPです。信頼できる専用LANを対象とし、インターネットへポート公開しません。

## 届いたC++アプリを置く

各PCの`apps/local/`に、そのPCで動かすプロジェクトを置きます。異なるPCで中身が違って構いません。`.env`を`APP_DIR=apps/local`へ変更します。`apps/local/`は未着の別プロジェクトを誤ってこのrepoへ追加しないようGit管理外です。

```text
distributed-cpp/
  compose.main.yaml
  compose.worker.yaml
  Dockerfile
  .env                       # PCごとの設定
  bridge/                    # main server / worker client
  apps/
    example/                 # 起動確認用の最小サンプル
    local/                   # 届いたプロジェクト
      CMakeLists.txt
      ...
```

アプリはCMakeでビルドし、`cmake --install`によって`bin/application`に実行ファイルを配置してください。例えば既存のtargetが`my_solver`なら、CMakeに次を追加します。

```cmake
set_target_properties(my_solver PROPERTIES OUTPUT_NAME application)
install(TARGETS my_solver RUNTIME DESTINATION bin)
```

`apps/example/CMakeLists.txt`も参考にできます。Linux用にコンテナ内でビルドするため、Mac/Windowsのビルド済みバイナリを置くだけでは動きません。外部ライブラリが必要ならDockerfileのビルド・実行用依存を追加します。アセットも必要な場所へ`install`してください。初回ビルドにはイメージとパッケージを取得できるネット接続が必要です。オフライン会場では事前にビルドします。

## アプリと中継の入出力

既存アプリに下記の薄い入出力アダプターを付けます。任意のプログラムを無変更で置くだけで相互通信できるわけではありません。通信や解法を未着の本体へ埋め込まず、stdin/stdoutを境界にします。

### メインアプリ

- `ROLE=main`で1回起動し、その後はアプリが終了するまで動作します。
- 他PCへ渡したい問題をstdoutへ1行出力し、flushします。形式は全解法で合意したASCIIの1行（JSON推奨、非ASCII文字はエスケープ）で、最大1 MiBです。
- 中継は接続済みworkerへ同じ入力を並行送信します。その間にメインアプリは解法Aを計算できます。
- stdinから複数行のJSONイベントを継続して読みます。受信スレッドまたはイベントループを計算と独立して動かしてください。依頼は1件ずつ扱い、`done`を読んでから次の依頼を送ります。
- `jobId`は中継が起動中に1から順番に発行する文字列です。再起動をまたぐ競技日・入力の同一性はアプリのpayload内でも識別・検証してください。

```json
{"type":"candidate","jobId":"1","result":{"nodeId":"pc-worker-1","seed":"202","status":"unvalidated","payload":"123"}}
{"type":"candidate","jobId":"1","result":{"nodeId":"pc-worker-1","seed":"202","status":"unvalidated","payload":"124"}}
{"type":"worker_done","jobId":"1","result":{"nodeId":"pc-worker-1","seed":"202","status":"ok","payload":""}}
{"type":"worker_done","jobId":"1","result":{"nodeId":"pc-worker-2","seed":"303","status":"unavailable","payload":""}}
{"type":"done","jobId":"1"}
```

`payload`はworkerの出力をJSON文字列に包んだものです。workerがJSONを出した場合はpayloadもパースします。`candidate`は未検証の候補で、worker終了前・他worker完了前に配送されます。`worker_done`の`ok`は正常終了、`error`は解法失敗、`unavailable`は切断・期限超過・不正応答であり、候補の正当性を表しません。終了前の候補も必ず独立検証します。全台不在なら`done`だけが届きます。到着順は非決定的で、評価のtie-breakに使ってはいけません。

配送待ちは最大64イベント＋workerごとの終了通知に制限し、未検証候補を別候補で上書きしません。受信が遅い場合は通信へbackpressureがかかり、計算期限を過ぎたworkerは`unavailable`になります。メインがstdinを読まない場合、`APP_READ_TIMEOUT_MS`（既定30秒）で停止します。これは競技の提出期限ではありません。以前の固定1秒での停止を撤廃しましたが、継続受信はアプリの責務です。

wire protocolは`cpp-cluster-v2`です。旧v1とは互換性がないため3台とも更新してください。

メインは候補の妥当性とスコアを自分で検証し、自分の候補を含めて選択します。中継は結果を採用せず、公式APIに接続せず、競技POSTを行いません。実際の問題のschema、採点、fallbackは別プロジェクト側で実装します。

### workerアプリ

- 依頼ごとに実行ファイルを起動します。stdinの1行を読み、最良候補が改善するたびにstdoutへ1行出してflushし、計算を続けます。完了時は終了コード0で終了します。1行だけ返す既存アダプターも使用できます。
- `SEED`、`NODE_ID`、`ROLE=worker`を環境変数から読みます。C++では`std::getenv("SEED")`等を使用します。
- 各候補は最大8 KiBです。焼きなましの一時的な悪化状態ではなくbest-so-farを出します。診断はstderrへ出してください。stdoutの各行は独立した候補として扱われます。
- `JOB_TIMEOUT_MS`以内で計算します。中継は期限超過の子プロセス群を停止します。重い初期化が毎回必要な常駐solver対応は将来の拡張です。

毎回同じシードから起動するため、日・問題ごとに探索列を変えたい場合は、アプリ側で設定シードと入力中のjob IDを決定的に組み合わせます。乱数列は各解法の内部設定であり、中継は上書きしません。

## 保存・停止・障害

- 作業ディレクトリ`/state`はPCごとのDocker named volumeです。アプリの保存データはここへ書きます。`docker compose down`では残り、`down -v`で削除されるため通常は`-v`を使いません。
- 実行ファイルとroot filesystemはread-only、`/tmp`は一時領域です。コンテナは非rootで実行します。
- workerとの接続は再接続しますが、失敗した計算依頼は自動再送しません。メインアプリも自動再起動しません。
- メインアプリの永続化・公式送信の重複防止・試合deadlineは、この汎用中継とは別に実装してください。現在のhexa-udonの安全契約を継承した製品ではありません。
- `JOB_TIMEOUT_MS`は1回のworker依頼の通信込み上限（100〜600000ms）です。worker側は自身の上限との小さい方で停止します。これは競技の絶対deadlineを保証しません。

## 確認手順

1. サンプルを各PCで起動し、mainのログに2台の`worker connected`が出ることを確認する。
2. `remote event`に各node ID・設定したseedの`candidate`が終了前から届き、`worker_done`と最後の`done`が出ることを確認する。
3. 各PCで別のアプリへ差し替え、同じ問題入力から結果が返ることを確認する。
4. worker停止、誤ったsecret、遅いsolver、同じnode IDで、メインが誤採用せずエラーまたは候補不足を扱えることを確認する。

実LAN接続とDockerコンテナ実行の確認は別途必要です。ローカルのコード検証だけでは3台の接続成功を意味しません。

### 開発用のloopbackテスト

Python 3とC++20コンパイラのあるMac/Linux/WSLで、このディレクトリから実行します。既存競技プロジェクトのbuildやCTestとは独立しています。

```sh
cmake -S bridge -B build/bridge
cmake --build build/bridge
cmake -S apps/example -B build/example
cmake --build build/example
c++ -std=c++20 -pthread tests/slow_solver.cpp -o build/slow_solver
python3 tests/smoke.py build/bridge/cluster_bridge build/example/application build/slow_solver
```

途中候補・連続依頼・受信遅延の回帰確認は追加で実行します。

```sh
c++ -std=c++20 -pthread tests/stream_fixture.cpp -o build/stream_fixture
python3 tests/streaming.py build/bridge/cluster_bridge build/stream_fixture
python3 tests/fixed_roles.py build/bridge/cluster_main build/bridge/cluster_worker build/example/application
python3 tests/startup_order.py build/bridge/cluster_main build/bridge/cluster_worker build/example/application
```

Macネイティブで確認しても、Windowsメインでの3台の実LAN接続は別途検証が必要です。実API・競技POSTはこれらのテストで実行しません。採点・再提出の統合契約は[方針資料](../../docs/distributed-cpp.md)を参照してください。

Dockerイメージは実行するPC上でビルドします。MacはLinux/arm64、2台のWindows/WSLはLinux/x86-64になります。異なるCPU向けにビルドしたローカルイメージをPC間でコピーしません。通信はアーキテクチャに依存しない改行区切りASCIIです。

2026-10-08: MacネイティブのC++20ビルド、main/worker両Compose設定、loopbackの10ケースを確認。2worker識別、固定役割バイナリ、worker先行起動からの再接続、異常終了、secret不一致、worker不在、時間超過、終了前配送、2依頼連続、3秒の受信遅延を検証しました。さらにDocker Desktop 29.8.1のLinux/arm64で両イメージをbuildし、別々のCompose projectとして起動したメイン1台・worker 2台の接続、node ID・seed識別、候補配送、正常終了を確認しました。Windows/WSLのLinux/x86-64 buildと3台の実LAN接続は未検証です。実API・競技POSTはこの基盤の対象外です。
