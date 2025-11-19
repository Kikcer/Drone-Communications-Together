#include "broadcast_protocol.h"

static MasterSession g_session;
static int g_multicast_sock;
static pthread_mutex_t g_session_mutex = PTHREAD_MUTEX_INITIALIZER;

// ========== 初始化Master会话 ==========
bool init_master_session(const char *filename, uint16_t file_id)
{
    memset(&g_session, 0, sizeof(g_session));

    // 打开输入文件
    g_session.input_file = fopen(filename, "rb");
    if (!g_session.input_file)
    {
        perror("Failed to open input file");
        return false;
    }

    // 获取文件大小
    fseek(g_session.input_file, 0, SEEK_END);
    long file_size = ftell(g_session.input_file);
    fseek(g_session.input_file, 0, SEEK_SET);

    // 初始化会话参数
    g_session.file_id = file_id;
    g_session.chunk_size = MAX_CHUNK_SIZE;
    g_session.window_size = WINDOW_SIZE;
    g_session.total_chunks = (file_size + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE;
    g_session.total_windows = (g_session.total_chunks + WINDOW_SIZE - 1) / WINDOW_SIZE;
    strncpy(g_session.filename, filename, sizeof(g_session.filename) - 1);

    // 分配窗口状态数组
    g_session.windows = calloc(g_session.total_windows, sizeof(MasterWindowState));
    if (!g_session.windows)
    {
        perror("Failed to allocate window states");
        fclose(g_session.input_file);
        return false;
    }

    // 初始化窗口状态
    for (uint32_t i = 0; i < g_session.total_windows; i++)
    {
        g_session.windows[i].window_id = i;
        g_session.windows[i].need_retransmit = 0;
        g_session.windows[i].round_count = 0;
        g_session.windows[i].completed = false;
    }

    printf("[Master] Session initialized:\n");
    printf("  File: %s\n", filename);
    printf("  Size: %ld bytes\n", file_size);
    printf("  Total chunks: %u\n", g_session.total_chunks);
    printf("  Total windows: %u\n", g_session.total_windows);
    printf("  Window size: %u chunks\n", g_session.window_size);

    return true;
}

// ========== 阶段1: 发送会话启动消息 ==========
void send_session_announce()
{
    SessionAnnounce msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.msg_type = MSG_SESSION_ANNOUNCE;
    msg.header.payload_len = sizeof(SessionAnnounce) - sizeof(MessageHeader);
    msg.file_id = g_session.file_id;
    msg.total_chunks = g_session.total_chunks;
    msg.window_size = g_session.window_size;
    msg.chunk_size = g_session.chunk_size;
    strncpy(msg.filename, g_session.filename, sizeof(msg.filename) - 1);

    printf("[Master] Sending SESSION_ANNOUNCE...\n");
    for (int i = 0; i < ANNOUNCE_REPEAT_COUNT; i++)
    {
        send_multicast(g_multicast_sock, &msg, sizeof(msg));
        usleep(10000);
    }
}

// ========== 阶段2: 广播单个窗口的数据块 ==========
void broadcast_window_chunks(uint32_t window_id)
{
    DataChunk chunk_msg;
    memset(&chunk_msg, 0, sizeof(chunk_msg));
    chunk_msg.header.msg_type = MSG_DATA_CHUNK;
    chunk_msg.file_id = g_session.file_id;
    chunk_msg.header.payload_len = sizeof(DataChunk) - sizeof(MessageHeader);

    uint32_t start_chunk = window_id * WINDOW_SIZE;
    uint32_t end_chunk = start_chunk + WINDOW_SIZE;
    if (end_chunk > g_session.total_chunks)
    {
        end_chunk = g_session.total_chunks;
    }

    printf("[Master] Broadcasting window %u (chunks %u-%u)...\n",
           window_id, start_chunk, end_chunk - 1);

    for (uint32_t chunk_id = start_chunk; chunk_id < end_chunk; chunk_id++)
    {
        // 定位并读取数据
        fseek(g_session.input_file, chunk_id * MAX_CHUNK_SIZE, SEEK_SET);
        size_t bytes_read = fread(chunk_msg.data, 1, MAX_CHUNK_SIZE, g_session.input_file);

        chunk_msg.chunk_id = chunk_id;
        chunk_msg.data_len = bytes_read;
        chunk_msg.crc = crc16(chunk_msg.data, bytes_read);

        // 发送数据块
        send_multicast(g_multicast_sock, &chunk_msg, sizeof(chunk_msg));

        // 控制发送速率（避免过快导致丢包）
        usleep(1000); // 1ms延迟，约1MB/s传输速率
    }

    printf("[Master] Window %u broadcast completed.\n", window_id);
}

// ========== 阶段3: 发送状态查询 ==========
void send_status_request(uint32_t window_id, uint16_t round_id)
{
    StatusRequest msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.msg_type = MSG_STATUS_REQ;
    msg.header.payload_len = sizeof(StatusRequest) - sizeof(MessageHeader);
    msg.file_id = g_session.file_id;
    msg.window_id = window_id;
    msg.round_id = round_id;

    printf("[Master] Sending STATUS_REQ for window %u (round %u)\n", window_id, round_id);
    send_multicast(g_multicast_sock, &msg, sizeof(msg));
}

// ========== NACK接收处理线程 ==========
void *nack_receiver_thread(void *arg)
{
    uint8_t buffer[2048];
    struct sockaddr_in src_addr;

    printf("[Master] NACK receiver thread started.\n");

    while (1)
    {
        int recv_len = recv_multicast(g_multicast_sock, buffer, sizeof(buffer), &src_addr);
        if (recv_len < sizeof(MessageHeader))
        {
            continue;
        }

        MessageHeader *header = (MessageHeader *)buffer;

        if (header->msg_type == MSG_NACK)
        {
            NackMessage *nack = (NackMessage *)buffer;

            if (nack->file_id != g_session.file_id)
            {
                continue;
            }

            pthread_mutex_lock(&g_session_mutex);

            uint32_t window_id = nack->window_id;
            if (window_id < g_session.total_windows)
            {
                // 合并NACK的缺失块到窗口状态
                // nack->missing_bitmap 已经是缺失块的bitmap，直接使用
                g_session.windows[window_id].need_retransmit |= nack->missing_bitmap;
                // 记录已知UAV与本窗口的响应
                if (nack->uav_id < MAX_UAVS)
                {
                    g_session.known_uavs_bitmap |= (1u << nack->uav_id);
                    g_session.windows[window_id].responded_uav_bitmap |= (1u << nack->uav_id);
                }

                printf("[Master] Received NACK from UAV %u for window %u (round %u), missing bits: %d\n",
                       nack->uav_id, window_id, nack->round_id, count_set_bits(nack->missing_bitmap));
            }

            pthread_mutex_unlock(&g_session_mutex);
        }
    }

    return NULL;
}

// ========== 阶段4: 重传指定窗口的缺失块 ==========
void retransmit_window_chunks(uint32_t window_id)
{
    pthread_mutex_lock(&g_session_mutex);

    uint64_t need_retransmit = g_session.windows[window_id].need_retransmit;

    pthread_mutex_unlock(&g_session_mutex);

    if (need_retransmit == 0)
    {
        return; // 没有需要重传的块
    }

    int retrans_count = 0;
    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        if (need_retransmit & (1ULL << i))
        {
            retrans_count++;
        }
    }

    printf("[Master] Retransmitting %d chunks for window %u\n", retrans_count, window_id);

    DataChunk chunk_msg;
    memset(&chunk_msg, 0, sizeof(chunk_msg));
    chunk_msg.header.msg_type = MSG_DATA_CHUNK;
    chunk_msg.file_id = g_session.file_id;
    chunk_msg.header.payload_len = sizeof(DataChunk) - sizeof(MessageHeader);

    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        if (need_retransmit & (1ULL << i))
        {
            uint32_t chunk_id = window_id * WINDOW_SIZE + i;

            if (chunk_id >= g_session.total_chunks)
            {
                break; // 超出文件范围
            }

            // 定位并读取数据
            fseek(g_session.input_file, chunk_id * MAX_CHUNK_SIZE, SEEK_SET);
            size_t bytes_read = fread(chunk_msg.data, 1, MAX_CHUNK_SIZE, g_session.input_file);

            chunk_msg.chunk_id = chunk_id;
            chunk_msg.data_len = bytes_read;
            chunk_msg.crc = crc16(chunk_msg.data, bytes_read);

            // 发送重传块
            send_multicast(g_multicast_sock, &chunk_msg, sizeof(chunk_msg));
            usleep(1000);
        }
    }

    // 注意：不在这里清零 need_retransmit
    // 应该在下一轮查询前清零，以便接收新的NACK
}

