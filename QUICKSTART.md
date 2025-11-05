# 快速开始指南 - UAV文件广播系统

## 🚀 30秒快速测试

```bash
# 1. 编译
make -f makefile_broadcast all

# 2. 运行自动化测试
./test_broadcast.sh
```

完成！系统会自动启动3个接收方，广播文件，并验证结果。

---

## 📖 手动测试步骤

### 步骤1：编译程序

```bash
make -f makefile_broadcast all
```

### 步骤2：准备测试文件

```bash
# 方法1：使用makefile创建（推荐）
make -f makefile_broadcast test-file

# 方法2：使用任意文件
cp /path/to/your/file.bin test_file.bin
```

### 步骤3：启动接收方

打开**3个终端窗口**，分别运行：

```bash
# 终端1
./receiver 1

# 终端2
./receiver 2

# 终端3
./receiver 3
```

你会看到：
```
========================================
  UAV File Broadcast Receiver
  UAV ID: 1
========================================
[UAV 1] Listening for broadcasts on 239.255.1.1:9000
[UAV 1] Message receiver thread started.
```

### 步骤4：启动主发送方

打开**第4个终端**，运行：

```bash
./master test_file.bin 1
```

### 步骤5：观察传输过程

你会看到类似输出：

**Master端：**
```
[Master] Session initialized:
  File: test_file.bin
  Size: 102400 bytes
  Total chunks: 100
  Total windows: 2

[Master] Sending SESSION_ANNOUNCE...
[Master] Starting data broadcast...
[Master] Broadcast progress: 100/100 chunks (100.0%)
[Master] Initial broadcast completed.

[Master] Entering status query and retransmission phase...
[Master] Sending STATUS_REQ for window 0 (round 0)
[Master] Window 0 completed.
[Master] Window 1 completed.

[Master] Sending END message (file_hash=0x1A2B3C4D)...
```

**Receiver端：**
```
[UAV 1] Session initialized:
  File: test_file.bin
  Total chunks: 100
  Total windows: 2
  Output: received_test_file.bin

[UAV 1] Progress: 100/100 chunks (100.0%)
[UAV 1] Window 0 completed and saved.
[UAV 1] Window 1 completed and saved.

[UAV 1] Received END message, verifying file...
[UAV 1] ✓ File transfer completed successfully!
[UAV 1] ✓ Hash verified: 0x1A2B3C4D
[UAV 1] ✓ File saved as: received_test_file.bin
```

### 步骤6：验证结果

```bash
# 比较文件hash
md5sum test_file.bin received_test_file.bin

# 应该输出相同的hash值，例如：
# a1b2c3d4... test_file.bin
# a1b2c3d4... received_test_file.bin
```

---

## 🎯 核心命令速查

| 命令 | 说明 |
|-----|------|
| `make -f makefile_broadcast all` | 编译所有程序 |
| `make -f makefile_broadcast clean` | 清理编译文件 |
| `make -f makefile_broadcast test-file` | 创建测试文件 |
| `./master <file> [id]` | 启动主发送方 |
| `./receiver <uav_id>` | 启动接收方 |
| `./test_broadcast.sh` | 自动化测试 |

---

## 🧪 测试不同场景

### 1. 大文件传输

```bash
# 创建10MB文件
dd if=/dev/urandom of=large_file.bin bs=1M count=10

# 传输
./master large_file.bin 2
```

### 2. 多接收方测试

```bash
# 启动10个接收方
for i in {1..10}; do
    ./receiver $i > receiver$i.log 2>&1 &
done

# 等待启动
sleep 2

# 开始传输
./master test_file.bin 1

# 停止所有接收方
killall receiver
```

### 3. 丢包测试

```bash
# 模拟10%丢包率（需要root权限）
sudo tc qdisc add dev lo root netem loss 10%

# 运行测试
./test_broadcast.sh

# 恢复网络
sudo tc qdisc del dev lo root
```

---

## ⚙️ 常用配置调整

### 调整传输速率

编辑 `master.c`，找到：
```c
usleep(1000); // 1ms延迟 → 约1MB/s
```

修改为：
```c
usleep(500);  // 0.5ms延迟 → 约2MB/s
usleep(2000); // 2ms延迟 → 约0.5MB/s
```

### 调整窗口大小

编辑 `broadcast_protocol.h`：
```c
#define WINDOW_SIZE  64   // 改为 32 或 128
```

### 调整块大小

编辑 `broadcast_protocol.h`：
```c
#define MAX_CHUNK_SIZE  1024  // 改为 512 或 4096
```

⚠️ **修改后需要重新编译**：
```bash
make -f makefile_broadcast clean
make -f makefile_broadcast all
```

---

## 🐛 常见问题

### 问题：接收方没有输出

**原因**：组播未正确配置

**解决**：
```bash
# 检查网络接口
ip addr show

# 检查路由
ip route show

# 添加组播路由（如果需要）
sudo route add -net 239.255.0.0 netmask 255.255.0.0 dev lo
```

### 问题：编译失败

**原因**：缺少依赖

**解决**：
```bash
# Ubuntu/Debian
sudo apt-get install build-essential

# 确认gcc版本
gcc --version  # 需要 >= 4.8
```

### 问题：Permission denied

**原因**：端口被占用或权限不足

**解决**：
```bash
# 检查端口占用
netstat -tulpn | grep 9000

# 修改端口（如果需要）
# 编辑 broadcast_protocol.h 中的 MULTICAST_PORT
```

---

## 📊 性能优化建议

### 高速传输

```c
// broadcast_protocol.h
#define MAX_CHUNK_SIZE      4096    // 增大块
#define NACK_TIMEOUT_MS     20      // 减小延迟
#define STATUS_REQ_INTERVAL 100     // 加快查询

// master.c
usleep(500); // 减小发送间隔
```

### 低带宽环境

```c
// broadcast_protocol.h
#define MAX_CHUNK_SIZE      512     // 减小块
#define NACK_TIMEOUT_MS     100     // 增大延迟
#define STATUS_REQ_INTERVAL 500     // 放慢查询

// master.c
usleep(5000); // 增大发送间隔
```

### 内存受限

```c
// broadcast_protocol.h
#define WINDOW_SIZE         32      // 减小窗口
#define MAX_CHUNK_SIZE      512     // 减小块
```

---

## 📚 下一步

- 阅读 [README_BROADCAST.md](README_BROADCAST.md) 了解协议详情
- 查看源代码注释理解实现细节
- 尝试修改参数观察性能变化
- 在真实网络环境中测试

---

**祝你使用顺利！如有问题，请查看日志文件。**

