# UAV 可靠广播文件传输系统

## 📡 项目概述

这是一个基于 UDP 组播的可靠文件传输系统，专为无人机（UAV）编队设计。系统实现了一个高效的广播协议，允许一台主无人机（Master）将大文件可靠地传输给多台接收无人机（Receivers），同时优化了带宽使用和避免了ACK风暴问题。

### 核心特性

✅ **真实广播通信** - 使用UDP组播，所有接收方同时收到数据  
✅ **内存友好** - 窗口化接收，只需跟踪当前窗口状态  
✅ **NACK抑制机制** - 避免反馈风暴，智能去重  
✅ **自动重传** - 缺失数据块自动重传  
✅ **完整性校验** - CRC + 文件hash双重验证  
✅ **高可靠性** - 多轮状态查询确保完整传输  

---

## 🏗️ 协议设计

### 五阶段传输流程

```
阶段1: SESSION_ANNOUNCE
   ↓ Master广播会话启动消息
   
阶段2: DATA_BROADCAST  
   ↓ Master顺序广播所有数据块
   
阶段3: STATUS_REQ / NACK
   ↓ Master查询各窗口状态
   ↓ Receiver智能反馈缺失块（NACK抑制）
   
阶段4: RETRANSMISSION
   ↓ Master重传缺失块
   ↓ 重复阶段3-4直到所有窗口完成
   
阶段5: END
   ↓ Master发送结束消息
   ↓ Receiver验证文件完整性
```

### 窗口与块的概念

```
文件 (1MB) 
   ↓ 切分为 1024 个块 (每块 1KB)
   
┌─────────────────────────────────────┐
│ 窗口0: chunk 0~63    (64个块)      │
│ 窗口1: chunk 64~127  (64个块)      │
│ 窗口2: chunk 128~191 (64个块)      │
│ ...                                 │
│ 窗口15: chunk 960~1023 (64个块)    │
└─────────────────────────────────────┘

每个窗口用 64-bit bitmap 跟踪接收状态
```

### NACK抑制机制

```
Master广播: STATUS_REQ (window=5, round=1)
                ↓
      ┌─────────┼─────────┐
      ↓         ↓         ↓
   UAV1      UAV2      UAV3
 (缺块5,7)  (缺块5)  (缺块5,7,9)
   ↓         ↓         ↓
随机延迟   随机延迟   随机延迟
 10ms      25ms      40ms
   ↓         ↓         ↓
发送NACK  监听到UAV1  监听到UAV1
         的NACK→     的NACK→
         抑制发送    部分抑制
                    只报告块9

结果：减少了重复NACK，节省带宽
```

---

## 📦 文件结构

```
broadcast_protocol.h    # 协议定义（消息类型、数据结构）
common.c               # 工具函数（CRC、组播、时间）
master.c               # 主发送方实现
receiver.c             # 接收方实现（含NACK抑制）
makefile_broadcast     # 编译配置
README_BROADCAST.md    # 本文档
```

---

## 🚀 快速开始

### 步骤1：编译程序

```bash
make -f makefile_broadcast all
```

输出：
- `master` - 主发送方可执行文件
- `receiver` - 接收方可执行文件

### 步骤2：创建测试文件

```bash
# 创建100KB测试文件
make -f makefile_broadcast test-file
```

或手动创建：
```bash
dd if=/dev/urandom of=test_file.bin bs=1K count=100
```

### 步骤3：启动接收方

在**不同的终端窗口**中启动多个接收方：

```bash
# 终端1：UAV 1
./receiver 1

# 终端2：UAV 2
./receiver 2

# 终端3：UAV 3
./receiver 3

# ... 可以启动更多接收方
```

输出示例：
```
========================================
  UAV File Broadcast Receiver
  UAV ID: 1
========================================
[UAV 1] Listening for broadcasts on 239.255.1.1:9000
[UAV 1] Message receiver thread started.
```

### 步骤4：启动主发送方

在新终端中运行：

```bash
./master test_file.bin 1
```

参数说明：
- `test_file.bin` - 要广播的文件
- `1` - 文件ID（可选，默认为1）

---

## 📊 运行输出示例

### Master 输出

