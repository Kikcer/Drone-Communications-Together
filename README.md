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

核心参数定义在 `broadcast_protocol.h` 中，修改后**必须重新编译**（执行 `make -f makefile_broadcast clean && make -f makefile_broadcast all`）。

### 主要参数

| 参数宏 | 默认值 | 说明 | 调整建议 |
|--------|--------|------|----------|
| `MULTICAST_GROUP` | "239.255.1.1" | 组播 IP 地址 | 需确保网络支持组播 |
| `MULTICAST_PORT` | 9000 | 组播端口 | 避免与其他服务冲突 |
| `MAX_CHUNK_SIZE` | 1024 | 单个数据块大小 (Bytes) | 建议小于 MTU (通常 1500) 以避免 IP 分片 |
| `WINDOW_SIZE` | 64 | 滑动窗口大小 (Blocks) | 内存受限时调小；高吞吐时调大 (最大不要超过 64 位图限制) |
| `SIMULATE_PACKET_LOSS` | 0 | 模拟丢包率 (0-100) | 仅用于测试环境模拟恶劣网络 |

### 高级调优参数

| 参数宏 | 默认值 | 说明 | 调整建议 |
|--------|--------|------|----------|
| `NACK_TIMEOUT_MS` | 15 | NACK 随机退避最大延迟 (ms) | 节点越多建议设得越大，以分散 NACK 响应 |
| `STATUS_REQ_INTERVAL` | 500 | 状态查询间隔 (ms) | 如果 NACK 回复较慢，需增大此值防止 Master 过早重试 |
| `MAX_RETRANS_ROUNDS` | 10 | 最大重传轮数 | 高丢包环境下增加此值，确保传输成功率 |
| `ANNOUNCE_REPEAT_COUNT` | 5 | 启动报文重复次数 | 确保所有节点都能收到初始通知 |

### 如何修改

1. 打开 `broadcast_protocol.h` 文件。
2. 找到 `#define` 宏定义部分。
3. 修改对应的值（例如将 `SIMULATE_PACKET_LOSS` 改为 `10` 以模拟 10% 丢包）。
4. 保存文件并重新编译项目。

## 📂 项目结构

- `master.c`: 发送端核心逻辑（文件读取、窗口管理、重传处理）。
- `receiver.c`: 接收端核心逻辑（数据接收、位图记录、NACK 生成）。
- `common.c`: 传输层封装（UDP Socket、多线程收发队列）。
- `broadcast_protocol.h`: 通信协议定义（消息头、数据包结构）。
- `makefile_broadcast`: 编译配置文件。
