#!/bin/bash

# 丢包测试脚本（适用于WSL2等不支持tc的环境）

# 检查参数
if [ $# -ne 1 ]; then
    echo "=========================================="
    echo "  丢包测试脚本"
    echo "=========================================="
    echo ""
    echo "用法: $0 <丢包率百分比>"
    echo ""
    echo "示例："
    echo "  $0 0    # 无丢包（理想环境）"
    echo "  $0 5    # 5%丢包（良好网络）"
    echo "  $0 10   # 10%丢包（一般网络）"
    echo "  $0 20   # 20%丢包（恶劣环境）"
    echo ""
    exit 1
fi

LOSS_RATE=$1

# 验证参数范围
if [ "$LOSS_RATE" -lt 0 ] || [ "$LOSS_RATE" -gt 50 ]; then
    echo "❌ 错误：丢包率必须在 0-50 之间"
    exit 1
fi

echo "=========================================="
echo "  丢包测试: ${LOSS_RATE}%"
echo "=========================================="
echo ""

# 备份原配置
if [ ! -f broadcast_protocol.h.backup ]; then
    cp broadcast_protocol.h broadcast_protocol.h.backup
    echo "✓ 已备份配置文件"
fi

# 修改配置
echo "⚙️  设置丢包率为 ${LOSS_RATE}%..."
sed -i "s/#define SIMULATE_PACKET_LOSS.*/#define SIMULATE_PACKET_LOSS  ${LOSS_RATE}/" broadcast_protocol.h

# 重新编译
echo "🔨 编译程序..."
make -f makefile_broadcast clean > /dev/null 2>&1
if ! make -f makefile_broadcast all > /dev/null 2>&1; then
    echo "❌ 编译失败！"
    # 恢复配置
    mv broadcast_protocol.h.backup broadcast_protocol.h
    exit 1
fi
echo "✓ 编译完成"
echo ""

# 清理旧的接收文件
rm -f received_uav*_*.bin 2>/dev/null

# 确保测试文件存在
if [ ! -f test_file.bin ]; then
    echo "📝 创建测试文件（300KB）..."
    dd if=/dev/urandom of=test_file.bin bs=1024 count=300 2>/dev/null
fi

echo "=========================================="
echo "  启动测试"
echo "=========================================="
echo ""

# 启动接收方
echo "🚁 启动接收方..."
./receiver 1 > receiver1.log 2>&1 &
PID1=$!
./receiver 2 > receiver2.log 2>&1 &
PID2=$!
./receiver 3 > receiver3.log 2>&1 &
PID3=$!

# 等待接收方就绪
sleep 1

# 记录开始时间
START_TIME=$(date +%s)

# 启动发送方
echo "📡 开始文件传输（丢包率=${LOSS_RATE}%）..."
echo ""
echo "----------------------------------------"
./master test_file.bin 1
echo "----------------------------------------"
echo ""

# 记录结束时间
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

# 等待接收完成
sleep 2

# 清理进程
echo "🧹 清理进程..."
kill $PID1 $PID2 $PID3 2>/dev/null
wait $PID1 $PID2 $PID3 2>/dev/null

echo ""
echo "=========================================="
echo "  测试结果"
echo "=========================================="
echo ""

# 验证文件完整性
SUCCESS_COUNT=0
TOTAL_RECEIVERS=3

for i in 1 2 3; do
    RECV_FILE="received_uav${i}_test_file.bin"
    
    if [ ! -f "$RECV_FILE" ]; then
        echo "❌ 接收方 $i: 文件不存在"
        continue
    fi
    
    if cmp -s test_file.bin "$RECV_FILE"; then
        echo "✅ 接收方 $i: 文件完整（验证通过）"
        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
    else
        echo "❌ 接收方 $i: 文件损坏（验证失败）"
    fi
done

echo ""
echo "📊 统计信息："
echo "   - 丢包率: ${LOSS_RATE}%"
echo "   - 传输时间: ${DURATION} 秒"
echo "   - 成功接收: ${SUCCESS_COUNT}/${TOTAL_RECEIVERS}"
echo "   - 成功率: $((SUCCESS_COUNT * 100 / TOTAL_RECEIVERS))%"

# 分析日志
echo ""
echo "📝 重传统计："
RETRANS_COUNT=$(grep -c "Retransmitting" master.log 2>/dev/null || echo "0")
if [ -f receiver1.log ]; then
    NACK_COUNT=$(grep -c "Sent NACK" receiver*.log 2>/dev/null || echo "0")
    echo "   - 重传次数: ${RETRANS_COUNT}"
    echo "   - NACK发送: ${NACK_COUNT}"
fi

echo ""

# 恢复配置
echo "🔄 恢复配置..."
if [ -f broadcast_protocol.h.backup ]; then
    mv broadcast_protocol.h.backup broadcast_protocol.h
fi

# 重新编译为默认配置
make -f makefile_broadcast clean > /dev/null 2>&1
make -f makefile_broadcast all > /dev/null 2>&1

echo ""
echo "=========================================="
if [ $SUCCESS_COUNT -eq $TOTAL_RECEIVERS ]; then
    echo "  ✅ 测试通过！"
else
    echo "  ⚠️  测试部分失败"
fi
echo "=========================================="
echo ""

# 提示查看日志
echo "💡 提示：查看详细日志请运行："
echo "   cat receiver1.log"
echo "   cat receiver2.log"
echo "   cat receiver3.log"
echo ""