// ========== 阶段2-4: 逐窗口广播和重传 ==========
void window_by_window_transmission()
{
    printf("[Master] Starting window-by-window transmission...\n");

    for (uint32_t window_id = 0; window_id < g_session.total_windows; window_id++)
    {
        // 步骤1: 广播该窗口的所有数据块
        broadcast_window_chunks(window_id);

        // 步骤2-3: 查询并重传，直到窗口完成
        // 每个窗口至少查询3轮，确保有足够机会收到NACK
        bool window_completed = false;
        uint16_t no_nack_rounds = 0; // 连续没有NACK的轮数

        for (uint16_t round = 0; round < MAX_RETRANS_ROUNDS; round++)
        {
            // 清零上一轮的重传标记与响应位图，准备接收新的应答
            pthread_mutex_lock(&g_session_mutex);
            g_session.windows[window_id].need_retransmit = 0;
            g_session.windows[window_id].responded_uav_bitmap = 0;
            pthread_mutex_unlock(&g_session_mutex);

            // 在未收到全部已知UAV的响应时，重发STATUS_REQ，最多MAX_RESEND_BITMAP_ASK次
            for (int attempt = 0; attempt < MAX_RESEND_BITMAP_ASK; attempt++)
            {
                // 发送状态查询
                printf("[Master] Sending STATUS_REQ for window %u (round %u, attempt %d)\n", window_id, round, attempt + 1);
                send_status_request(window_id, round);
                // 等待响应
                usleep(STATUS_REQ_INTERVAL * 1000);

                // 检查是否所有已知UAV都已响应
                pthread_mutex_lock(&g_session_mutex);
                uint32_t known_mask = g_session.known_uavs_bitmap;
                uint32_t responded_mask = g_session.windows[window_id].responded_uav_bitmap;
                pthread_mutex_unlock(&g_session_mutex);
                if (known_mask == 0 || (responded_mask & known_mask) == known_mask)
                {
                    break; // 所有已知UAV均已响应，结束重发
                }
            }

            // 检查是否收到NACK（是否需要重传）
            pthread_mutex_lock(&g_session_mutex);
            uint64_t need_retransmit = g_session.windows[window_id].need_retransmit;
            uint32_t known_mask = g_session.known_uavs_bitmap;
            uint32_t responded_mask = g_session.windows[window_id].responded_uav_bitmap;
            pthread_mutex_unlock(&g_session_mutex);

            // 如果收到NACK，执行重传
            if (need_retransmit != 0)
            {
                retransmit_window_chunks(window_id);
                no_nack_rounds = 0; // 重置计数
            }
            else
            {
                // 仅当所有已知UAV均已响应且没有需要重传的块时，才记为一轮“无NACK”
                if (known_mask == 0 || (responded_mask & known_mask) == known_mask)
                {
                    no_nack_rounds++;
                    // 连续3轮没有收到NACK，才认为窗口完成
                    if (no_nack_rounds >= 3)
                    {
                        pthread_mutex_lock(&g_session_mutex);
                        g_session.windows[window_id].completed = true;
                        pthread_mutex_unlock(&g_session_mutex);
                        printf("[Master] Window %u completed after %u rounds (no NACK for 3 consecutive rounds).\n",
                               window_id, round);
                        window_completed = true;
                        break;
                    }
                }
                else
                {
                    // 未收到所有已知UAV响应，不计入“无NACK”轮
                    no_nack_rounds = 0;
                }
            }
        }

        if (!window_completed)
        {
            printf("[Master] WARNING: Window %u reached max retransmission rounds.\n", window_id);
        }
    }

    printf("[Master] All windows transmitted and verified.\n");
}

