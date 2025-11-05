#include "broadcast_protocol.h"

static ReceiverSession g_session;
static int g_multicast_sock;
static uint8_t g_uav_id = 0;
static pthread_mutex_t g_session_mutex = PTHREAD_MUTEX_INITIALIZER;

// NACK抑制相关
typedef struct
{
    bool active;
    uint32_t window_id;
    uint16_t round_id;
    uint64_t my_missing_bitmap;
    uint64_t pending_timeout_ms;
    pthread_t timer_thread;
    bool suppressed;
} NackContext;

static NackContext g_nack_ctx;
static pthread_mutex_t g_nack_mutex = PTHREAD_MUTEX_INITIALIZER;

// ========== 初始化接收方会话 ==========
bool init_receiver_session(const SessionAnnounce *announce)
{
    pthread_mutex_lock(&g_session_mutex);

    // 检查是否已有会话
    if (g_session.session_active && g_session.file_id == announce->file_id)
    {
        pthread_mutex_unlock(&g_session_mutex);
        return true; // 会话已存在
    }

    memset(&g_session, 0, sizeof(g_session));

    g_session.file_id = announce->file_id;
    g_session.total_chunks = announce->total_chunks;
    g_session.window_size = announce->window_size;
    g_session.chunk_size = announce->chunk_size;
    strncpy(g_session.filename, announce->filename, sizeof(g_session.filename) - 1);
    g_session.total_windows = (g_session.total_chunks + g_session.window_size - 1) / g_session.window_size;

    // 分配窗口状态数组
    g_session.windows = calloc(g_session.total_windows, sizeof(WindowState));
    if (!g_session.windows)
    {
        perror("Failed to allocate window states");
        pthread_mutex_unlock(&g_session_mutex);
        return false;
    }

    // 初始化窗口状态（不再需要缓冲区，改为立即写入文件）
    for (uint32_t i = 0; i < g_session.total_windows; i++)
    {
        g_session.windows[i].window_id = i;
        g_session.windows[i].received_bitmap = 0;
        g_session.windows[i].completed = false;
        g_session.windows[i].data_buffer = NULL; // 不再需要缓冲区
    }

    // 打开输出文件（每个UAV使用独立的文件名）
    char output_filename[128];
    snprintf(output_filename, sizeof(output_filename), "received_uav%u_%s", g_uav_id, g_session.filename);
    g_session.output_file = fopen(output_filename, "wb");
    if (!g_session.output_file)
    {
        perror("Failed to open output file");
        pthread_mutex_unlock(&g_session_mutex);
        return false;
    }

    g_session.session_active = true;
    g_session.received_chunks = 0;

    printf("[UAV %u] Session initialized:\n", g_uav_id);
    printf("  File: %s\n", g_session.filename);
    printf("  Total chunks: %u\n", g_session.total_chunks);
    printf("  Total windows: %u\n", g_session.total_windows);
    printf("  Output: %s\n", output_filename);

    pthread_mutex_unlock(&g_session_mutex);
    return true;
}

