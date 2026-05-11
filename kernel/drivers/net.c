#include "kernel.h"
#include "net.h"

#define NET_ETH_TYPE_ARP 0x0806u
#define NET_ETH_TYPE_IPV4 0x0800u
#define NET_IP_PROTO_TCP 6u
#define NET_ARP_OP_REQUEST 1u
#define NET_ARP_OP_REPLY 2u
#define NET_TIMEOUT_TICKS 500ull
#define NET_DEFAULT_IP ((10u << 24) | (0u << 16) | (2u << 8) | 15u)
#define NET_DEFAULT_MASK ((255u << 24) | (255u << 16) | (255u << 8))
#define NET_DEFAULT_GATEWAY ((10u << 24) | (0u << 16) | (2u << 8) | 2u)

static net_device_t devices[NET_MAX_DEVICES];
static uint32_t device_count;

typedef struct __attribute__((packed)) {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} net_eth_header_t;

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} net_arp_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} net_ipv4_header_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_offset;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} net_tcp_header_t;

typedef struct {
    int active;
    uint32_t device;
    uint32_t wanted_ip;
    uint8_t mac[6];
    int found;
} net_arp_wait_t;

typedef struct {
    int active;
    uint32_t device;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t remote_mac[6];
    int connected;
    int closed;
    int reset;
    char *out;
    uint32_t out_capacity;
    uint32_t out_size;
    int header_done;
    char header_tail[4];
    uint32_t header_tail_count;
} net_tcp_get_t;

static net_arp_wait_t arp_wait;
static net_tcp_get_t tcp_get;
static uint16_t next_ip_id = 1u;
static uint16_t next_local_port = 49152u;
static uint32_t local_ip = NET_DEFAULT_IP;
static uint32_t local_netmask = NET_DEFAULT_MASK;
static uint32_t local_gateway = NET_DEFAULT_GATEWAY;

static void copy_name(char *dst, const char *src) {
    uint32_t i = 0;

    while (src[i] && i + 1u < sizeof(devices[0].name)) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

void net_init(void) {
    for (uint32_t i = 0; i < NET_MAX_DEVICES; ++i) {
        devices[i].present = 0;
    }

    device_count = 0;
    e1000_init();
}

void net_set_ipv4_config(uint32_t address, uint32_t netmask, uint32_t gateway) {
    local_ip = address;
    local_netmask = netmask;
    local_gateway = gateway;
}

uint32_t net_ipv4_address(void) {
    return local_ip;
}

uint32_t net_ipv4_netmask(void) {
    return local_netmask;
}

uint32_t net_ipv4_gateway(void) {
    return local_gateway;
}

int net_register_device(const char *name,
                        const uint8_t mac[NET_MAC_SIZE],
                        int link_up,
                        net_send_frame_t send_frame,
                        net_poll_t poll,
                        void *ctx) {
    if (device_count >= NET_MAX_DEVICES || !name || !mac || !send_frame || !poll) {
        return -1;
    }

    uint32_t index = device_count++;
    devices[index].present = 1;
    copy_name(devices[index].name, name);
    for (uint32_t i = 0; i < NET_MAC_SIZE; ++i) {
        devices[index].mac[i] = mac[i];
    }
    devices[index].link_up = link_up ? 1 : 0;
    devices[index].rx_packets = 0;
    devices[index].tx_packets = 0;
    devices[index].rx_dropped = 0;
    devices[index].tx_errors = 0;
    devices[index].send_frame = send_frame;
    devices[index].poll = poll;
    devices[index].ctx = ctx;
    return (int)index;
}

uint32_t net_device_count(void) {
    return device_count;
}

net_device_t *net_get_device(uint32_t index) {
    if (index >= device_count || !devices[index].present) {
        return 0;
    }

    return &devices[index];
}

const net_device_t *net_get_device_const(uint32_t index) {
    return net_get_device(index);
}

void net_set_link(uint32_t index, int link_up) {
    net_device_t *dev = net_get_device(index);
    if (dev) {
        dev->link_up = link_up ? 1 : 0;
    }
}

void net_record_rx(uint32_t index) {
    net_device_t *dev = net_get_device(index);
    if (dev) {
        ++dev->rx_packets;
    }
}

void net_record_rx_drop(uint32_t index) {
    net_device_t *dev = net_get_device(index);
    if (dev) {
        ++dev->rx_dropped;
    }
}

void net_record_tx(uint32_t index) {
    net_device_t *dev = net_get_device(index);
    if (dev) {
        ++dev->tx_packets;
    }
}

void net_record_tx_error(uint32_t index) {
    net_device_t *dev = net_get_device(index);
    if (dev) {
        ++dev->tx_errors;
    }
}

int net_send_frame(uint32_t index, const void *data, uint32_t size) {
    net_device_t *dev = net_get_device(index);
    if (!dev || !data || size < NET_MIN_FRAME_SIZE || size > NET_MAX_FRAME_SIZE) {
        return -1;
    }

    return dev->send_frame(dev->ctx, data, size);
}

int net_poll_device(uint32_t index) {
    net_device_t *dev = net_get_device(index);
    if (!dev) {
        return -1;
    }

    return dev->poll(dev->ctx);
}

static uint16_t net_bswap16(uint16_t value) {
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint32_t net_bswap32(uint32_t value) {
    return ((value & 0x000000FFu) << 24) |
           ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8) |
           ((value & 0xFF000000u) >> 24);
}

static uint16_t net_htons(uint16_t value) { return net_bswap16(value); }
static uint16_t net_ntohs(uint16_t value) { return net_bswap16(value); }
static uint32_t net_htonl(uint32_t value) { return net_bswap32(value); }
static uint32_t net_ntohl(uint32_t value) { return net_bswap32(value); }

static void net_copy(void *dst, const void *src, uint32_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (uint32_t i = 0; i < size; ++i) {
        d[i] = s[i];
    }
}

static void net_zero(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t *)ptr;

    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static int net_starts_with(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text != *prefix) {
            return 0;
        }
        ++text;
        ++prefix;
    }
    return 1;
}

