#!/bin/bash
# ==============================================================================
# 网络重置动作脚本：由 C 语言 GPIO 守护进程在长按 3 秒时调用
# ==============================================================================

echo "[$(date '+%Y-%m-%d %H:%M:%S')] 收到物理按键重置触发信号..." >> /tmp/debugbox_reset.log

# 1. 删除所有已保存的 Wi-Fi 连接
for conn in $(nmcli -t -f NAME,TYPE connection show | grep ":802-11-wireless$" | cut -d: -f1); do
    if [ "$conn" != "DebugBox-Hotspot" ]; then
        echo "删除历史 Wi-Fi 配置: $conn" >> /tmp/debugbox_reset.log
        nmcli connection delete "$conn" 2>&1 >> /tmp/debugbox_reset.log || true
    fi
done

# 2. 如果 Python 网络服务正在运行，重启网络服务进入配网热点
systemctl restart debugbox-network.service 2>&1 >> /tmp/debugbox_reset.log || true