// ========== 阶段5: 发送结束消息 ==========
void send_end_message()
{
    // 计算文件hash
    fseek(g_session.input_file, 0, SEEK_SET);
    uint8_t *file_buffer = malloc(g_session.total_chunks * MAX_CHUNK_SIZE);
    if (!file_buffer)
    {
        perror("Failed to allocate buffer for hash");
        return;
    }

    size_t total_read = fread(file_buffer, 1, g_session.total_chunks * MAX_CHUNK_SIZE,
                              g_session.input_file);
    uint32_t file_hash = simple_hash(file_buffer, total_read);
    free(file_buffer);

    EndMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.msg_type = MSG_END;
    msg.header.payload_len = sizeof(EndMessage) - sizeof(MessageHeader);
    msg.file_id = g_session.file_id;
    msg.total_chunks = g_session.total_chunks;
    msg.file_hash = file_hash;

    printf("[Master] Sending END message (file_hash=0x%08X)...\n", file_hash);

    // 多次发送END消息
    for (int i = 0; i < 5; i++)
    {
        send_multicast(g_multicast_sock, &msg, sizeof(msg));
        usleep(50000);
    }
}

// ========== 清理资源 ==========
void cleanup_master_session()
{
    if (g_session.input_file)
    {
        fclose(g_session.input_file);
    }
    if (g_session.windows)
    {
        free(g_session.windows);
    }
    if (g_multicast_sock >= 0)
    {
        close(g_multicast_sock);
    }
}

