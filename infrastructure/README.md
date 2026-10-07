# 独立インフラ

このディレクトリは現行hexa-udonのproduction sourceから分離した実行基盤を置く。`src/`、`include/`、production profile、既存protocol/sessionを変更せず、生成物、secret、runtime、結果を追加しない。

- [`distributed-cpp/`](distributed-cpp/README.md): Windows/WSLメイン1台と、WSL・Mac worker 2台を有線LANで接続するDocker Compose基盤。現段階は通信確立用で、公式API、公式token、競技POST、候補評価を含まない。当日の短縮手順は[CHEATSHEET](distributed-cpp/CHEATSHEET.md)を参照。
