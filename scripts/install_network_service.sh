#!/bin/bash
# ==============================================================================
# 第一步服务安装脚本：手机热点配网 + GPIO 按键与 LED 服务
# 支持系统：Raspberry Pi OS (Bookworm / Bullseye 64-bit Lite)
# ==============================================================================

set -e

if [ "$EUID" -ne 0 ]; then
  echo "请以 root 权限运行此脚本: sudo bash scripts/install_network_service.sh"
  exit 1
fi

INSTALL_DIR="/opt/debugbox"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo ">>> 1. 安装基础依赖包 (NetworkManager, Python3, libgpiod-dev, gcc, make)..."
apt-get update
apt-get install -y network-manager python3 python3-yaml libgpiod-dev gcc make

echo ">>> 2. 编译并安装 C 语言硬件控制器 (debugbox_gpio_daemon)..."
cd "${SCRIPT_DIR}/src/c_gpio"
make clean && make
make install

echo ">>> 3. 部署源码至 ${INSTALL_DIR}..."
mkdir -p "${INSTALL_DIR}"
cp -r "${SCRIPT_DIR}/config" "${INSTALL_DIR}/"
cp -r "${SCRIPT_DIR}/src" "${INSTALL_DIR}/"
cp -r "${SCRIPT_DIR}/scripts" "${INSTALL_DIR}/"
chmod +x "${INSTALL_DIR}/scripts/"*.sh

echo ">>> 4. 配置 NetworkManager AP 模式下的 DNS 泛解析 (自动弹窗)..."
mkdir -p /etc/NetworkManager/dnsmasq-shared.d
cp "${SCRIPT_DIR}/config/00-captive-portal.conf" /etc/NetworkManager/dnsmasq-shared.d/
systemctl reload NetworkManager || true

echo ">>> 4. 配置 systemd 开机自启守护服务..."
cp "${INSTALL_DIR}/config/systemd/debugbox-gpio.service" /etc/systemd/system/
cp "${INSTALL_DIR}/config/systemd/debugbox-network.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable debugbox-gpio.service
systemctl enable debugbox-network.service

echo ">>> 5. 启动服务..."
systemctl restart debugbox-gpio.service
systemctl restart debugbox-network.service

echo "=================================================================="
echo "✓ 第一步：手机热点配网与硬件按键服务安装完成！"
echo "  - 查看运行状态: sudo systemctl status debugbox-network.service"
echo "  - 查看实时日志: sudo journalctl -u debugbox-network.service -f"
echo "=================================================================="
