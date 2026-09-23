# 工业远程调试伴侣盒 (Raspberry Pi 4B)

用于嵌入式设备现场远程调试的伴侣盒。将被监控设备的串口输出保存至树莓派本地，并支持工程师异地实时监控与敲命令交互。专为频繁变换工厂的场景打造，提供零门槛手机配网与物理按键一键重置。

---

## 阶段一：手机 AP 配网与 C 语言 GPIO 硬件驱动模块（已完成并实测）

### 1. 硬件接线定义

| 物理元件 | 树莓派 40Pin 引脚 | GPIO 编号 | 接线方式与电路说明 |
| :--- | :--- | :--- | :--- |
| **复位按键** | **Pin 11** | **GPIO 17** | 一端接 Pin 11 (GPIO 17)，另一端接 Pin 9 或 Pin 14 (GND)。内部已启用上拉电阻，按下为低电平。 |
| **状态指示灯 (LED)** | **Pin 13** | **GPIO 27** | 阳极（长脚）接 Pin 13 (GPIO 27)，阴极（短脚）串联 **330Ω 电阻** 接 Pin 39 (GND)。 |

---

### 2. 机身指示灯（LED）状态含义

| LED 闪烁状态 | 对应设备状态 | 工人/工程师操作指引 |
| :--- | :--- | :--- |
| **中速快闪 (0.25s开, 0.25s关)**| **开机正在尝试连网** | 系统正在尝试搜索车间 Wi-Fi，请稍候 |
| **悠闲慢闪 (0.8s开, 0.8s关)** | **配网热点已开启 (无网)** | 手机搜索并连接 `DebugBox-XXXX` 热点进行 Wi-Fi 配置 |
| **心跳双跳 (双闪一下歇一下)** | **工作就绪 (已联网)** | 树莓派已成功连上网络，异地工程师随时可以连入 |
| **急促狂闪 (0.08s狂闪)** | **复位确认** | 长按按键已满 3 秒，网络重置已触发，请松开按键 |

---

### 3. 工人手机配网操作流程（免装 App）

1. **开机 / 触发配网**：
   - 树莓派开机若未连上已知网络（或长按机身按键 **3 秒**），LED 转为**慢闪**。
2. **手机连热点**：
   - 用手机 Wi-Fi 搜索以 **`DebugBox-`** 开头的热点（如 `DebugBox-9A4B`）并连接（免密）。
3. **打开配网页**：
   - 手机自动弹出登录页面；若未弹出，在手机浏览器输入 **`192.168.4.1`**。
4. **配置车间 Wi-Fi**：
   - 页面自动列出附近扫描到的 Wi-Fi；
   - 点击车间 Wi-Fi，输入密码，点击 **“保存并连接”**。
5. **观察机身**：
   - 热点将自动关闭，机身 LED 转为**心跳双跳**即代表配网成功！

---

### 4. 目录结构

```text
.
├── .gitignore                         # Git 忽略文件规则
├── README.md                          # 项目工程文档与接线指引
├── config/
│   ├── config.yaml                    # 全局配置参数（引脚、SSID、IP、超时）
│   └── systemd/                       # systemd 开机自启服务
│       ├── debugbox-gpio.service      # C 语言硬件守护进程单元
│       └── debugbox-network.service   # 网络总控守护进程单元
├── src/
│   ├── c_gpio/                        # 【纯 C 实现】硬件极速驱动模块
│   │   ├── gpio_controller.h          # 核心头文件 (状态枚举、API声明)
│   │   ├── gpio_controller.c          # libgpiod v2 + pthread 双线程驱动
│   │   ├── main.c                     # 守护进程入口与 IPC 套接字服务
│   │   └── Makefile                   # 本地 GCC 编译构建规则
│   ├── gpio_manager/                  # IPC 通信适配层
│   │   ├── c_gpio_client.py           # Python 调用 C 守护进程的套接字客户端
│   │   └── button_led.py              # 纯 Python 版硬件驱动（备用/测试）
│   ├── wifi_portal/                   # 手机配网模块
│   │   ├── wifi_controller.py         # NetworkManager (nmcli) 控制层
│   │   ├── server.py                  # 纯 Python 标准库 HTTP 服务 (Captive Portal)
│   │   └── templates/
│   │       └── index.html             # 针对手机端优化的自适应配网单页
│   └── network_service.py             # 核心守护进程（协调网络、按键与LED）
├── scripts/
│   ├── install_network_service.sh     # 树莓派一键编译、安装与注册服务脚本
│   ├── on_network_reset.sh            # 物理按键触发网络重置动作脚本
│   └── test_hardware.py               # 硬件独立自检程序
└── tests/
    └── test_step1.py                  # 第一步单元逻辑自动化测试脚本
```

---

### 5. 在树莓派上构建与部署

在树莓派终端中执行：

```bash
# 一键编译安装 C 守护进程并注册自启服务
sudo bash scripts/install_network_service.sh

# 手动查询或控制 LED 状态
gpio_ctl get
gpio_ctl set ONLINE
gpio_ctl set AP_MODE

# 查看服务运行日志
sudo journalctl -u debugbox-gpio.service -f
sudo journalctl -u debugbox-network.service -f
```
