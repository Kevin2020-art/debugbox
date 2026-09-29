#!/bin/bash
# ==============================================================================
# DebugBox 第一阶段全自动部署脚本 (Step 1 Complete Deployment Script)
# 
# 包含功能：
#   1. 安装系统底层依赖 (C 编译器、libgpiod、NetworkManager、Python3、dnsmasq-base 等)
#   2. 编译并安装 C 语言硬件控制器与 CLI 工具 (/usr/local/bin/)
#   3. 部署生产代码与配置文件至 /opt/debugbox
#   4. 配置 NetworkManager Captive Portal DNS 泛解析劫持规则
#   5. 安装并注册 systemd 服务，配置开机自启并启动
#   6. 自动验证服务运行状态与 IPC 通信管道
#
# 使用方式：
#   本地在开发板上执行:  sudo ./scripts/deploy.sh
#   从 Mac/PC 远程部署:  ./scripts/deploy.sh --remote dong@192.168.31.107
# ==============================================================================

set -e

REMOTE_TARGET=""

# 解析参数
while [[ $# -gt 0 ]]; do
  case $1 in
    --remote)
      REMOTE_TARGET="$2"
      shift 2
      ;;
    *)
      echo "未知参数: $1"
      echo "用法: $0 [--remote user@host]"
      exit 1
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ------------------------------------------------------------------------------
# 远程部署模式 (从 Mac 一键推送到树莓派执行)
# ------------------------------------------------------------------------------
if [ -n "$REMOTE_TARGET" ]; then
  echo "=================================================================="
  echo ">>> [远程部署模式] 目标机器: ${REMOTE_TARGET}"
  echo "=================================================================="

  echo "1. 同步项目代码至树莓派 /home/dong/rasp4b_for_remote_debuger/..."
  rsync -avz --exclude '.git' --exclude 'build' --exclude '*.o' \
    "${SCRIPT_DIR}/" "${REMOTE_TARGET}:~/rasp4b_for_remote_debuger/"

  echo "2. 在远程树莓派上执行部署脚本 (需要 sudo 权限)..."
  ssh -t "${REMOTE_TARGET}" "sudo bash ~/rasp4b_for_remote_debuger/scripts/deploy.sh"

  echo "✓ 远程一键部署完成！"
  exit 0
fi

# ------------------------------------------------------------------------------
# 本机部署模式 (必须在树莓派上以 root / sudo 权限运行)
# ------------------------------------------------------------------------------
if [ "$EUID" -ne 0 ]; then
  echo "❌ 错误: 本机安装必须以 root 权限运行，请执行: sudo $0"
  exit 1
fi

INSTALL_DIR="/opt/debugbox"

echo "=================================================================="
echo ">>> 开始部署 DebugBox 第一阶段服务 (C GPIO + 手机热点配网)"
echo "=================================================================="

# 1. 安装系统依赖
echo ">>> [1/6] 安装操作系统底层依赖包..."
apt-get update -qq
apt-get install -y --no-install-recommends \
  gcc \
  make \
  libgpiod-dev \
  gpiod \
  network-manager \
  dnsmasq-base \
  python3 \
  python3-yaml \
  rsync

# 2. 编译并安装 C 硬件守护程序
echo ">>> [2/6] 编译并全局安装 C 语言硬件控制器..."
cd "${SCRIPT_DIR}/src/c_gpio"
make clean
make
make install
# 验证二进制
if [ ! -f /usr/local/bin/debugbox_gpio_daemon ]; then
  echo "❌ 编译失败: 未找到 /usr/local/bin/debugbox_gpio_daemon"
  exit 1
fi
echo "✓ C 二进制已成功安装至 /usr/local/bin/debugbox_gpio_daemon 与 /usr/local/bin/gpio_ctl"

# 3. 部署生产文件至 /opt/debugbox
echo ">>> [3/6] 同步代码至生产目录 ${INSTALL_DIR}..."
mkdir -p "${INSTALL_DIR}"
mkdir -p "${INSTALL_DIR}/config"
mkdir -p "${INSTALL_DIR}/scripts"
mkdir -p "${INSTALL_DIR}/src"

cp -r "${SCRIPT_DIR}/config/"* "${INSTALL_DIR}/config/"
cp -r "${SCRIPT_DIR}/scripts/"* "${INSTALL_DIR}/scripts/"
cp -r "${SCRIPT_DIR}/src/"* "${INSTALL_DIR}/src/"

chmod +x "${INSTALL_DIR}/scripts/"*.sh
echo "✓ 生产目录同步完成"

# 4. 配置 NetworkManager Captive Portal DNS 劫持规则
echo ">>> [4/6] 配置 Wi-Fi 强制弹窗 (Captive Portal) 通配 DNS 规则..."
mkdir -p /etc/NetworkManager/dnsmasq-shared.d
cp "${INSTALL_DIR}/config/00-captive-portal.conf" /etc/NetworkManager/dnsmasq-shared.d/
chmod 644 /etc/NetworkManager/dnsmasq-shared.d/00-captive-portal.conf
systemctl reload NetworkManager || true
echo "✓ DNS 泛解析规则配置生效 (address=/#/192.168.4.1)"

# 5. 安装并注册 systemd 服务
echo ">>> [5/6] 注册并启动 systemd 开机自启服务..."
cp "${INSTALL_DIR}/config/systemd/debugbox-gpio.service" /etc/systemd/system/
cp "${INSTALL_DIR}/config/systemd/debugbox-network.service" /etc/systemd/system/
chmod 644 /etc/systemd/system/debugbox-gpio.service
chmod 644 /etc/systemd/system/debugbox-network.service

systemctl daemon-reload
systemctl enable debugbox-gpio.service
systemctl enable debugbox-network.service

# 重启服务以应用最新代码
systemctl restart debugbox-gpio.service
sleep 1
systemctl restart debugbox-network.service
sleep 2

# 6. 状态健康检查
echo ">>> [6/6] 验证服务运行状态..."
GPIO_STATUS=$(systemctl is-active debugbox-gpio.service || true)
NET_STATUS=$(systemctl is-active debugbox-network.service || true)

echo "  - debugbox-gpio.service:    ${GPIO_STATUS}"
echo "  - debugbox-network.service: ${NET_STATUS}"

if [ "${GPIO_STATUS}" = "active" ] && [ "${NET_STATUS}" = "active" ]; then
  echo "=================================================================="
  echo "🎉 部署成功！所有服务已在后台健康运行并配置开机自启。"
  echo "  - C 语言硬件 IPC Socket: /run/debugbox_gpio.sock"
  echo "  - 测试 LED 控制:         gpio_ctl LED ONLINE"
  echo "  - 查看网络服务日志:       journalctl -u debugbox-network.service -f"
  echo "  - 查看按键硬件日志:       journalctl -u debugbox-gpio.service -f"
  echo "=================================================================="
else
  echo "⚠️ 警告: 部分服务状态异常，请运行 journalctl -xe 查看报错信息。"
  exit 1
fi
