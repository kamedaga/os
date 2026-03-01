Windows 11 + WSL2 (Ubuntu 24.04 noble) WSL 内で作業

Java (TLA+ 用)
sudo apt update
sudo apt install -y openjdk-17-jre

確認：

java -version

TLA+ (CLI版 TLC)
mkdir -p ~/tools/tla
cd ~/tools/tla
wget https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar

動作確認：

java -cp tla2tools.jar tlc2.TLC

mkdir -p ~/tools/zig
cd ~/tools/zig
wget https://ziglang.org/download/0.14.0/zig-linux-x86_64-0.14.0.tar.xz
tar -xf zig-linux-x86_64-0.14.0.tar.xz

PATH 追加
echo 'export PATH=$HOME/tools/zig/zig-linux-x86_64-0.14.0:$PATH' >> ~/.bashrc
source ~/.bashrc

確認：

zig version