```
========================================
  UAV File Broadcast Master
========================================
[Master] Session initialized:
  File: test_file.bin
  Size: 102400 bytes
  Total chunks: 100
  Total windows: 2
  Window size: 64 chunks

[Master] Sending SESSION_ANNOUNCE...
[Master] Starting data broadcast...
[Master] Broadcast progress: 100/100 chunks (100.0%)
[Master] Initial broadcast completed.

[Master] Entering status query and retransmission phase...
[Master] Sending STATUS_REQ for window 0 (round 0)
[Master] Received NACK from UAV 2 for window 0, missing bits: 3
[Master] Retransmitting 3 chunks for window 0
[Master] Window 0 completed.

[Master] Sending STATUS_REQ for window 1 (round 0)
[Master] Window 1 completed.

[Master] Status query and retransmission phase completed.
[Master] Sending END message (file_hash=0x1A2B3C4D)...
[Master] All phases completed. Press Ctrl+C to exit.
```

### Receiver 输出

```
[UAV 2] Session initialized:
  File: test_file.bin
  Total chunks: 100
  Total windows: 2
  Output: received_test_file.bin

[UAV 2] Progress: 100/100 chunks (100.0%)
[UAV 2] Window 0 completed and saved.
[UAV 2] Window 1 completed and saved.

[UAV 2] Received END message, verifying file...
[UAV 2] ✓ File transfer completed successfully!
[UAV 2] ✓ Hash verified: 0x1A2B3C4D
[UAV 2] ✓ File saved as: received_test_file.bin
```

---

## ⚙️ 配置参数

在 `broadcast_protocol.h` 中可以调整以下参数：

```c
#define MULTICAST_GROUP     "239.255.1.1"    // 组播地址
#define MULTICAST_PORT      9000              // 组播端口
#define MAX_CHUNK_SIZE      1024              // 块大小(字节)
#define WINDOW_SIZE         64                // 窗口大小(块数)
#define MAX_UAVS            32                // 最大UAV数量
#define NACK_TIMEOUT_MS     50                // NACK退避最大延迟
#define STATUS_REQ_INTERVAL 200               // 状态查询间隔(ms)
#define MAX_RETRANS_ROUNDS  5                 // 最大重传轮数
```

### 参数调优建议

| 场景 | 推荐配置 |
|-----|---------|
| **高速网络** | `NACK_TIMEOUT_MS=20`, `STATUS_REQ_INTERVAL=100` |
| **高丢包率** | `MAX_RETRANS_ROUNDS=10`, `STATUS_REQ_INTERVAL=300` |
| **大文件传输** | `MAX_CHUNK_SIZE=4096`, `WINDOW_SIZE=128` |
| **内存受限** | `WINDOW_SIZE=32`, `MAX_CHUNK_SIZE=512` |

---

## 🧪 测试与验证

### 基本功能测试

```bash
# 1. 编译
make -f makefile_broadcast all

# 2. 创建测试文件
dd if=/dev/urandom of=test.bin bs=1M count=1

# 3. 启动3个接收方
./receiver 1 &
./receiver 2 &
./receiver 3 &

# 4. 启动发送方
./master test.bin 1

# 5. 等待传输完成，验证文件
md5sum test.bin received_test.bin
```

### 丢包测试

使用 `tc` 命令模拟网络丢包：

```bash
# 模拟10%丢包率
sudo tc qdisc add dev lo root netem loss 10%

# 运行测试
./master test.bin 1

# 恢复网络
sudo tc qdisc del dev lo root
```

### 性能测试

```bash
# 测试不同文件大小的传输时间
for size in 1 5 10 50 100; do
    dd if=/dev/urandom of=test_${size}MB.bin bs=1M count=$size
    time ./master test_${size}MB.bin 1
done
```

---

## 📈 性能指标

### 理论带宽

```
每块大小: 1KB
发送间隔: 1ms
理论吞吐: 1KB / 1ms = 1MB/s
```

实际性能受以下因素影响：
- 网络延迟和抖动
- 丢包率
- CPU处理能力
- 磁盘I/O速度

### 内存占用

```
Master:
  窗口状态: 总窗口数 × 16字节
  例：1000窗口 → 16KB

Receiver:
  窗口缓冲: 活跃窗口数 × 64KB
  例：保持3个窗口 → 192KB
```

