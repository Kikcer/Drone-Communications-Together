#include "broadcast_protocol.h"

// ========== 全局传输层状态 ==========
static struct
{
    int sock;
    PacketQueue tx_queue;
    PacketQueue rx_queue;
    pthread_t tx_thread;
    pthread_t rx_thread;
    bool running;
} g_transport;

// ========== CRC16校验实现 ==========
uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
        {
            if (crc & 0x0001)
            {
                crc = (crc >> 1) ^ 0xA001;
            }
            else
            {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

// ========== 简单hash计算 ==========
uint32_t simple_hash(const uint8_t *data, size_t len)
{
    uint32_t hash = 0x811C9DC5; // FNV-1a初始值
    for (size_t i = 0; i < len; i++)
    {
        hash ^= data[i];
        hash *= 0x01000193;
    }
    return hash;
}

// ========== 创建组播socket ==========
int create_multicast_socket(bool sender)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("socket creation failed");
        return -1;
    }

    // 设置socket选项允许地址重用
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt SO_REUSEADDR failed");
        close(sock);
        return -1;
    }

    if (sender)
    {
        // 发送方：设置TTL（跳数限制）
        unsigned char ttl = 32;
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
        {
            perror("setsockopt IP_MULTICAST_TTL failed");
            close(sock);
            return -1;
        }

        // 启用回环（本地测试时需要，实际部署时可以禁用）
        unsigned char loop = 1; // 改为1，启用回环
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)
        {
            perror("setsockopt IP_MULTICAST_LOOP failed");
            close(sock);
            return -1;
        }
    }
    else
    {
        // 接收方：绑定端口
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        local_addr.sin_port = htons(MULTICAST_PORT);

        if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
        {
            perror("bind failed");
            close(sock);
            return -1;
        }

        // 加入组播组
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        {
            perror("setsockopt IP_ADD_MEMBERSHIP failed");
            close(sock);
            return -1;
        }
    }

    return sock;
}

// ========== 发送组播消息 ==========
int send_multicast(int sock, const void *data, size_t len)
{
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(MULTICAST_GROUP);
    dest_addr.sin_port = htons(MULTICAST_PORT);

    ssize_t sent = sendto(sock, data, len, 0,
                          (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (sent < 0)
    {
        perror("sendto failed");
        return -1;
    }
    return sent;
}

// ========== 接收组播消息 ==========
int recv_multicast(int sock, void *buffer, size_t len, struct sockaddr_in *src_addr)
{
    socklen_t addr_len = sizeof(*src_addr);
    ssize_t recv_len = recvfrom(sock, buffer, len, 0,
                                (struct sockaddr *)src_addr, &addr_len);
    if (recv_len < 0)
    {
        perror("recvfrom failed");
        return -1;
    }
    return recv_len;
}

// ========== 获取当前时间（毫秒） ==========
uint64_t get_time_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ========== 打印bitmap ==========
void print_bitmap(uint64_t bitmap)
{
    for (int i = 0; i < 64; i++)
    {
        printf("%d", (bitmap & (1ULL << i)) ? 1 : 0);
        if ((i + 1) % 8 == 0)
            printf(" ");
    }
    printf("\n");
}

// ========== 计算bitmap中置位(1)的数量 ==========
int count_set_bits(uint64_t bitmap)
{
    int count = 0;
    for (int i = 0; i < 64; i++)
    {
        if (bitmap & (1ULL << i))
        {
            count++;
        }
    }
    return count;
}

// ========== 判断bitmap1是否包含bitmap2的所有缺失块 ==========
bool bitmap_covers(uint64_t bitmap1, uint64_t bitmap2)
{
    // bitmap中0表示缺失，1表示已收到
    // bitmap1包含bitmap2，意味着bitmap2缺的块在bitmap1中也缺
    uint64_t missing1 = ~bitmap1; // bitmap1缺失的块
    uint64_t missing2 = ~bitmap2; // bitmap2缺失的块

    // 如果missing2是missing1的子集，则bitmap1覆盖bitmap2
    return (missing2 & missing1) == missing2;
}

// ========== 队列操作函数 ==========

static void queue_init(PacketQueue *q)
{
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void queue_push(PacketQueue *q, const void *data, size_t len)
{
    pthread_mutex_lock(&q->mutex);

    // 如果队列满，等待（阻塞）
    while (q->count >= QUEUE_CAPACITY)
    {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }

    memcpy(q->data[q->tail], data, len);
    q->lens[q->tail] = len;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static size_t queue_pop(PacketQueue *q, void *buffer, size_t max_len)
{
    pthread_mutex_lock(&q->mutex);

    // 如果队列空，等待（阻塞）
    while (q->count == 0)
    {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    size_t len = q->lens[q->head];
    if (len > max_len)
    {
        len = max_len; // 截断
    }
    memcpy(buffer, q->data[q->head], len);
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    return len;
}

// ========== 传输层线程函数 ==========

static void *tx_thread_func(void *arg)
{
    uint8_t buffer[MAX_PACKET_SIZE];
    while (g_transport.running)
    {
        size_t len = queue_pop(&g_transport.tx_queue, buffer, sizeof(buffer));
        if (len > 0)
        {
            send_multicast(g_transport.sock, buffer, len);
        }
    }
    return NULL;
}

static void *rx_thread_func(void *arg)
{
    uint8_t buffer[MAX_PACKET_SIZE];
    struct sockaddr_in src_addr;
    while (g_transport.running)
    {
        int len = recv_multicast(g_transport.sock, buffer, sizeof(buffer), &src_addr);
        if (len > 0)
        {
            queue_push(&g_transport.rx_queue, buffer, len);
        }
    }
    return NULL;
}

// ========== 传输层接口实现 ==========

bool transport_init(bool is_sender)
{
    g_transport.sock = create_multicast_socket(is_sender);
    if (g_transport.sock < 0)
    {
        return false;
    }

    queue_init(&g_transport.tx_queue);
    queue_init(&g_transport.rx_queue);
    g_transport.running = true;

    // 启动Tx线程
    if (pthread_create(&g_transport.tx_thread, NULL, tx_thread_func, NULL) != 0)
    {
        perror("Failed to create Tx thread");
        close(g_transport.sock);
        return false;
    }

    // 启动Rx线程
    if (pthread_create(&g_transport.rx_thread, NULL, rx_thread_func, NULL) != 0)
    {
        perror("Failed to create Rx thread");
        g_transport.running = false;
        pthread_join(g_transport.tx_thread, NULL);
        close(g_transport.sock);
        return false;
    }

    return true;
}

void transport_send(const void *data, size_t len)
{
    if (!g_transport.running)
        return;
    queue_push(&g_transport.tx_queue, data, len);
}

size_t transport_recv(void *buffer, size_t max_len)
{
    if (!g_transport.running)
        return 0;
    return queue_pop(&g_transport.rx_queue, buffer, max_len);
}

void transport_close()
{
    g_transport.running = false;
    // 唤醒可能阻塞的线程
    pthread_cancel(g_transport.tx_thread);
    pthread_cancel(g_transport.rx_thread);

    pthread_join(g_transport.tx_thread, NULL);
    pthread_join(g_transport.rx_thread, NULL);

    close(g_transport.sock);
}