// ========== 处理接收到的数据块 ==========
void process_data_chunk(const DataChunk *chunk)
{
    if (chunk->file_id != g_session.file_id || !g_session.session_active)
    {
        return;
    }

    if (chunk->chunk_id >= g_session.total_chunks)
    {
        return; // 超出范围
    }

    // 验证CRC
    uint16_t calc_crc = crc16(chunk->data, chunk->data_len);
    if (calc_crc != chunk->crc)
    {
        printf("[UAV %u] CRC error for chunk %u, discarding.\n", g_uav_id, chunk->chunk_id);
        return;
    }

    pthread_mutex_lock(&g_session_mutex);

    uint32_t window_id = chunk->chunk_id / g_session.window_size;
    uint32_t chunk_offset = chunk->chunk_id % g_session.window_size;

    WindowState *window = &g_session.windows[window_id];

    // 检查是否已经收到过
    if (window->received_bitmap & (1ULL << chunk_offset))
    {
        pthread_mutex_unlock(&g_session_mutex);
        return; // 已收到，跳过
    }

    // 标记为已收到
    window->received_bitmap |= (1ULL << chunk_offset);
    g_session.received_chunks++;

    // 立即写入文件（不等待窗口完成）
    fseek(g_session.output_file, chunk->chunk_id * MAX_CHUNK_SIZE, SEEK_SET);
    fwrite(chunk->data, 1, chunk->data_len, g_session.output_file);
    fflush(g_session.output_file);

    // 检查窗口是否完成
    uint32_t chunks_in_window = (window_id == g_session.total_windows - 1) ? (g_session.total_chunks - window_id * g_session.window_size) : g_session.window_size;

    // 特殊处理：当chunks_in_window=64时，(1ULL << 64)是未定义行为
    uint64_t expected_bitmap;
    if (chunks_in_window == 64)
    {
        expected_bitmap = 0xFFFFFFFFFFFFFFFFULL; // 64个1
    }
    else
    {
        expected_bitmap = (1ULL << chunks_in_window) - 1;
    }

    if (window->received_bitmap == expected_bitmap)
    {
        window->completed = true;

        // 释放窗口缓冲区（数据已写入文件）
        if (window->data_buffer)
        {
            free(window->data_buffer);
            window->data_buffer = NULL;
        }

        printf("[UAV %u] Window %u completed and saved.\n", g_uav_id, window_id);
    }

    // 显示进度
    if (g_session.received_chunks % 100 == 0)
    {
        printf("[UAV %u] Progress: %u/%u chunks (%.1f%%)\n",
               g_uav_id, g_session.received_chunks, g_session.total_chunks,
               100.0 * g_session.received_chunks / g_session.total_chunks);
    }

    pthread_mutex_unlock(&g_session_mutex);
}

// ========== NACK定时器线程 ==========
void *nack_timer_thread(void *arg)
{
    NackContext *ctx = (NackContext *)arg;

    usleep(ctx->pending_timeout_ms * 1000);

    pthread_mutex_lock(&g_nack_mutex);

    if (!ctx->suppressed && ctx->active)
    {
        // 发送NACK
        NackMessage nack;
        memset(&nack, 0, sizeof(nack));
        nack.header.msg_type = MSG_NACK;
        nack.header.payload_len = sizeof(NackMessage) - sizeof(MessageHeader);
        nack.file_id = g_session.file_id;
        nack.window_id = ctx->window_id;
        nack.round_id = ctx->round_id;
        nack.uav_id = g_uav_id;
        nack.missing_bitmap = ctx->my_missing_bitmap;

        send_multicast(g_multicast_sock, &nack, sizeof(nack));

        int missing_count = count_missing_bits(ctx->my_missing_bitmap);
        printf("[UAV %u] Sent NACK for window %u (missing %d chunks)\n",
               g_uav_id, ctx->window_id, missing_count);
    }
    else if (ctx->suppressed)
    {
        printf("[UAV %u] NACK suppressed for window %u (covered by others)\n",
               g_uav_id, ctx->window_id);
    }

    ctx->active = false;
    pthread_mutex_unlock(&g_nack_mutex);

    return NULL;
}

