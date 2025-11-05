#ifndef BROADCAST_PROTOCOL_H
#define BROADCAST_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>

// ========== 协议参数配置 ==========
#define MULTICAST_GROUP "239.255.1.1"
#define MULTICAST_PORT 9000
#define MAX_CHUNK_SIZE 1024     // 每个数据块1KB
#define WINDOW_SIZE 64          // 每个窗口64个块
#define MAX_UAVS 32             // 最大无人机数量
#define NACK_TIMEOUT_MS 50      // NACK随机退避最大延迟
#define STATUS_REQ_INTERVAL 500 // 状态查询间隔（毫秒）增加以确保NACK有足够时间
#define MAX_RETRANS_ROUNDS 10   // 最大重传轮数

// ========== 丢包模拟配置（用于WSL2等不支持tc的环境）==========
// 设置为0禁用丢包模拟，设置为1-100表示丢包百分比
#define SIMULATE_PACKET_LOSS 0 // 丢包率百分比（0=禁用，10=10%丢包）

// ========== 消息类型 ==========
typedef enum
{
    MSG_SESSION_ANNOUNCE = 1,
    MSG_DATA_CHUNK = 2,
    MSG_STATUS_REQ = 3,
    MSG_NACK = 4,
    MSG_END = 5
} MessageType;

// ========== 消息结构定义 ==========

// 通用消息头（所有消息前4字节）
typedef struct __attribute__((packed))
{
    uint8_t msg_type;     // 消息类型
    uint8_t reserved;     // 保留字段
    uint16_t payload_len; // 载荷长度
} MessageHeader;

// 阶段1: 会话启动消息
typedef struct __attribute__((packed))
{
    MessageHeader header;
    uint16_t file_id;      // 文件ID
    uint32_t total_chunks; // 总块数
    uint16_t window_size;  // 窗口大小（块数）
    uint32_t chunk_size;   // 每块大小（字节）
    char filename[64];     // 文件名
} SessionAnnounce;

// 阶段2: 数据块消息
typedef struct __attribute__((packed))
{
    MessageHeader header;
    uint16_t file_id;             // 文件ID
    uint32_t chunk_id;            // 块编号
    uint16_t data_len;            // 实际数据长度
    uint16_t crc;                 // CRC校验
    uint8_t data[MAX_CHUNK_SIZE]; // 数据内容
} DataChunk;

// 阶段3: 状态查询消息
typedef struct __attribute__((packed))
{
    MessageHeader header;
    uint16_t file_id;   // 文件ID
    uint32_t window_id; // 窗口ID
    uint16_t round_id;  // 查询轮次
} StatusRequest;

// 阶段3: NACK消息（缺块反馈）
typedef struct __attribute__((packed))
{
    MessageHeader header;
    uint16_t file_id;        // 文件ID
    uint32_t window_id;      // 窗口ID
    uint16_t round_id;       // 轮次
    uint8_t uav_id;          // 发送方ID
    uint64_t missing_bitmap; // 缺块bitmap（64位）
} NackMessage;

// 阶段5: 结束消息
typedef struct __attribute__((packed))
{
    MessageHeader header;
    uint16_t file_id;      // 文件ID
    uint32_t total_chunks; // 总块数
    uint32_t file_hash;    // 文件hash校验
} EndMessage;

// ========== 本地状态结构 ==========

// 窗口接收状态
typedef struct
{
    uint32_t window_id;
    uint64_t received_bitmap; // 64位bitmap，1表示已收到
    bool completed;           // 窗口是否完成
    uint8_t *data_buffer;     // 数据缓冲区
} WindowState;

// 接收方会话状态
typedef struct
{
    uint16_t file_id;
    uint32_t total_chunks;
    uint16_t window_size;
    uint32_t chunk_size;
    char filename[64];
    uint32_t total_windows;
    WindowState *windows; // 窗口状态数组
    FILE *output_file;
    bool session_active;
    uint32_t received_chunks;
} ReceiverSession;

// 发送方窗口状态
typedef struct
{
    uint32_t window_id;
    uint64_t need_retransmit; // 需要重传的块bitmap
    uint8_t round_count;      // 已查询轮数
    bool completed;           // 窗口是否完成
} MasterWindowState;

// 发送方会话状态
typedef struct
{
    uint16_t file_id;
    uint32_t total_chunks;
    uint16_t window_size;
    uint32_t chunk_size;
    char filename[64];
    uint32_t total_windows;
    FILE *input_file;
    MasterWindowState *windows;
    bool broadcast_completed; // 是否完成初始广播
} MasterSession;

// ========== 工具函数声明 ==========

// CRC16校验
uint16_t crc16(const uint8_t *data, size_t len);

// 简单hash计算
uint32_t simple_hash(const uint8_t *data, size_t len);

// 创建组播socket
int create_multicast_socket(bool sender);

// 发送组播消息
int send_multicast(int sock, const void *data, size_t len);

// 接收组播消息
int recv_multicast(int sock, void *buffer, size_t len, struct sockaddr_in *src_addr);

// 获取当前时间（毫秒）
uint64_t get_time_ms();

// 打印bitmap（调试用）
void print_bitmap(uint64_t bitmap);

// 检查bitmap中缺失的块数量
int count_missing_bits(uint64_t bitmap);

// 判断bitmap1是否包含bitmap2的所有缺失块
bool bitmap_covers(uint64_t bitmap1, uint64_t bitmap2);

#endif // BROADCAST_PROTOCOL_H