---

## 🔧 故障排查

### 问题1：接收方收不到数据

**原因**：防火墙阻止组播

**解决**：
```bash
# 临时关闭防火墙
sudo ufw disable

# 或添加规则
sudo ufw allow 9000/udp
```

### 问题2：hash校验失败

**原因**：数据损坏或接收不完整

**解决**：
1. 检查日志中的 "Progress: X/Y chunks"
2. 增加重传轮数：`MAX_RETRANS_ROUNDS`
3. 延长状态查询间隔：`STATUS_REQ_INTERVAL`

### 问题3：NACK风暴（大量重复NACK）

**原因**：NACK抑制机制失效

**解决**：
1. 增加 `NACK_TIMEOUT_MS` 以扩大退避窗口
2. 检查系统时钟是否正常

### 问题4：传输速度慢

**原因**：发送速率受限

**解决**：
在 `master.c` 中调整：
```c
// 减少发送间隔（当前1ms）
usleep(500);  // 改为0.5ms → 2MB/s
```

---

## 🆚 与原系统对比

| 特性 | 原系统（UWB测距仿真） | 新系统（文件广播） |
|-----|---------------------|------------------|
| **通信方式** | TCP中心化 | UDP组播 |
| **拓扑结构** | 星型（Center-Drone） | 扁平化（Master-Receivers） |
| **应用场景** | 测距算法仿真 | 文件/固件分发 |
| **广播方式** | 伪广播（逐个发送） | 真实广播（同时到达） |
| **可靠性机制** | 无（基于仿真数据） | NACK + 重传 |
| **反馈机制** | 响应计数 | NACK抑制 |
| **内存占用** | 全局状态跟踪 | 窗口化状态 |

---

## 🌟 高级功能扩展

### 1. 动态速率调整

根据NACK数量自动调整发送速率：

```c
// 在master.c中添加
int nack_count_last_window = /* 统计 */;
int delay_us = (nack_count > 10) ? 2000 : 1000;
usleep(delay_us);
```

### 2. 邻居互补

接收方之间点对点补充缺失块：

```c
// 在receiver.c中添加
if (file_incomplete && heard_END) {
    request_from_neighbor(missing_chunks);
}
```

### 3. 优先级传输

为不同文件设置优先级：

```c
typedef struct {
    uint16_t file_id;
    uint8_t priority;  // 0-255
} SessionAnnounce;
```

### 4. 加密传输

添加数据加密层：

```c
// 在发送前加密
encrypt_chunk(chunk->data, chunk->data_len, encryption_key);

// 在接收后解密
decrypt_chunk(chunk->data, chunk->data_len, encryption_key);
```

---

## 📚 参考资料

### 相关协议
- **IEEE 802.11** - 无线局域网标准
- **NORM (RFC 5740)** - NACK-Oriented可靠组播
- **PGM (RFC 3208)** - 实用通用组播
- **FLUTE (RFC 6726)** - 单向传输

### 学术论文
- *Reliable Multicast Transport Protocol (RMTP)*
- *Scalable Reliable Multicast (SRM)*
- *NACK Suppression Mechanisms in Multicast*

---

## 📝 开发说明

### 添加新消息类型

1. 在 `broadcast_protocol.h` 中定义：
```c
typedef struct __attribute__((packed)) {
    MessageHeader header;
    // ... 自定义字段
} NewMessageType;
```

2. 在 `master.c` 和 `receiver.c` 中添加处理逻辑

### 调试模式

启用详细日志：
```c
#define DEBUG_MODE 1

#ifdef DEBUG_MODE
    printf("[DEBUG] ...\n");
#endif
```

---

## 🤝 贡献指南

欢迎提交改进建议和bug报告！

改进方向：
- [ ] 支持断点续传
- [ ] 多文件批量传输
- [ ] Web界面监控
- [ ] 性能分析工具
- [ ] 真实无人机硬件测试

---

## 📄 许可证

本项目基于原 `drone-communication-simulation-system` 项目修改，保持相同许可证。

---

## ✉️ 联系方式

如有问题或建议，请通过以下方式联系：
- GitHub Issues
- Email: [your-email]

---

**祝你使用愉快！🚁📡**