// ========== 主函数 ==========
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s <filename> [file_id]\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    uint16_t file_id = (argc >= 3) ? atoi(argv[2]) : 1;

    // 禁用输出缓冲，确保日志立即写入
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    // 初始化随机数种子（用于丢包模拟）
    srand(time(NULL));

    printf("========================================\n");
    printf("  UAV File Broadcast Master\n");
    printf("========================================\n");
#if SIMULATE_PACKET_LOSS > 0
    printf("  ⚠️  Packet Loss Simulation: %d%%\n", SIMULATE_PACKET_LOSS);
    printf("========================================\n");
#endif
    fflush(stdout);

    // 创建组播socket（Master 需要接收NACK，所以使用false模式）
    g_multicast_sock = create_multicast_socket(false);
    if (g_multicast_sock < 0)
    {
        fprintf(stderr, "Failed to create multicast socket\n");
        return 1;
    }

    // 初始化会话
    if (!init_master_session(filename, file_id))
    {
        cleanup_master_session();
        return 1;
    }

    // 启动NACK接收线程
    pthread_t nack_thread;
    pthread_create(&nack_thread, NULL, nack_receiver_thread, NULL);
    pthread_detach(nack_thread);

    sleep(1);

    // 阶段1: 会话启动
    send_session_announce();
    sleep(1);

    // 阶段2-4: 逐窗口广播和重传
    window_by_window_transmission();
    sleep(1);

    // 阶段5: 结束
    send_end_message();

    printf("[Master] All phases completed. Press Ctrl+C to exit.\n");

    // 保持运行以继续处理延迟的NACK
    sleep(5);

    cleanup_master_session();
    return 0;
}