static uint32_t net_strlen(const char *text) {
    uint32_t len = 0;

    while (text[len]) {
        ++len;
    }
    return len;
}

static uint16_t net_checksum_finish(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint32_t net_checksum_add(uint32_t sum, const void *data, uint32_t size) {
    const uint8_t *p = (const uint8_t *)data;

    while (size > 1u) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        size -= 2;
    }

    if (size != 0) {
        sum += ((uint16_t)p[0] << 8);
    }

    return sum;
}

static uint16_t net_checksum(const void *data, uint32_t size) {
    return net_checksum_finish(net_checksum_add(0, data, size));
}

static int net_parse_dec_octet(const char **text, uint32_t *out) {
    uint32_t value = 0;
    uint32_t digits = 0;

    while (**text >= '0' && **text <= '9') {
        value = value * 10u + (uint32_t)(**text - '0');
        if (value > 255u) {
            return -1;
        }
        ++digits;
        ++(*text);
    }

    if (digits == 0) {
        return -1;
    }

    *out = value;
    return 0;
}

static int net_parse_ipv4(const char **text, uint32_t *out) {
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    uint32_t d = 0;

    if (net_parse_dec_octet(text, &a) != 0 || **text != '.') return -1;
    ++(*text);
    if (net_parse_dec_octet(text, &b) != 0 || **text != '.') return -1;
    ++(*text);
    if (net_parse_dec_octet(text, &c) != 0 || **text != '.') return -1;
    ++(*text);
    if (net_parse_dec_octet(text, &d) != 0) return -1;

    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

int net_parse_ipv4_addr(const char *text, uint32_t *out) {
    const char *s = text;

    if (text == 0 || out == 0 || net_parse_ipv4(&s, out) != 0 || *s != '\0') {
        return -1;
    }

    return 0;
}

static int net_parse_port(const char **text, uint16_t *out) {
    uint32_t value = 0;
    uint32_t digits = 0;

    while (**text >= '0' && **text <= '9') {
        value = value * 10u + (uint32_t)(**text - '0');
        if (value > 65535u) {
            return -1;
        }
        ++digits;
        ++(*text);
    }

    if (digits == 0 || value == 0) {
        return -1;
    }

    *out = (uint16_t)value;
    return 0;
}

static int net_parse_url(const char *url,
                         uint32_t *ip,
                         uint16_t *port,
                         const char **path) {
    const char *s = url;

    if (!net_starts_with(s, "http://")) {
        return -1;
    }
    s += 7;

    if (net_parse_ipv4(&s, ip) != 0) {
        return -1;
    }

    *port = 80u;
    if (*s == ':') {
        ++s;
        if (net_parse_port(&s, port) != 0) {
            return -1;
        }
    }

    if (*s == '\0') {
        *path = "/";
        return 0;
    }

    if (*s != '/') {
        return -1;
    }

    *path = s;
    return 0;
}

static int net_send_arp_request(uint32_t index, uint32_t target_ip) {
    uint8_t frame[NET_MIN_FRAME_SIZE];
    net_eth_header_t *eth = (net_eth_header_t *)frame;
    net_arp_packet_t *arp = (net_arp_packet_t *)(frame + sizeof(net_eth_header_t));
    const net_device_t *dev = net_get_device_const(index);

    if (!dev) {
        return -1;
    }

    for (uint32_t i = 0; i < sizeof(frame); ++i) {
        frame[i] = 0;
    }
    for (uint32_t i = 0; i < 6u; ++i) {
        eth->dst[i] = 0xFFu;
        eth->src[i] = dev->mac[i];
        arp->sha[i] = dev->mac[i];
    }
    eth->type = net_htons(NET_ETH_TYPE_ARP);
    arp->htype = net_htons(1u);
    arp->ptype = net_htons(NET_ETH_TYPE_IPV4);
    arp->hlen = 6u;
    arp->plen = 4u;
    arp->oper = net_htons(NET_ARP_OP_REQUEST);
    arp->spa = net_htonl(local_ip);
    arp->tpa = net_htonl(target_ip);

    return net_send_frame(index, frame, sizeof(frame));
}

static int net_arp_resolve(uint32_t index, uint32_t ip, uint8_t mac[6]) {
    unsigned long long start = timer_ticks();

    arp_wait.active = 1;
    arp_wait.device = index;
    arp_wait.wanted_ip = ip;
    arp_wait.found = 0;
    net_zero(arp_wait.mac, sizeof(arp_wait.mac));

    for (;;) {
        if (net_send_arp_request(index, ip) != 0) {
            arp_wait.active = 0;
            return -1;
        }

        while (timer_ticks() - start < NET_TIMEOUT_TICKS) {
            (void)net_poll_device(index);
            if (arp_wait.found) {
                net_copy(mac, arp_wait.mac, 6u);
                arp_wait.active = 0;
                return 0;
            }
        }

        arp_wait.active = 0;
        return -1;
    }
}

static int net_send_tcp(uint32_t index,
                        const uint8_t dst_mac[6],
                        uint32_t dst_ip,
                        uint16_t src_port,
                        uint16_t dst_port,
                        uint32_t seq,
                        uint32_t ack,
                        uint8_t flags,
                        const void *payload,
                        uint32_t payload_size) {
    uint8_t frame[NET_MAX_FRAME_SIZE];
    net_eth_header_t *eth = (net_eth_header_t *)frame;
    net_ipv4_header_t *ip = (net_ipv4_header_t *)(frame + sizeof(net_eth_header_t));
    net_tcp_header_t *tcp = (net_tcp_header_t *)((uint8_t *)ip + sizeof(net_ipv4_header_t));
    const net_device_t *dev = net_get_device_const(index);
    uint32_t tcp_size = sizeof(net_tcp_header_t) + payload_size;
    uint32_t ip_size = sizeof(net_ipv4_header_t) + tcp_size;
    uint32_t frame_size = sizeof(net_eth_header_t) + ip_size;
    uint32_t sum = 0;

    if (!dev || frame_size > NET_MAX_FRAME_SIZE) {
        return -1;
    }

    net_zero(frame, sizeof(frame));
    net_copy(eth->dst, dst_mac, 6u);
    net_copy(eth->src, dev->mac, 6u);
    eth->type = net_htons(NET_ETH_TYPE_IPV4);

    ip->ver_ihl = 0x45u;
    ip->tos = 0;
    ip->total_length = net_htons((uint16_t)ip_size);
    ip->id = net_htons(next_ip_id++);
    ip->flags_fragment = net_htons(0x4000u);
    ip->ttl = 64u;
    ip->proto = NET_IP_PROTO_TCP;
    ip->src = net_htonl(local_ip);
    ip->dst = net_htonl(dst_ip);
    ip->checksum = 0;
    ip->checksum = net_htons(net_checksum(ip, sizeof(net_ipv4_header_t)));

    tcp->src_port = net_htons(src_port);
    tcp->dst_port = net_htons(dst_port);
    tcp->seq = net_htonl(seq);
    tcp->ack = net_htonl(ack);
    tcp->data_offset = (uint8_t)(sizeof(net_tcp_header_t) / 4u) << 4;
    tcp->flags = flags;
    tcp->window = net_htons(4096u);
    tcp->checksum = 0;
    tcp->urgent = 0;
    if (payload_size != 0) {
        net_copy((uint8_t *)tcp + sizeof(net_tcp_header_t), payload, payload_size);
    }

    sum += (uint16_t)(local_ip >> 16);
    sum += (uint16_t)(local_ip & 0xFFFFu);
    sum += (uint16_t)(dst_ip >> 16);
    sum += (uint16_t)(dst_ip & 0xFFFFu);
    sum += NET_IP_PROTO_TCP;
    sum += tcp_size;
    sum = net_checksum_add(sum, tcp, tcp_size);
    tcp->checksum = net_htons(net_checksum_finish(sum));

    if (frame_size < NET_MIN_FRAME_SIZE) {
        frame_size = NET_MIN_FRAME_SIZE;
    }

    return net_send_frame(index, frame, frame_size);
}

static void net_http_copy_body_byte(net_tcp_get_t *ctx, char ch) {
    if (!ctx->header_done) {
        if (ctx->header_tail_count < sizeof(ctx->header_tail)) {
            ctx->header_tail[ctx->header_tail_count++] = ch;
        } else {
            ctx->header_tail[0] = ctx->header_tail[1];
            ctx->header_tail[1] = ctx->header_tail[2];
            ctx->header_tail[2] = ctx->header_tail[3];
            ctx->header_tail[3] = ch;
        }

        if (ctx->header_tail_count == 4u &&
            ctx->header_tail[0] == '\r' &&
            ctx->header_tail[1] == '\n' &&
            ctx->header_tail[2] == '\r' &&
            ctx->header_tail[3] == '\n') {
            ctx->header_done = 1;
        }
        return;
    }

    if (ctx->out_size + 1u < ctx->out_capacity) {
        ctx->out[ctx->out_size++] = ch;
        ctx->out[ctx->out_size] = '\0';
    }
}

static void net_handle_arp(uint32_t index, const uint8_t *frame, uint32_t size) {
    const net_arp_packet_t *arp;
    uint32_t sender_ip;

    if (size < sizeof(net_eth_header_t) + sizeof(net_arp_packet_t)) {
        return;
    }

    arp = (const net_arp_packet_t *)(frame + sizeof(net_eth_header_t));
    sender_ip = net_ntohl(arp->spa);

    if (arp_wait.active &&
        arp_wait.device == index &&
        arp_wait.wanted_ip == sender_ip &&
        net_ntohs(arp->oper) == NET_ARP_OP_REPLY) {
        net_copy(arp_wait.mac, arp->sha, 6u);
        arp_wait.found = 1;
    }
}

static void net_handle_tcp(uint32_t index,
                           const uint8_t *frame,
                           uint32_t size,
                           const net_ipv4_header_t *ip,
                           uint32_t ip_header_size,
                           uint32_t ip_total_size) {
    const net_tcp_header_t *tcp;
    const uint8_t *payload;
    uint32_t tcp_header_size;
    uint32_t payload_size;
    uint32_t seq;
    uint32_t src_ip;

    if (!tcp_get.active || tcp_get.device != index || ip_total_size < ip_header_size + sizeof(net_tcp_header_t)) {
        return;
    }

    tcp = (const net_tcp_header_t *)((const uint8_t *)ip + ip_header_size);
    tcp_header_size = (uint32_t)(tcp->data_offset >> 4) * 4u;
    if (tcp_header_size < sizeof(net_tcp_header_t) ||
        ip_total_size < ip_header_size + tcp_header_size ||
        size < sizeof(net_eth_header_t) + ip_header_size + tcp_header_size) {
        return;
    }

    src_ip = net_ntohl(ip->src);
    if (src_ip != tcp_get.remote_ip ||
        net_ntohs(tcp->src_port) != tcp_get.remote_port ||
        net_ntohs(tcp->dst_port) != tcp_get.local_port) {
        return;
    }

    if (tcp->flags & 0x04u) {
        tcp_get.reset = 1;
        return;
    }

    seq = net_ntohl(tcp->seq);
    payload = (const uint8_t *)tcp + tcp_header_size;
    payload_size = ip_total_size - ip_header_size - tcp_header_size;

    if (!tcp_get.connected && (tcp->flags & 0x12u) == 0x12u) {
        tcp_get.ack = seq + 1u;
        tcp_get.connected = 1;
        (void)net_send_tcp(index,
                           tcp_get.remote_mac,
                           tcp_get.remote_ip,
                           tcp_get.local_port,
                           tcp_get.remote_port,
                           tcp_get.seq,
                           tcp_get.ack,
                           0x10u,
                           0,
                           0);
        return;
    }

    if (payload_size != 0 && seq == tcp_get.ack) {
        for (uint32_t i = 0; i < payload_size; ++i) {
            net_http_copy_body_byte(&tcp_get, (char)payload[i]);
        }
        tcp_get.ack += payload_size;
        (void)net_send_tcp(index,
                           tcp_get.remote_mac,
                           tcp_get.remote_ip,
                           tcp_get.local_port,
                           tcp_get.remote_port,
                           tcp_get.seq,
                           tcp_get.ack,
                           0x10u,
                           0,
                           0);
    }

    if (tcp->flags & 0x01u) {
        tcp_get.ack = seq + payload_size + 1u;
        tcp_get.closed = 1;
        (void)net_send_tcp(index,
                           tcp_get.remote_mac,
                           tcp_get.remote_ip,
                           tcp_get.local_port,
                           tcp_get.remote_port,
                           tcp_get.seq,
                           tcp_get.ack,
                           0x11u,
                           0,
                           0);
        ++tcp_get.seq;
    }

    (void)frame;
}

static void net_handle_ipv4(uint32_t index, const uint8_t *frame, uint32_t size) {
    const net_ipv4_header_t *ip;
    uint32_t ip_header_size;
    uint32_t ip_total_size;

    if (size < sizeof(net_eth_header_t) + sizeof(net_ipv4_header_t)) {
        return;
    }

    ip = (const net_ipv4_header_t *)(frame + sizeof(net_eth_header_t));
    ip_header_size = (uint32_t)(ip->ver_ihl & 0x0Fu) * 4u;
    ip_total_size = net_ntohs(ip->total_length);
    if ((ip->ver_ihl >> 4) != 4u ||
        ip_header_size < sizeof(net_ipv4_header_t) ||
        ip_total_size < ip_header_size ||
        size < sizeof(net_eth_header_t) + ip_total_size ||
        ip->proto != NET_IP_PROTO_TCP ||
        net_ntohl(ip->dst) != local_ip) {
        return;
    }

    net_handle_tcp(index, frame, size, ip, ip_header_size, ip_total_size);
}

void net_receive_frame(uint32_t index, const void *data, uint32_t size) {
    const uint8_t *frame = (const uint8_t *)data;
    const net_eth_header_t *eth;
    uint16_t type;

    if (index >= device_count || data == 0 || size < sizeof(net_eth_header_t)) {
        return;
    }

    eth = (const net_eth_header_t *)frame;
    type = net_ntohs(eth->type);
    if (type == NET_ETH_TYPE_ARP) {
        net_handle_arp(index, frame, size);
    } else if (type == NET_ETH_TYPE_IPV4) {
        net_handle_ipv4(index, frame, size);
    }
}

static int net_wait_for_tcp_connected(uint32_t index) {
    unsigned long long start = timer_ticks();

    while (timer_ticks() - start < NET_TIMEOUT_TICKS) {
        (void)net_poll_device(index);
        if (tcp_get.connected) {
            return 0;
        }
        if (tcp_get.reset) {
            return -1;
        }
    }

    return -1;
}

static int net_wait_for_tcp_done(uint32_t index) {
    unsigned long long start = timer_ticks();
    uint32_t last_size = tcp_get.out_size;

    while (timer_ticks() - start < NET_TIMEOUT_TICKS) {
        (void)net_poll_device(index);
        if (tcp_get.closed) {
            return 0;
        }
        if (tcp_get.reset) {
            return -1;
        }
        if (tcp_get.out_size != last_size) {
            last_size = tcp_get.out_size;
            start = timer_ticks();
        }
    }

    return tcp_get.header_done ? 0 : -1;
}

static int net_build_http_request(const char *path,
                                  uint32_t ip,
                                  char *request,
                                  uint32_t request_size) {
    uint32_t pos = 0;
    uint32_t parts[4];

    parts[0] = (ip >> 24) & 0xFFu;
    parts[1] = (ip >> 16) & 0xFFu;
    parts[2] = (ip >> 8) & 0xFFu;
    parts[3] = ip & 0xFFu;

    if (request_size < 64u) {
        return -1;
    }

#define APPEND_CH(ch) do { if (pos + 1u >= request_size) return -1; request[pos++] = (ch); request[pos] = '\0'; } while (0)
#define APPEND_TEXT(text) do { const char *ap = (text); while (*ap) { APPEND_CH(*ap++); } } while (0)
#define APPEND_DEC(value) do { \
    char digits[10]; \
    uint32_t count = 0; \
    uint32_t v = (value); \
    if (v == 0) { APPEND_CH('0'); } \
    while (v != 0) { digits[count++] = (char)('0' + (v % 10u)); v /= 10u; } \
    while (count != 0) { APPEND_CH(digits[--count]); } \
} while (0)

    APPEND_TEXT("GET ");
    APPEND_TEXT(path);
    APPEND_TEXT(" HTTP/1.0\r\nHost: ");
    APPEND_DEC(parts[0]); APPEND_CH('.');
    APPEND_DEC(parts[1]); APPEND_CH('.');
    APPEND_DEC(parts[2]); APPEND_CH('.');
    APPEND_DEC(parts[3]);
    APPEND_TEXT("\r\nConnection: close\r\n\r\n");

#undef APPEND_DEC
#undef APPEND_TEXT
#undef APPEND_CH

    return 0;
}

