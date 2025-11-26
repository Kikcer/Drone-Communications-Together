# UAV 可靠广播文件传输系统 (UAV Reliable File Broadcast System)

这是一个基于 UDP 组播实现的高可靠文件传输系统，专为无人机集群通信环境设计。该系统允许主节点（Master）将大文件高效广播给多个接收节点（Receiver），并采用 NACK 抑制机制来优化带宽利用率。

## 🌟 核心特性

- **UDP 组播传输**: 支持一对多同时传输，节省网络带宽。
- **可靠性保障**: 采用滑动窗口 + NACK 重传机制，确保数据零丢失。
- **NACK 抑制**: 接收节点在发送 NACK 前会进行随机退避，避免“反馈风暴”。
- **完整性校验**: 每个数据块包含 CRC16 校验，文件传输结束进行全量 Hash 校验。
- **独立运行**: 不依赖外部复杂库，纯 C 实现，易于移植。

## 🚀 快速开始

### 1. 编译项目

由于本项目包含多个模块，请使用专用的 makefile 进行编译：

```bash
make -f makefile_broadcast all
```

编译成功后将生成以下可执行文件：
- `master`: 发送端程序
- `receiver`: 接收端程序

### 2. 运行测试

#### 自动测试（推荐）

本项目提供了一个自动化测试脚本，可以在单机上模拟 1 个 Master 和 3 个 Receiver 的通信过程：

```bash
./test_broadcast.sh
```

该脚本会自动：
1. 创建测试文件
2. 启动多个接收端进程
3. 启动发送端进行广播
4. 验证接收到的文件完整性


如果想开启丢包，请使用：
````
./test_with_loss.sh "丢包率（比如5，表示5%概率丢包）"
````
#### 手动运行

**第一步：启动接收端**

在不同的终端窗口中启动接收端（指定不同的 UAV ID）：

```bash
# 终端 1
./receiver 1

# 终端 2
./receiver 2
```

**第二步：启动发送端**

在新的终端中启动发送端，指定要发送的文件：

```bash
# 创建一个测试文件 (如果还没有)
dd if=/dev/urandom of=test_data.bin bs=1M count=1

# 启动广播 (文件ID设为 1)
./master test_data.bin 1
```

## ⚙️ 配置说明

核心参数定义在 `broadcast_protocol.h` 中，可根据实际网络环境进行调整：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `MULTICAST_GROUP` | "239.255.1.1" | 组播 IP 地址 |
| `MULTICAST_PORT` | 9000 | 组播端口 |
| `MAX_CHUNK_SIZE` | 1024 | 单个数据块大小 (Bytes) |
| `WINDOW_SIZE` | 64 | 滑动窗口大小 (Blocks) |
| `SIMULATE_PACKET_LOSS` | 0 | 模拟丢包率 (0-100)，用于测试 |

## 📂 项目结构

- `master.c`: 发送端核心逻辑（文件读取、窗口管理、重传处理）。
- `receiver.c`: 接收端核心逻辑（数据接收、位图记录、NACK 生成）。
- `common.c`: 传输层封装（UDP Socket、多线程收发队列）。
- `broadcast_protocol.h`: 通信协议定义（消息头、数据包结构）。
- `makefile_broadcast`: 编译配置文件。
