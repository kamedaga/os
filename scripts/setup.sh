#!/usr/bin/env bash
set -e

echo "Installing Java..."
sudo apt update
sudo apt install -y openjdk-17-jre

echo "Installing TLA+..."
mkdir -p ~/tools/tla
cd ~/tools/tla
wget -nc https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar

echo "Installing Zig..."
mkdir -p ~/tools/zig
cd ~/tools/zig
wget -nc https://ziglang.org/download/0.14.0/zig-linux-x86_64-0.14.0.tar.xz
tar -xf zig-linux-x86_64-0.14.0.tar.xz

if ! grep -q "zig-linux-x86_64-0.14.0" ~/.bashrc; then
  echo 'export PATH=$HOME/tools/zig/zig-linux-x86_64-0.14.0:$PATH' >> ~/.bashrc
fi

echo "Setup complete."