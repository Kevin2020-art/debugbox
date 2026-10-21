# 工业远程调试伴侣盒 (DebugBox for Raspberry Pi 4B)

用于嵌入式设备现场远程调试的高可靠伴侣盒。将被监控目标板的串口输出 24 小时高精存盘（毫秒级时间戳 + 20MB 自动轮转），并支持异地工程师穿透工业防火墙实现远程 SSH、实时监看与双向 Shell 敲命令交互。专为频繁变换工厂现场的恶劣工业环境打造，提供工人手机热点配网（Captive Portal）、物理按键自愈重置、以及物理 USB 槽位防漂移的多串口扩展能力。

---

## 核心系统架构图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                      【异地工程师 PC (Mac / Windows)】                  │
│   ping 10.144.144.2  │  ssh dong@10.144.144.2  │  debug-console uart1/2 │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │ (EasyTier P2P 加密隧道 / 无视工厂防火墙)
┌───────────────────────────────▼─────────────────────────────────────────┐
│                 【树莓派 4B 工业调试盒子 (10.144.144.2)】                │
│                                                                         │
│  [网络层] EasyTier P2P Mesh Daemon (开机自启, tun0)                     │
│  [硬件层] C 语言 GPIO 守护进程 (libgpiod v2, 按键 3s重置 / LED状态机)    │
│  [配网层] Python Network Supervisor (WiFi断线自愈, 手机热点强制弹窗)     │
│  [串口层] 纯 C 异步 poll() 守护进程 (单实例 1.5MB 内存, 0.0% CPU)        │
│          ├── 24h 毫秒级时间戳日志切片 (/var/log/debugbox/uartX/*.log)   │
│          └── UNIX Domain Socket 广播 Hub (/run/debugbox_uartX.sock)     │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │ (USB 物理孔位固化 /dev/target_uart1 ~ uart4)
┌───────────────────────────────▼─────────────────────────────────────────┐
│              【被测目标设备 (Linux 主板 / MCU / 4G 模组)】              │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 一、 硬件接线与物理插孔定义

### 1. 机身按键与指示灯（GPIO）

| 物理元件 | 树莓派 40Pin 引脚 | GPIO 编号 | 接线方式与电路说明 |
| :--- | :--- | :--- | :--- |
| **复位按键** | **Pin 11** | **GPIO 17** | 一端接 Pin 11 (GPIO 17)，另一端接 Pin 9 或 Pin 14 (GND)。内部开启上拉电阻，按下为低电平。 |
| **状态指示灯 (LED)** | **Pin 13** | **GPIO 27** | 阳极（长脚）接 Pin 13 (GPIO 27)，阴极（短脚）串联 **330Ω 电阻** 接 Pin 39 (GND)。 |

### 2. 串口接线 3 原则（目标单片机/主板 ↔ USB-TTL 小板）

* **GND 必须接（共地）**：树莓派 USB 串口的 `GND` 必须接到目标设备的 `GND`。
* **TX/RX 交叉相连**：树莓派 `TX` ➔ 目标板 `RX`；树莓派 `RX` ➔ 目标板 `TX`。
* **VCC 悬空不接**：目标板若自身有供电，**切勿接 VCC（3.3V/5V）**，防止两端电源倒灌烧坏芯片。

### 3. 多串口物理 USB 孔位绑定（防设备名漂移）

树莓派 4B 后视 USB 插槽与系统设备名已通过 UDEV 规则永久物理绑定（实测固化）：

```
树莓派后盖 USB 视角（接口朝向自己）：
┌──────────┬─────────────────────────────────┬─────────────────────────────────┐
│          │  蓝色 USB 3.0 上口 (总线 1-1.1)  │  黑色 USB 2.0 上口 (总线 1-1.3)  │
│  以太网口  │  ──> 固定绑定为 /dev/target_uart1 │  ──> 固定绑定为 /dev/target_uart3 │
│  (RJ45)  ├─────────────────────────────────┼─────────────────────────────────┤
│          │  蓝色 USB 3.0 下口 (总线 1-1.2)  │  黑色 USB 2.0 下口 (总线 1-1.4)  │
│          │  ──> 固定绑定为 /dev/target_uart2 │  ──> 固定绑定为 /dev/target_uart4 │
└──────────┴─────────────────────────────────┴─────────────────────────────────┘
```
*(注：1 号串口同时保留软链接 /dev/target_uart，单串口向下完全兼容)*

---

## 二、 机身指示灯（LED）状态机说明

现场无需显示器，通过观察外壳单个双色/单色 LED 的闪烁模式即可 1 秒判断系统健康状态：

| LED 闪烁节拍 | 对应设备状态 | 现场含义与运维指引 |
| :--- | :--- | :--- |
| **慢闪 (0.8s 亮 / 0.8s 灭)** | **配网热点已开启 (AP 模式)** | 未连上现场 Wi-Fi，手机搜索并连接 `DebugBox-xxxx` 热点进行配网。 |
| **快闪 (0.25s 亮 / 0.25s 灭)** | **正在尝试连网 (Connecting)** | 正在连接现场 Wi-Fi 或正在建立加密隧道，请稍等片刻。 |
| **心跳双闪 (闪两下停一下)** | **在线就绪 (Online Ready)** | Wi-Fi 与 EasyTier 异地加密隧道均已打通，远程工程师可随时连入！ |
| **极速狂闪 (0.08s 狂闪)** | **硬件复位确认 (Resetting)** | 长按机身按键已满 3 秒，网络重置已触发，松开按键即可重新配网。 |

---

## 三、 现场工人操作指南（零门槛手机配网）

### 1. 自动感应热点（去到新工厂/新现场）
* 将盒子插上电源通电开机；
* 树莓派开机 6 秒内若未找到已知网络，**自动进入 AP 配网模式**（LED 自动变为**慢闪**），无需按任何按键！

### 2. 手机免 App 配网流程
1. 打开手机 Wi-Fi，搜索连接以 **`DebugBox-`** 开头的热点（如 `DebugBox-d83a`，免密直接连入）；
2. 手机会自动弹出网页（强制弹窗 Captive Portal）；若未弹出，在手机浏览器中打开 **`192.168.4.1`**；
3. 页面会自动列出扫描到的车间 Wi-Fi 列表，点击对应 Wi-Fi 并输入密码，点击 **“保存并连接”**；
4. 盒子自动保存并接入车间网络，机身 LED 变为**心跳双跳**即代表大功告成！

### 3. 现场误操作重置（硬件按键）
* 若 Wi-Fi 密码输错或需要强制更换网络，**按住机身按键 3~5 秒**（直至 LED 狂闪后松开）；
* 系统自动清除记忆并重新拉起配网热点。

---

## 四、 远程工程师使用指南（异地穿透与控制台交互）

### 1. 工程师 PC 端启动 EasyTier

前往 [EasyTier 官网 Releases](https://github.com/EasyTier/EasyTier/releases) 下载对应系统的客户端解压。

在 PC 终端执行一行启动命令（需要管理员权限以创建虚拟 TUN 网卡）：

**macOS 终端：**
```bash
# 首次运行解除苹果 Gatekeeper 限制：
xattr -d com.apple.quarantine ./easytier-* 2>/dev/null || true
codesign --force --deep --sign - ./easytier-core

# 启动异地虚拟网卡：
sudo ./easytier-core \
  --network-name debugbox_mesh \
  --network-secret debugbox_pass_2026 \
  --ipv4 10.144.144.1 \
  -e tcp://225284.xyz:11010
```

**Windows (管理员身份运行 PowerShell)：**
```powershell
.\easytier-core.exe `
  --network-name debugbox_mesh `
  --network-secret debugbox_pass_2026 `
  --ipv4 10.144.144.1 `
  -e tcp://225284.xyz:11010
```

> **注意（Clash Verge TUN 模式兼容）**：
> 若电脑开启了 Clash Verge 的 TUN 模式，请在 Clash 中将 `225284.xyz` 设为 `DIRECT` 直连，并在 TUN 的绕过网段中包含 `10.144.144.0/24`，避免 Fake-IP 拦截打洞握手。

---

### 2. 异地远程接入与调试

保持 EasyTier 终端在后台运行，新开一个终端窗口：

#### ① 测试连通性
```bash
ping 10.144.144.2
```

#### ② 远程 SSH 登录盒子
```bash
ssh dong@10.144.144.2
# 默认密码: raspberry
```

#### ③ 运行双向交互控制台 `debug-console`
登录进树莓派后，直接运行控制台接入目标单片机：

```bash
# 1. 查看当前系统中正在运行的所有串口通道列表
debug-console --list

# 2. 接入默认或 1 号串口通道 (SoC 调试控制台)
debug-console uart1
# 或者简写为:
debug-console 1

# 3. 接入 2 号串口通道 (MCU 遥测控制台)
debug-console uart2
```

* **特性**：敲一键送一键（Raw 透传），支持 Tab 自动补全、上下键历史指令；
* **随时安全退出**：在控制台中按下快捷键 **`Ctrl + ]`** 即可安全退出，回到树莓派 shell，**后台 24h 持续存盘绝不中断**！

---

## 五、 多串口通道扩展与运维

系统采用 **Systemd 模板服务 (`debugbox-serial@.service`)** 架构，可按需任意增加串口，每个通道独立占用 1.5MB 内存，崩溃与拔线故障彻底隔离。

### 1. 启动指定通道的守护进程
```bash
# 启动 1 号串口 (蓝色 USB 3.0 上口 / 靠网口)
sudo systemctl enable --now debugbox-serial@uart1.service

# 启动 2 号串口 (蓝色 USB 3.0 下口 / 靠网口)
sudo systemctl enable --now debugbox-serial@uart2.service

# 启动 3 号串口 (黑色 USB 2.0 上口)
sudo systemctl enable --now debugbox-serial@uart3.service

# 启动 4 号串口 (黑色 USB 2.0 下口)
sudo systemctl enable --now debugbox-serial@uart4.service
```

### 2. 自定义串口波特率
默认波特率为 `115200`。若某通道（如 4G 模组或 GPS）需要 `9600` 波特率，可直接传参启动：
```bash
# 手动以前台/后台模式指定波特率与通道：
debugbox_serial_daemon -c uart3 -b 9600
```

---

## 六、 日志存盘、时间戳与拉取分析

### 1. 24 小时高精度日志格式
所有接收到的数据均带有纳秒时钟格式化的毫秒时间戳：
```text
[2026-10-20 08:30:15.124] Kernel panic - not syncing: Fatal exception
[2026-10-20 08:30:15.125] CPU: 0 PID: 1 Comm: swapper/0 Not tainted 6.1.0 #1
[2026-10-20 08:30:15.126] Hardware name: BCM2835
[2026-10-20 08:30:15.127] [<c010d8e4>] (unwind_backtrace) from [<c010a3ac>] (show_stack+0xb/0xc)
[2026-10-20 08:30:20.005][INPUT] reboot -f
```
* 工程师在控制台输入的指令会自动打上 `[INPUT]` 标记留痕审计；
* 单日志文件达到 **20MB** 时自动轮转切片，最多保留最新的 **50 个历史文件**（最多保留 1GB，防止撑爆 SD 卡）。

### 2. 日志目录存放结构
* **1 号串口日志**：`/var/log/debugbox/uart1/serial_YYYYMMDD_HHMMSS.log`
* **2 号串口日志**：`/var/log/debugbox/uart2/serial_YYYYMMDD_HHMMSS.log`
* **默认单串口日志**：`/var/log/debugbox/serial_*.log`

### 3. 工程师本地一键批量拉取日志
在异地 PC 电脑上，直接通过虚拟 IP 将现场整天的日志拷回本地：
```bash
# 拉取 1 号串口的所有日志到本地分析：
scp -r dong@10.144.144.2:/var/log/debugbox/uart1/ ./local_logs_uart1/
```

---

## 七、 统一部署与日常运维命令

### 1. 生产环境一键部署脚本
在树莓派中拉取最新代码后，执行全自动部署脚本：
```bash
sudo bash scripts/deploy.sh
```
脚本将自动完成：
1. 编译纯 C 硬件 GPIO 守护进程与串口双向 Hub；
2. 自动检查并安装 EasyTier P2P 穿透核心组件；
3. 安装 UDEV 物理 USB 槽位固化规则；
4. 注册并配置全部 Systemd 开机自启服务。

### 2. 常用服务状态检查
```bash
# 查看硬件 GPIO 守护进程状态
sudo systemctl status debugbox-gpio.service

# 查看网络与手机配网守护进程状态
sudo systemctl status debugbox-network.service

# 查看默认串口守护进程状态
sudo systemctl status debugbox-serial.service

# 查看指定通道串口服务状态
sudo systemctl status debugbox-serial@uart1.service

# 查看 EasyTier 虚拟组网状态
sudo systemctl status debugbox-easytier.service

# 查看 EasyTier 虚拟网卡节点与物理延迟
easytier-cli node
easytier-cli peer
```

---

## 八、 代码仓库完整目录结构

```text
.
├── config/
│   ├── 00-captive-portal.conf         # NetworkManager DNS 泛解析劫持规则
│   ├── 99-target-serial.rules         # UDEV 物理 USB 插孔与多通道固化映射规则
│   ├── config.yaml                    # 全局参数配置文件 (引脚/IP/EasyTier/串口通道)
│   └── systemd/                       # Systemd 开机自启单元
│       ├── debugbox-easytier.service  # EasyTier 异地 P2P 虚拟网卡服务
│       ├── debugbox-gpio.service      # C 语言硬件按键与 LED 守护进程
│       ├── debugbox-network.service   # 网络监控与手机热点配网服务
│       ├── debugbox-serial.service    # 串口默认守护进程
│       └── debugbox-serial@.service   # 串口多通道模板服务 (debugbox-serial@uartX)
├── scripts/
│   ├── deploy.sh                      # 全系统生产环境一键编译、安装与部署脚本
│   ├── on_network_reset.sh            # 物理按键触发网络重置脚本
│   └── test_hardware.py               # 硬件引脚独立自检脚本
├── src/
│   ├── c_gpio/                        # 【纯 C】硬件极速驱动模块 (libgpiod v2, 1.5MB 内存)
│   │   ├── gpio_controller.c/.h       # 按键双跳去抖、LED 状态机与非阻塞定时器
│   │   ├── main.c                     # 守护进程入口与 UNIX Socket IPC 服务
│   │   └── Makefile                   # GCC 编译构建规则
│   ├── c_serial/                      # 【纯 C】24h 串口日志与交互 Hub (poll 异步驱动)
│   │   ├── include/
│   │   │   ├── client_hub.h           # UNIX Domain Socket 广播与并发管理
│   │   │   ├── log_rotator.h          # 毫秒时间戳格式化与 20MB 文件轮转
│   │   │   └── serial_port.h          # POSIX termios 8N1 RAW 模式与自动重连
│   │   ├── src/
│   │   │   ├── client_hub.c
│   │   │   ├── console_cli.c          # debug-console 终端透传工具 (Ctrl+] 退出)
│   │   │   ├── log_rotator.c
│   │   │   ├── main.c                 # debugbox_serial_daemon 主进程 (-c 通道支持)
│   │   │   └── serial_port.c
│   │   └── Makefile
│   ├── gpio_manager/                  # Python 与 C 守护进程通信适配器
│   ├── wifi_portal/                   # 手机扫码配网 HTTP 服务与自适应前端网页
│   └── network_service.py             # 网络总控服务 (自愈检测、AP 切换与 EasyTier 联动)
└── README.md                          # 项目工程完整架构与使用说明文档
```