// ========== 处理状态查询（STATUS_REQ） ==========
void process_status_request(const StatusRequest *req)
{
    if (req->file_id != g_session.file_id || !g_session.session_active)
    {
        return;
    }

    uint32_t window_id = req->window_id;
    if (window_id >= g_session.total_windows)
    {
        return;
    }

    pthread_mutex_lock(&g_session_mutex);
    WindowState *window = &g_session.windows[window_id];

    // 如果窗口已完成，不需要响应
    if (window->completed)
    {
        pthread_mutex_unlock(&g_session_mutex);
        return;
    }

    uint64_t received_bitmap = window->received_bitmap;
    pthread_mutex_unlock(&g_session_mutex);

    // 计算缺失的块数量
    uint32_t chunks_in_window = (window_id == g_session.total_windows - 1) ? (g_session.total_chunks - window_id * g_session.window_size) : g_session.window_size;

    // 特殊处理：当chunks_in_window=64时，(1ULL << 64)是未定义行为
    uint64_t expected_bitmap;
    if (chunks_in_window == 64)
    {
        expected_bitmap = 0xFFFFFFFFFFFFFFFFULL; // 64个1
    }
    else
    {
        expected_bitmap = (1ULL << chunks_in_window) - 1;
    }
    uint64_t missing_bitmap = expected_bitmap & (~received_bitmap);

    // 统计收到的块数
    int received_count = 0;
    for (int i = 0; i < chunks_in_window; i++)
    {
        if (received_bitmap & (1ULL << i))
        {
            received_count++;
        }
    }

    printf("[UAV %u] Window %u status: received %d/%u chunks, received_bitmap=0x%llx, expected_bitmap=0x%llx, missing_bitmap=0x%llx\n",
           g_uav_id, window_id, received_count, chunks_in_window,
           (unsigned long long)received_bitmap,
           (unsigned long long)expected_bitmap,
           (unsigned long long)missing_bitmap);

    if (missing_bitmap == 0)
    {
        printf("[UAV %u] Window %u: No missing chunks, not sending NACK\n", g_uav_id, window_id);
        return; // 没有缺失，不响应
    }

    // 计算缺失块数量
    int missing_count = 0;
    for (int i = 0; i < 64; i++)
    {
        if (missing_bitmap & (1ULL << i))
        {
            missing_count++;
        }
    }
    printf("[UAV %u] Window %u: Preparing to send NACK for %d missing chunks\n",
           g_uav_id, window_id, missing_count);

    // 启动NACK抑制机制
    pthread_mutex_lock(&g_nack_mutex);

    // 取消之前的NACK定时器（如果存在）
    if (g_nack_ctx.active)
    {
        g_nack_ctx.suppressed = true;
    }

    // 创建新的NACK上下文
    g_nack_ctx.active = true;
    g_nack_ctx.window_id = window_id;
    g_nack_ctx.round_id = req->round_id;
    g_nack_ctx.my_missing_bitmap = missing_bitmap; // 存储缺失块的bitmap
    g_nack_ctx.suppressed = false;

    // 随机退避延迟（0 ~ NACK_TIMEOUT_MS）
    g_nack_ctx.pending_timeout_ms = rand() % NACK_TIMEOUT_MS;

    // 启动定时器线程
    pthread_create(&g_nack_ctx.timer_thread, NULL, nack_timer_thread, &g_nack_ctx);
    pthread_detach(g_nack_ctx.timer_thread);

    pthread_mutex_unlock(&g_nack_mutex);
}

// ========== 处理其他节点的NACK（用于抑制） ==========
void process_other_nack(const NackMessage *nack)
{
    if (nack->file_id != g_session.file_id || !g_session.session_active)
    {
        return;
    }

    if (nack->uav_id == g_uav_id)
    {
        return; // 忽略自己的NACK
    }

    pthread_mutex_lock(&g_nack_mutex);

    // 如果我有pending的NACK，且与收到的NACK相关
    if (g_nack_ctx.active &&
        g_nack_ctx.window_id == nack->window_id &&
        g_nack_ctx.round_id == nack->round_id)
    {

        // 检查对方的NACK是否覆盖了我的需求
        if (bitmap_covers(nack->missing_bitmap, g_nack_ctx.my_missing_bitmap))
        {
            // 对方的NACK已经涵盖了我的缺失块，抑制我的NACK
            g_nack_ctx.suppressed = true;
        }
    }

    pthread_mutex_unlock(&g_nack_mutex);
}

// ========== 处理结束消息 ==========
void process_end_message(const EndMessage *end_msg)
{
    if (end_msg->file_id != g_session.file_id || !g_session.session_active)
    {
        return;
    }

    printf("[UAV %u] Received END message, verifying file...\n", g_uav_id);

    pthread_mutex_lock(&g_session_mutex);

    // 检查是否收齐所有块
    bool all_received = (g_session.received_chunks == g_session.total_chunks);

    if (!all_received)
    {
        printf("[UAV %u] WARNING: File incomplete! Received %u/%u chunks\n",
               g_uav_id, g_session.received_chunks, g_session.total_chunks);
        pthread_mutex_unlock(&g_session_mutex);
        return;
    }

    // 验证文件hash（关闭并重新打开确保所有数据已写入）
    fflush(g_session.output_file);
    fclose(g_session.output_file);

    char output_filename[128];
    snprintf(output_filename, sizeof(output_filename), "received_%s", g_session.filename);
    FILE *verify_file = fopen(output_filename, "rb");
    if (!verify_file)
    {
        pthread_mutex_unlock(&g_session_mutex);
        return;
    }

    uint8_t *file_buffer = malloc(g_session.total_chunks * MAX_CHUNK_SIZE);
    if (file_buffer)
    {
        size_t total_read = fread(file_buffer, 1, g_session.total_chunks * MAX_CHUNK_SIZE,
                                  verify_file);
        uint32_t calc_hash = simple_hash(file_buffer, total_read);
        free(file_buffer);

        if (calc_hash == end_msg->file_hash)
        {
            printf("[UAV %u] ✓ File transfer completed successfully!\n", g_uav_id);
            printf("[UAV %u] ✓ Hash verified: 0x%08X\n", g_uav_id, calc_hash);
            printf("[UAV %u] ✓ File saved as: received_%s\n", g_uav_id, g_session.filename);
            g_session.session_active = false; // 标记会话完成
        }
        else
        {
            printf("[UAV %u] ✗ Hash mismatch! Expected 0x%08X, got 0x%08X\n",
                   g_uav_id, end_msg->file_hash, calc_hash);
        }
    }

    fclose(verify_file);
    pthread_mutex_unlock(&g_session_mutex);
}

