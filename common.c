#include "broadcast_protocol.h"

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
    // 注意：丢包模拟已移到接收方，确保每个接收方独立丢包
    // 发送方总是发送所有数据包

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
