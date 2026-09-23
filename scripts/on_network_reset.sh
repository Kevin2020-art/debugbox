#!/bin/bash
# ==============================================================================
# 网络重置动作脚本：由 C 语言 GPIO 守护进程在长按 3 秒时调用
# ==============================================================================

echo "[$(date '+%Y-%m-%d %H:%M:%S')] 收到物理按键重置触发信号..." >> /tmp/debugbox_reset.log

# 1. 删除所有已保存的 Wi-Fi 连接 (安全处理包含空格的 SSID)
nmcli -t -f NAME,TYPE connection show | while IFS=: read -r conn type; do
    if [ "$type" = "802-11-wireless" ] && [ "$conn" != "DebugBox-Hotspot" ]; then
        echo "删除历史 Wi-Fi 配置: $conn" >> /tmp/debugbox_reset.log
        nmcli connection delete "$conn" >> /tmp/debugbox_reset.log 2>&1 || true
    fi
done

# 2. 重启网络总控服务，自动切入 AP 配网热点
systemctl restart debugbox-network.service >> /tmp/debugbox_reset.log 2>&1 || true