// ========== 消息接收主循环 ==========
void *message_receiver_thread(void *arg)
{
    uint8_t buffer[2048];
    struct sockaddr_in src_addr;

    printf("[UAV %u] Message receiver thread started.\n", g_uav_id);

    while (1)
    {
        int recv_len = recv_multicast(g_multicast_sock, buffer, sizeof(buffer), &src_addr);
        if (recv_len < sizeof(MessageHeader))
        {
            continue;
        }

        MessageHeader *header = (MessageHeader *)buffer;

        switch (header->msg_type)
        {
        case MSG_SESSION_ANNOUNCE:
            if (recv_len >= sizeof(SessionAnnounce))
            {
                SessionAnnounce *announce = (SessionAnnounce *)buffer;
                init_receiver_session(announce);
            }
            break;

        case MSG_DATA_CHUNK:
            if (recv_len >= sizeof(DataChunk))
            {
                // ========== 应用层丢包模拟（每个接收方独立）==========
#if SIMULATE_PACKET_LOSS > 0
                // 每个接收方独立地随机丢弃数据包
                int random_value = rand() % 100;
                if (random_value < SIMULATE_PACKET_LOSS)
                {
                    // 模拟丢包：忽略此数据块
                    break;
                }
#endif
                DataChunk *chunk = (DataChunk *)buffer;
                process_data_chunk(chunk);
            }
            break;

        case MSG_STATUS_REQ:
            if (recv_len >= sizeof(StatusRequest))
            {
                StatusRequest *req = (StatusRequest *)buffer;
                process_status_request(req);
            }
            break;

        case MSG_NACK:
            if (recv_len >= sizeof(NackMessage))
            {
                NackMessage *nack = (NackMessage *)buffer;
                process_other_nack(nack);
            }
            break;

        case MSG_END:
            if (recv_len >= sizeof(EndMessage))
            {
                EndMessage *end_msg = (EndMessage *)buffer;
                process_end_message(end_msg);
            }
            break;

        default:
            break;
        }
    }

    return NULL;
}

// ========== 清理资源 ==========
void cleanup_receiver_session()
{
    if (g_session.output_file)
    {
        fclose(g_session.output_file);
    }
    if (g_session.windows)
    {
        for (uint32_t i = 0; i < g_session.total_windows; i++)
        {
            if (g_session.windows[i].data_buffer)
            {
                free(g_session.windows[i].data_buffer);
            }
        }
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
        printf("Usage: %s <uav_id>\n", argv[0]);
        return 1;
    }

    g_uav_id = atoi(argv[1]);

    // 初始化随机数种子（用于NACK退避）
    srand(time(NULL) + g_uav_id);

    // 禁用输出缓冲，确保日志立即写入
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("========================================\n");
    printf("  UAV File Broadcast Receiver\n");
    printf("  UAV ID: %u\n", g_uav_id);
    printf("========================================\n");
    fflush(stdout);

    // 创建组播socket
    g_multicast_sock = create_multicast_socket(false);
    if (g_multicast_sock < 0)
    {
        fprintf(stderr, "Failed to create multicast socket\n");
        return 1;
    }

    printf("[UAV %u] Listening for broadcasts on %s:%d\n",
           g_uav_id, MULTICAST_GROUP, MULTICAST_PORT);

    // 启动消息接收线程
    pthread_t receiver_thread;
    pthread_create(&receiver_thread, NULL, message_receiver_thread, NULL);

    // 主线程等待
    pthread_join(receiver_thread, NULL);

    cleanup_receiver_session();
    return 0;
}
