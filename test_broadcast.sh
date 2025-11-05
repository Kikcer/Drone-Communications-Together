#!/bin/bash

# UAV文件广播系统测试脚本

echo "========================================"
echo "  UAV File Broadcast System Test"
echo "========================================"
echo ""

# 检查可执行文件是否存在
if [ ! -f "./master" ] || [ ! -f "./receiver" ]; then
    echo "错误：请先编译程序"
    echo "运行: make -f makefile_broadcast all"
    exit 1
fi

# 检查测试文件是否存在
if [ ! -f "./test_file.bin" ]; then
    echo "创建测试文件..."
    make -f makefile_broadcast test-file
    echo ""
fi

# 清理之前的接收文件
rm -f received_test_file.bin

echo "启动接收方..."
# 启动3个接收方（后台运行）
./receiver 1 > receiver1.log 2>&1 &
RECV1_PID=$!
echo "  [✓] Receiver 1 (PID: $RECV1_PID) 已启动"

./receiver 2 > receiver2.log 2>&1 &
RECV2_PID=$!
echo "  [✓] Receiver 2 (PID: $RECV2_PID) 已启动"

./receiver 3 > receiver3.log 2>&1 &
RECV3_PID=$!
echo "  [✓] Receiver 3 (PID: $RECV3_PID) 已启动"

# 等待接收方启动
echo ""
echo "等待接收方初始化..."
sleep 2

echo ""
echo "启动主发送方..."
echo "========================================"
./master test_file.bin 1
echo "========================================"

# 等待传输完成
echo ""
echo "等待传输完成..."
sleep 3

# 停止接收方
echo ""
echo "停止接收方..."
kill $RECV1_PID $RECV2_PID $RECV3_PID 2>/dev/null
echo "  [✓] 所有接收方已停止"

# 验证结果
echo ""
echo "========================================"
echo "  验证结果"
echo "========================================"

# 检查文件是否存在
if [ -f "received_test_file.bin" ]; then
    # 计算原始文件和接收文件的hash
    ORIGINAL_MD5=$(md5sum test_file.bin | awk '{print $1}')
    RECEIVED_MD5=$(md5sum received_test_file.bin | awk '{print $1}')
    
    echo "原始文件MD5:  $ORIGINAL_MD5"
    echo "接收文件MD5:  $RECEIVED_MD5"
    echo ""
    
    if [ "$ORIGINAL_MD5" == "$RECEIVED_MD5" ]; then
        echo "✓ 文件传输成功！文件完全一致"
        
        # 显示文件大小
        ORIGINAL_SIZE=$(stat -f%z test_file.bin 2>/dev/null || stat -c%s test_file.bin)
        RECEIVED_SIZE=$(stat -f%z received_test_file.bin 2>/dev/null || stat -c%s received_test_file.bin)
        echo "  原始文件大小: $ORIGINAL_SIZE 字节"
        echo "  接收文件大小: $RECEIVED_SIZE 字节"
    else
        echo "✗ 文件传输失败：MD5不匹配"
    fi
else
    echo "✗ 文件传输失败：未找到接收文件"
fi

echo ""
echo "日志文件："
echo "  receiver1.log - UAV 1 的日志"
echo "  receiver2.log - UAV 2 的日志"
echo "  receiver3.log - UAV 3 的日志"
echo ""

# 显示接收方的关键日志
echo "========================================"
echo "  接收方1日志摘要"
echo "========================================"
grep -E "Session initialized|Progress|Window.*completed|File transfer completed" receiver1.log | head -20

echo ""
echo "测试完成！"

