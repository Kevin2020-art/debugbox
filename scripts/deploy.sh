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

# 2. 编译并安装 C 硬件守护程序与串口服务、安装 EasyTier
echo ">>> [2/7] 编译并全局安装 C 语言硬件控制器与串口守护进程..."
cd "${SCRIPT_DIR}/src/c_gpio"
make clean
make
make install

cd "${SCRIPT_DIR}/src/c_serial"
make clean
make
make install

# 自动检查并安装 EasyTier
if [ ! -f /usr/local/bin/easytier-core ]; then
  echo ">>> [EasyTier] 下载并安装 EasyTier P2P 组网工具 (v2.6.4 aarch64)..."
  curl -L -o /tmp/easytier.zip https://github.com/EasyTier/EasyTier/releases/download/v2.6.4/easytier-linux-aarch64-v2.6.4.zip
  unzip -o /tmp/easytier.zip -d /tmp/
  cp /tmp/easytier-linux-aarch64/easytier-core /tmp/easytier-linux-aarch64/easytier-cli /usr/local/bin/
  chmod +x /usr/local/bin/easytier-*
fi

# 验证二进制
if [ ! -f /usr/local/bin/debugbox_gpio_daemon ] || [ ! -f /usr/local/bin/debugbox_serial_daemon ] || [ ! -f /usr/local/bin/debug-console ] || [ ! -f /usr/local/bin/easytier-core ]; then
  echo "❌ 编译/安装失败: 未找到必需的可执行文件"
  exit 1
fi
echo "✓ 必需可执行文件已全部就绪 (/usr/local/bin/)"

# 3. 部署生产文件至 /opt/debugbox 与创建日志目录
echo ">>> [3/7] 同步代码至生产目录 ${INSTALL_DIR} 与准备日志目录..."
mkdir -p "${INSTALL_DIR}"
mkdir -p "${INSTALL_DIR}/config"
mkdir -p "${INSTALL_DIR}/scripts"
mkdir -p "${INSTALL_DIR}/src"
mkdir -p /var/log/debugbox
chmod 755 /var/log/debugbox

cp -r "${SCRIPT_DIR}/config/"* "${INSTALL_DIR}/config/"
cp -r "${SCRIPT_DIR}/scripts/"* "${INSTALL_DIR}/scripts/"
cp -r "${SCRIPT_DIR}/src/"* "${INSTALL_DIR}/src/"

chmod +x "${INSTALL_DIR}/scripts/"*.sh
echo "✓ 生产目录与日志目录准备完成"

# 4. 配置 UDEV 固化串口规则
echo ">>> [4/7] 安装 UDEV 串口固化规则 (/dev/target_uart)..."
cp "${INSTALL_DIR}/config/99-target-serial.rules" /etc/udev/rules.d/
udevadm control --reload-rules || true
udevadm trigger || true
echo "✓ UDEV 规则已加载生效"

# 5. 配置 NetworkManager Captive Portal DNS 劫持规则
echo ">>> [5/7] 配置 Wi-Fi 强制弹窗 (Captive Portal) 通配 DNS 规则..."
mkdir -p /etc/NetworkManager/dnsmasq-shared.d
cp "${INSTALL_DIR}/config/00-captive-portal.conf" /etc/NetworkManager/dnsmasq-shared.d/
chmod 644 /etc/NetworkManager/dnsmasq-shared.d/00-captive-portal.conf
systemctl reload NetworkManager || true
echo "✓ DNS 泛解析规则配置生效 (address=/#/192.168.4.1)"

# 6. 安装并注册 systemd 服务
echo ">>> [6/7] 注册并启动 systemd 开机自启服务 (含 4路全通道串口守护进程)..."
cp "${INSTALL_DIR}/config/systemd/debugbox-gpio.service" /etc/systemd/system/
cp "${INSTALL_DIR}/config/systemd/debugbox-network.service" /etc/systemd/system/
cp "${INSTALL_DIR}/config/systemd/debugbox-serial@.service" /etc/systemd/system/
cp "${INSTALL_DIR}/config/systemd/debugbox-easytier.service" /etc/systemd/system/
chmod 644 /etc/systemd/system/debugbox-*.service /etc/systemd/system/debugbox-serial@.service 2>/dev/null || true

# 停用并清理旧的单串口服务
systemctl stop debugbox-serial.service 2>/dev/null || true
systemctl disable debugbox-serial.service 2>/dev/null || true
rm -f /etc/systemd/system/debugbox-serial.service /run/debugbox_serial.sock

systemctl daemon-reload
systemctl enable debugbox-gpio.service
systemctl enable debugbox-network.service
systemctl enable debugbox-serial@uart1.service
systemctl enable debugbox-serial@uart2.service
systemctl enable debugbox-serial@uart3.service
systemctl enable debugbox-serial@uart4.service
systemctl enable debugbox-easytier.service

# 重启服务以应用最新代码
systemctl restart debugbox-gpio.service
sleep 1
systemctl restart debugbox-network.service
sleep 1
systemctl restart debugbox-serial@uart1.service
systemctl restart debugbox-serial@uart2.service
systemctl restart debugbox-serial@uart3.service
systemctl restart debugbox-serial@uart4.service
sleep 1
systemctl restart debugbox-easytier.service
sleep 2

# 7. 状态健康检查
echo ">>> [7/7] 验证服务运行状态..."
GPIO_STATUS=$(systemctl is-active debugbox-gpio.service || true)
NET_STATUS=$(systemctl is-active debugbox-network.service || true)
UART1_STATUS=$(systemctl is-active debugbox-serial@uart1.service || true)
UART2_STATUS=$(systemctl is-active debugbox-serial@uart2.service || true)
UART3_STATUS=$(systemctl is-active debugbox-serial@uart3.service || true)
UART4_STATUS=$(systemctl is-active debugbox-serial@uart4.service || true)
EASYTIER_STATUS=$(systemctl is-active debugbox-easytier.service || true)

echo "  - debugbox-gpio.service:         ${GPIO_STATUS}"
echo "  - debugbox-network.service:      ${NET_STATUS}"
echo "  - debugbox-serial@uart1~4:       ${UART1_STATUS} / ${UART2_STATUS} / ${UART3_STATUS} / ${UART4_STATUS}"
echo "  - debugbox-easytier.service:     ${EASYTIER_STATUS}"

if [ "${GPIO_STATUS}" = "active" ] && [ "${NET_STATUS}" = "active" ] && [ "${SERIAL_STATUS}" = "active" ] && [ "${EASYTIER_STATUS}" = "active" ]; then
  echo "=================================================================="
  echo "🎉 部署成功！全系统四大核心服务均在后台健康运行并配置开机自启！"
  echo "  - 硬件按键与 LED:    /run/debugbox_gpio.sock (gpio_ctl 调试)"
  echo "  - 串口 24h 存盘日志:  /var/log/debugbox/serial_*.log"
  echo "  - 工程师双向控制台:  debug-console (支持: debug-console uart1 / uart2)"
  echo "  - 扩展多串口服务:    sudo systemctl enable --now debugbox-serial@uart2.service"
  echo "  - 异地 P2P 穿透网卡: 10.144.144.2 (EasyTier 虚拟内网)"
  echo "=================================================================="
else
  echo "⚠️ 警告: 部分服务状态异常，请运行 journalctl -xe 查看报错信息。"
  exit 1
fi