int net_http_get(uint32_t index,
                 const char *url,
                 char *out,
                 uint32_t out_capacity,
                 uint32_t *out_size) {
    uint32_t ip = 0;
    uint16_t port = 80u;
    const char *path = 0;
    uint8_t mac[6];
    uint32_t arp_ip = 0;
    char request[512];
    uint32_t request_size;

    if (out_size) {
        *out_size = 0;
    }
    if (!url || !out || out_capacity == 0 || index >= device_count) {
        return -1;
    }
    out[0] = '\0';

    if (net_parse_url(url, &ip, &port, &path) != 0) {
        return -2;
    }

    if ((ip & local_netmask) == (local_ip & local_netmask)) {
        arp_ip = ip;
    } else if (local_gateway != 0) {
        arp_ip = local_gateway;
    } else {
        return -7;
    }

    if (net_arp_resolve(index, arp_ip, mac) != 0) {
        return -3;
    }

    if (net_build_http_request(path, ip, request, sizeof(request)) != 0) {
        return -2;
    }
    request_size = net_strlen(request);

    net_zero(&tcp_get, sizeof(tcp_get));
    tcp_get.active = 1;
    tcp_get.device = index;
    tcp_get.remote_ip = ip;
    tcp_get.remote_port = port;
    tcp_get.local_port = next_local_port++;
    tcp_get.seq = 0x10000000u + (uint32_t)timer_ticks();
    tcp_get.ack = 0;
    net_copy(tcp_get.remote_mac, mac, 6u);
    tcp_get.out = out;
    tcp_get.out_capacity = out_capacity;
    tcp_get.out_size = 0;

    if (net_send_tcp(index, mac, ip, tcp_get.local_port, port, tcp_get.seq, 0, 0x02u, 0, 0) != 0) {
        tcp_get.active = 0;
        return -4;
    }
    ++tcp_get.seq;

    if (net_wait_for_tcp_connected(index) != 0) {
        tcp_get.active = 0;
        return -5;
    }

    if (net_send_tcp(index,
                     mac,
                     ip,
                     tcp_get.local_port,
                     port,
                     tcp_get.seq,
                     tcp_get.ack,
                     0x18u,
                     request,
                     request_size) != 0) {
        tcp_get.active = 0;
        return -4;
    }
    tcp_get.seq += request_size;

    if (net_wait_for_tcp_done(index) != 0) {
        tcp_get.active = 0;
        return -6;
    }

    if (out_size) {
        *out_size = tcp_get.out_size;
    }
    tcp_get.active = 0;
    return 0;
}
