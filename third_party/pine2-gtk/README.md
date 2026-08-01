# Pine2 GTK3 Desktop

Pine2の既存HTTPS APIを直接利用するGTK3ネイティブクライアントです。WebViewは使いません。

現在のプレビュー版で利用できるもの:

- HttpOnly Cookieによる既存Pine2認証とセッション復元
- Web版と同じログイン・アカウント作成切り替え
- ルーム一覧と未読数
- ルーム作成と招待リンクのコピー
- 4秒間隔のメッセージ同期
- 15秒間隔のルーム一覧再同期
- 表示中ルームの一括既読化
- メッセージ送信と自分の投稿の既読数
- Enterで送信、Shift+Enterで改行する複数行入力
- プロフィールAPIから取得したユーザーアイコンの円形表示とメモリキャッシュ
- Thunarのような平坦な面と細い境界線を基調にしたデスクトップ向けUI
- Pineオレンジをアクセントにしたライト／ダークテーマ
- ネイティブ設定画面からの即時テーマ切り替えと選択内容の保存
- ネイティブ設定画面での表示名・自己紹介・URL画像・プリセットアイコン編集
- アカウント情報、ユーザー管理への導線、接続・同期情報の確認
- ネイティブ記事一覧・本文表示・新規作成・自分の記事の編集
- cmark-gfmによるCommonMark/GFMプレビュー（見出し、強調、引用、リスト、リンク、コード、表など）

Boomタブは、ネイティブ画面が実装されるまでWeb版を既定ブラウザで開きます。GTK版では
安全のため記事内HTMLを実行せず、Mermaidコードフェンスはソース表示にフォールバックします。

保存済みセッションは `$XDG_STATE_HOME/pine2-gtk/cookies.txt`（通常は
`~/.local/state/pine2-gtk/cookies.txt`）にユーザー本人だけが読める状態で保存します。
テーマ設定は `$XDG_CONFIG_HOME/pine2-gtk/settings.ini`（通常は
`~/.config/pine2-gtk/settings.ini`）へ保存します。

## WSL2でビルド

WSL側にDockerがあれば、開発用パッケージをホストへ追加せずにビルドできます。

```sh
cd desktop-gtk
chmod +x tools/build-wsl.sh tools/test-wsl.sh
./tools/build-wsl.sh
./tools/test-wsl.sh
./build/pine2-gtk
```

## musl版をビルド

Alpine Linux 3.21のコンテナで、musl向けのリリースバイナリを生成できます。
既存のglibc版とは別に`build-musl/pine2-gtk`へ出力されます。

```sh
cd desktop-gtk
chmod +x tools/build-musl.sh tools/test-musl.sh
./tools/build-musl.sh
./tools/test-musl.sh
```

GTK3は動的リンクのままです。実行先にはAlpineの
`gtk+3.0 libcurl json-c font-noto-cjk`相当が必要です。cmark-gfm本体とGFM拡張は
musl版バイナリへ静的リンクするため、実行先で追加する必要はありません。
生成されるCPUアーキテクチャはDockerを動かしているホストと同じです。

Alpine/musl環境で直接起動する場合:

```sh
apk add gtk+3.0 libcurl json-c font-noto-cjk
PINE2_SERVER_URL=https://pine2-nine.vercel.app ./build-musl/pine2-gtk
```

WSLへ開発用パッケージを入れる場合は、通常のMesonでもビルドできます。

```sh
sudo apt install build-essential meson ninja-build libgtk-3-dev libcurl4-openssl-dev libjson-c-dev libcmark-gfm-dev libcmark-gfm-extensions-dev
meson setup build
meson compile -C build
./build/pine2-gtk
```

日本語を表示するため、WSLまたは実行環境に日本語フォントも必要です。Ubuntuでは
`fonts-noto-cjk`、Alpineでは`font-noto-cjk`を利用できます。

```sh
sudo apt install fonts-noto-cjk
```

別のPine2環境へ接続するときは起動時に指定します。

```sh
PINE2_SERVER_URL=http://localhost:3000 ./build/pine2-gtk
```

## 自作OS向けランタイム依存

GTK3版はGTK3、GLib/GIO、libcurl、json-c、cmark-gfmと日本語フォントを必要とします。
Alpine系なら`gtk+3.0 curl json-c cmark-gfm font-noto-cjk`に相当します。動画再生は次段階で
GStreamerを任意依存として追加し、
未導入環境ではポスター表示と外部プレイヤー起動へフォールバックする予定です。
