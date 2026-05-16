#include "kernel.h"
#include "net.h"

#define NET_ETH_TYPE_ARP 0x0806u
#define NET_ETH_TYPE_IPV4 0x0800u
#define NET_ETH_TYPE_VLAN 0x8100u
#define NET_ETH_TYPE_QINQ 0x88A8u
#define NET_IP_PROTO_TCP 6u
#define NET_IP_PROTO_UDP 17u
#define NET_ARP_OP_REQUEST 1u
#define NET_ARP_OP_REPLY 2u
#define NET_TIMEOUT_TICKS 500ull
#define NET_DEFAULT_IP ((10u << 24) | (0u << 16) | (2u << 8) | 15u)
#define NET_DEFAULT_MASK ((255u << 24) | (255u << 16) | (255u << 8))
#define NET_DEFAULT_GATEWAY ((10u << 24) | (0u << 16) | (2u << 8) | 2u)
#define NET_DEFAULT_DNS ((10u << 24) | (0u << 16) | (2u << 8) | 3u)
#define NET_DHCP_CLIENT_PORT 68u
#define NET_DHCP_SERVER_PORT 67u
#define NET_DNS_PORT 53u

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

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} net_udp_header_t;

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
} net_dhcp_packet_t;

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
    int full;
    char header_tail[4];
    uint32_t header_tail_count;
} net_tcp_get_t;

typedef struct {
    int active;
    uint32_t device;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t seq;
    uint32_t ack;
    uint32_t tx_acked;
    uint8_t remote_mac[6];
    int connected;
    int closed;
    int reset;
    uint8_t *rx_buffer;
    uint32_t rx_capacity;
    uint32_t rx_size;
} net_tcp_stream_t;

typedef struct {
    int active;
    uint32_t device;
    uint32_t xid;
    uint8_t wanted_type;
    int found;
    uint32_t offered_ip;
    uint32_t server_ip;
    uint32_t subnet_mask;
    uint32_t router;
    uint32_t dns;
} net_dhcp_wait_t;

typedef struct {
    int active;
    uint32_t device;
    uint16_t id;
    uint16_t local_port;
    int found;
    uint32_t ip;
} net_dns_wait_t;

static net_arp_wait_t arp_wait;
static net_tcp_get_t tcp_get;
static net_tcp_stream_t tcp_stream;
static net_dhcp_wait_t dhcp_wait;
static net_dns_wait_t dns_wait;
static uint16_t next_ip_id = 1u;
static uint16_t next_local_port = 49152u;
static uint32_t local_ip = NET_DEFAULT_IP;
static uint32_t local_netmask = NET_DEFAULT_MASK;
static uint32_t local_gateway = NET_DEFAULT_GATEWAY;
static uint32_t local_dns = NET_DEFAULT_DNS;
static net_debug_info_t debug_info;

static void net_zero(void *ptr, uint32_t size);
static int net_hostname_is_ipv4(const char *name, uint32_t *out);

static void copy_name(char *dst, const char *src) {
    uint32_t i = 0;

    while (src[i] && i + 1u < sizeof(devices[0].name)) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

static void copy_driver(char *dst, const char *src) {
    uint32_t i = 0;

    while (src[i] && i + 1u < sizeof(devices[0].driver)) {
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
    net_zero(&debug_info, sizeof(debug_info));
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

void net_set_dns_server(uint32_t dns) {
    local_dns = dns;
}

uint32_t net_dns_server(void) {
    return local_dns;
}

int net_register_device(const char *name,
                        const char *driver,
                        uint16_t vendor_id,
                        uint16_t device_id,
                        const uint8_t mac[NET_MAC_SIZE],
                        int link_up,
                        net_send_frame_t send_frame,
                        net_poll_t poll,
                        void *ctx) {
    if (device_count >= NET_MAX_DEVICES || !name || !driver || !mac || !send_frame || !poll) {
        return -1;
    }

    uint32_t index = device_count++;
    devices[index].present = 1;
    copy_name(devices[index].name, name);
    copy_driver(devices[index].driver, driver);
    devices[index].vendor_id = vendor_id;
    devices[index].device_id = device_id;
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
                         char *host,
                         uint32_t host_capacity,
                         const char **path,
                         int *is_https) {
    const char *s = url;
    uint32_t host_len = 0;

    if (net_starts_with(s, "http://")) {
        s += 7;
        *is_https = 0;
    } else if (net_starts_with(s, "https://")) {
        s += 8;
        *is_https = 1;
    } else {
        return -1;
    }

    while (s[host_len] &&
           s[host_len] != ':' &&
           s[host_len] != '/' &&
           host_len + 1u < host_capacity) {
        host[host_len] = s[host_len];
        ++host_len;
    }
    host[host_len] = '\0';
    if (host_len == 0u) {
        return -1;
    }
    s += host_len;

    if (!net_hostname_is_ipv4(host, ip)) {
        *ip = 0;
    }

    *port = *is_https ? 443u : 80u;
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

    ++debug_info.arp_requests;
    debug_info.last_arp_requested_ip = target_ip;

    if (!dev) {
        ++debug_info.arp_tx_errors;
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

    if (net_send_frame(index, frame, sizeof(frame)) != 0) {
        ++debug_info.arp_tx_errors;
        return -1;
    }

    return 0;
}

static int net_send_udp(uint32_t index,
                        const uint8_t dst_mac[6],
                        uint32_t dst_ip,
                        uint16_t src_port,
                        uint16_t dst_port,
                        const void *payload,
                        uint32_t payload_size) {
    uint8_t frame[NET_MAX_FRAME_SIZE];
    net_eth_header_t *eth = (net_eth_header_t *)frame;
    net_ipv4_header_t *ip = (net_ipv4_header_t *)(frame + sizeof(net_eth_header_t));
    net_udp_header_t *udp = (net_udp_header_t *)((uint8_t *)ip + sizeof(net_ipv4_header_t));
    const net_device_t *dev = net_get_device_const(index);
    uint32_t udp_size = sizeof(net_udp_header_t) + payload_size;
    uint32_t ip_size = sizeof(net_ipv4_header_t) + udp_size;
    uint32_t frame_size = sizeof(net_eth_header_t) + ip_size;
    uint32_t sum = 0;

    if (!dev || !dst_mac || frame_size > NET_MAX_FRAME_SIZE) {
        ++debug_info.udp_tx_errors;
        return -1;
    }

    net_zero(frame, sizeof(frame));
    net_copy(eth->dst, dst_mac, 6u);
    net_copy(eth->src, dev->mac, 6u);
    eth->type = net_htons(NET_ETH_TYPE_IPV4);

    ip->ver_ihl = 0x45u;
    ip->total_length = net_htons((uint16_t)ip_size);
    ip->id = net_htons(next_ip_id++);
    ip->flags_fragment = net_htons(0x4000u);
    ip->ttl = 64u;
    ip->proto = NET_IP_PROTO_UDP;
    ip->src = net_htonl(local_ip);
    ip->dst = net_htonl(dst_ip);
    ip->checksum = net_htons(net_checksum(ip, sizeof(net_ipv4_header_t)));

    udp->src_port = net_htons(src_port);
    udp->dst_port = net_htons(dst_port);
    udp->length = net_htons((uint16_t)udp_size);
    udp->checksum = 0;
    if (payload_size != 0) {
        net_copy((uint8_t *)udp + sizeof(net_udp_header_t), payload, payload_size);
    }

    sum += (uint16_t)(local_ip >> 16);
    sum += (uint16_t)(local_ip & 0xFFFFu);
    sum += (uint16_t)(dst_ip >> 16);
    sum += (uint16_t)(dst_ip & 0xFFFFu);
    sum += NET_IP_PROTO_UDP;
    sum += udp_size;
    sum = net_checksum_add(sum, udp, udp_size);
    udp->checksum = net_htons(net_checksum_finish(sum));

    if (frame_size < NET_MIN_FRAME_SIZE) {
        frame_size = NET_MIN_FRAME_SIZE;
    }

    if (net_send_frame(index, frame, frame_size) != 0) {
        ++debug_info.udp_tx_errors;
        return -1;
    }

    return 0;
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

    if (ctx->out_size + 1u >= ctx->out_capacity) {
        ctx->full = 1;
        return;
    }
    ctx->out[ctx->out_size++] = ch;
    ctx->out[ctx->out_size] = '\0';
}

static void net_service_background(void) {
    statusbar_update_if_due();
}

static void net_dhcp_add_option(uint8_t *options, uint32_t *pos, uint8_t code, const void *data, uint8_t size) {
    if (*pos + 2u + size >= 312u) {
        return;
    }
    options[(*pos)++] = code;
    options[(*pos)++] = size;
    net_copy(&options[*pos], data, size);
    *pos += size;
}

static int net_send_dhcp(uint32_t index,
                         uint8_t message_type,
                         uint32_t xid,
                         uint32_t requested_ip,
                         uint32_t server_ip) {
    static const uint8_t broadcast_mac[6] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
    static const uint8_t parameter_request[] = { 1u, 3u, 6u };
    net_dhcp_packet_t packet;
    const net_device_t *dev = net_get_device_const(index);
    uint32_t pos = 0;
    uint8_t type = message_type;
    uint8_t client_id[7];
    uint32_t network_value;

    if (!dev) {
        return -1;
    }

    net_zero(&packet, sizeof(packet));
    packet.op = 1u;
    packet.htype = 1u;
    packet.hlen = NET_MAC_SIZE;
    packet.xid = net_htonl(xid);
    packet.flags = net_htons(0x8000u);
    packet.magic = net_htonl(0x63825363u);
    net_copy(packet.chaddr, dev->mac, NET_MAC_SIZE);

    client_id[0] = 1u;
    net_copy(&client_id[1], dev->mac, NET_MAC_SIZE);
    net_dhcp_add_option(packet.options, &pos, 53u, &type, 1u);
    net_dhcp_add_option(packet.options, &pos, 61u, client_id, sizeof(client_id));
    net_dhcp_add_option(packet.options, &pos, 55u, parameter_request, sizeof(parameter_request));
    if (requested_ip != 0) {
        network_value = net_htonl(requested_ip);
        net_dhcp_add_option(packet.options, &pos, 50u, &network_value, 4u);
    }
    if (server_ip != 0) {
        network_value = net_htonl(server_ip);
        net_dhcp_add_option(packet.options, &pos, 54u, &network_value, 4u);
    }
    if (pos < sizeof(packet.options)) {
        packet.options[pos++] = 255u;
    }

    ++debug_info.dhcp_tx;
    return net_send_udp(index,
                        broadcast_mac,
                        0xFFFFFFFFu,
                        NET_DHCP_CLIENT_PORT,
                        NET_DHCP_SERVER_PORT,
                        &packet,
                        sizeof(packet));
}

static int net_send_arp_reply(uint32_t index, const uint8_t target_mac[6], uint32_t target_ip) {
    uint8_t frame[NET_MIN_FRAME_SIZE];
    net_eth_header_t *eth = (net_eth_header_t *)frame;
    net_arp_packet_t *arp = (net_arp_packet_t *)(frame + sizeof(net_eth_header_t));
    const net_device_t *dev = net_get_device_const(index);

    if (!dev) {
        return -1;
    }

    net_zero(frame, sizeof(frame));
    net_copy(eth->dst, target_mac, 6u);
    net_copy(eth->src, dev->mac, 6u);
    eth->type = net_htons(NET_ETH_TYPE_ARP);
    arp->htype = net_htons(1u);
    arp->ptype = net_htons(NET_ETH_TYPE_IPV4);
    arp->hlen = 6u;
    arp->plen = 4u;
    arp->oper = net_htons(NET_ARP_OP_REPLY);
    net_copy(arp->sha, dev->mac, 6u);
    arp->spa = net_htonl(local_ip);
    net_copy(arp->tha, target_mac, 6u);
    arp->tpa = net_htonl(target_ip);

    return net_send_frame(index, frame, sizeof(frame));
}

static void net_handle_arp(uint32_t index, const uint8_t *payload, uint32_t size) {
    const net_arp_packet_t *arp;
    uint32_t sender_ip;
    uint32_t target_ip;
    uint16_t op;

    if (size < sizeof(net_arp_packet_t)) {
        return;
    }

    arp = (const net_arp_packet_t *)payload;
    if (net_ntohs(arp->htype) != 1u ||
        net_ntohs(arp->ptype) != NET_ETH_TYPE_IPV4 ||
        arp->hlen != 6u ||
        arp->plen != 4u) {
        return;
    }

    sender_ip = net_ntohl(arp->spa);
    target_ip = net_ntohl(arp->tpa);
    op = net_ntohs(arp->oper);

    debug_info.last_arp_op = op;
    debug_info.last_arp_sender_ip = sender_ip;
    debug_info.last_arp_target_ip = target_ip;
    if (op == NET_ARP_OP_REPLY) {
        ++debug_info.arp_replies;
    } else if (op == NET_ARP_OP_REQUEST && target_ip == local_ip) {
        (void)net_send_arp_reply(index, arp->sha, sender_ip);
    }

    if (arp_wait.active &&
        arp_wait.device == index &&
        arp_wait.wanted_ip == sender_ip &&
        op == NET_ARP_OP_REPLY) {
        net_copy(arp_wait.mac, arp->sha, 6u);
        arp_wait.found = 1;
    } else if (arp_wait.active && op == NET_ARP_OP_REPLY) {
        ++debug_info.arp_mismatches;
    }
}

static void net_handle_tcp(uint32_t index,
                           const net_ipv4_header_t *ip,
                           uint32_t ip_header_size,
                           uint32_t ip_total_size) {
    const net_tcp_header_t *tcp;
    const uint8_t *payload;
    uint32_t tcp_header_size;
    uint32_t payload_size;
    uint32_t seq;
    uint32_t ack;
    uint32_t src_ip;

    if (ip_total_size < ip_header_size + sizeof(net_tcp_header_t)) {
        return;
    }

    tcp = (const net_tcp_header_t *)((const uint8_t *)ip + ip_header_size);
    tcp_header_size = (uint32_t)(tcp->data_offset >> 4) * 4u;
    if (tcp_header_size < sizeof(net_tcp_header_t) ||
        ip_total_size < ip_header_size + tcp_header_size) {
        return;
    }

    src_ip = net_ntohl(ip->src);
    seq = net_ntohl(tcp->seq);
    ack = net_ntohl(tcp->ack);
    payload = (const uint8_t *)tcp + tcp_header_size;
    payload_size = ip_total_size - ip_header_size - tcp_header_size;

    if (tcp_stream.active &&
        tcp_stream.device == index &&
        src_ip == tcp_stream.remote_ip &&
        net_ntohs(tcp->src_port) == tcp_stream.remote_port &&
        net_ntohs(tcp->dst_port) == tcp_stream.local_port) {
        if (tcp->flags & 0x04u) {
            tcp_stream.reset = 1;
            return;
        }

        if ((tcp->flags & 0x10u) != 0 && ack > tcp_stream.tx_acked) {
            tcp_stream.tx_acked = ack;
        }

        if (!tcp_stream.connected && (tcp->flags & 0x12u) == 0x12u) {
            tcp_stream.ack = seq + 1u;
            tcp_stream.tx_acked = ack;
            tcp_stream.connected = 1;
            (void)net_send_tcp(index,
                               tcp_stream.remote_mac,
                               tcp_stream.remote_ip,
                               tcp_stream.local_port,
                               tcp_stream.remote_port,
                               tcp_stream.seq,
                               tcp_stream.ack,
                               0x10u,
                               0,
                               0);
            return;
        }

        if (payload_size != 0 && seq == tcp_stream.ack) {
            uint32_t room = tcp_stream.rx_capacity - tcp_stream.rx_size;
            uint32_t copy_size = payload_size < room ? payload_size : room;

            if (copy_size != 0) {
                net_copy(tcp_stream.rx_buffer + tcp_stream.rx_size, payload, copy_size);
                tcp_stream.rx_size += copy_size;
            }
            ++debug_info.tcp_stream_rx;
            tcp_stream.ack += payload_size;
            (void)net_send_tcp(index,
                               tcp_stream.remote_mac,
                               tcp_stream.remote_ip,
                               tcp_stream.local_port,
                               tcp_stream.remote_port,
                               tcp_stream.seq,
                               tcp_stream.ack,
                               0x10u,
                               0,
                               0);
        }

        if (tcp->flags & 0x01u) {
            tcp_stream.ack = seq + payload_size + 1u;
            tcp_stream.closed = 1;
            (void)net_send_tcp(index,
                               tcp_stream.remote_mac,
                               tcp_stream.remote_ip,
                               tcp_stream.local_port,
                               tcp_stream.remote_port,
                               tcp_stream.seq,
                               tcp_stream.ack,
                               0x11u,
                               0,
                               0);
            ++tcp_stream.seq;
        }
        return;
    }

    if (!tcp_get.active || tcp_get.device != index) {
        return;
    }

    if (src_ip != tcp_get.remote_ip ||
        net_ntohs(tcp->src_port) != tcp_get.remote_port ||
        net_ntohs(tcp->dst_port) != tcp_get.local_port) {
        return;
    }

    if (tcp->flags & 0x04u) {
        tcp_get.reset = 1;
        return;
    }

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

}

static void net_handle_dhcp(uint32_t index, const uint8_t *payload, uint32_t size) {
    const net_dhcp_packet_t *packet;
    uint32_t options_offset = 0;
    uint8_t message_type = 0;
    uint32_t server_ip = 0;
    uint32_t subnet_mask = 0;
    uint32_t router = 0;
    uint32_t dns = 0;

    if (!dhcp_wait.active ||
        dhcp_wait.device != index ||
        size < 240u ||
        size < sizeof(net_dhcp_packet_t) - sizeof(((net_dhcp_packet_t *)0)->options)) {
        return;
    }

    packet = (const net_dhcp_packet_t *)payload;
    if (packet->op != 2u ||
        packet->htype != 1u ||
        packet->hlen != NET_MAC_SIZE ||
        net_ntohl(packet->xid) != dhcp_wait.xid ||
        net_ntohl(packet->magic) != 0x63825363u) {
        return;
    }

    options_offset = 240u;
    while (options_offset < size) {
        uint8_t code = payload[options_offset++];
        uint8_t len;

        if (code == 0u) {
            continue;
        }
        if (code == 255u || options_offset >= size) {
            break;
        }
        len = payload[options_offset++];
        if (options_offset + len > size) {
            break;
        }

        if (code == 53u && len >= 1u) {
            message_type = payload[options_offset];
        } else if (code == 54u && len >= 4u) {
            server_ip = ((uint32_t)payload[options_offset] << 24) |
                        ((uint32_t)payload[options_offset + 1u] << 16) |
                        ((uint32_t)payload[options_offset + 2u] << 8) |
                        payload[options_offset + 3u];
        } else if (code == 1u && len >= 4u) {
            subnet_mask = ((uint32_t)payload[options_offset] << 24) |
                          ((uint32_t)payload[options_offset + 1u] << 16) |
                          ((uint32_t)payload[options_offset + 2u] << 8) |
                          payload[options_offset + 3u];
        } else if (code == 3u && len >= 4u) {
            router = ((uint32_t)payload[options_offset] << 24) |
                     ((uint32_t)payload[options_offset + 1u] << 16) |
                     ((uint32_t)payload[options_offset + 2u] << 8) |
                     payload[options_offset + 3u];
        } else if (code == 6u && len >= 4u) {
            dns = ((uint32_t)payload[options_offset] << 24) |
                  ((uint32_t)payload[options_offset + 1u] << 16) |
                  ((uint32_t)payload[options_offset + 2u] << 8) |
                  payload[options_offset + 3u];
        }
        options_offset += len;
    }

    if (message_type != dhcp_wait.wanted_type) {
        return;
    }

    ++debug_info.dhcp_rx;
    dhcp_wait.offered_ip = net_ntohl(packet->yiaddr);
    dhcp_wait.server_ip = server_ip;
    dhcp_wait.subnet_mask = subnet_mask;
    dhcp_wait.router = router;
    dhcp_wait.dns = dns;
    dhcp_wait.found = 1;
}

static uint32_t net_dns_skip_name(const uint8_t *payload, uint32_t size, uint32_t offset) {
    while (offset < size) {
        uint8_t len = payload[offset++];
        if (len == 0u) {
            return offset;
        }
        if ((len & 0xC0u) == 0xC0u) {
            return offset < size ? offset + 1u : size;
        }
        offset += len;
    }
    return size;
}

static void net_handle_dns(uint32_t index, const uint8_t *payload, uint32_t size) {
    uint16_t answer_count;
    uint32_t offset;

    if (!dns_wait.active || dns_wait.device != index || size < 12u) {
        return;
    }
    if ((((uint16_t)payload[0] << 8) | payload[1]) != dns_wait.id ||
        (payload[2] & 0x80u) == 0 ||
        (payload[3] & 0x0Fu) != 0) {
        return;
    }

    answer_count = ((uint16_t)payload[6] << 8) | payload[7];
    offset = 12u;
    offset = net_dns_skip_name(payload, size, offset);
    if (offset + 4u > size) {
        return;
    }
    offset += 4u;

    for (uint32_t i = 0; i < answer_count && offset < size; ++i) {
        uint16_t type;
        uint16_t class_code;
        uint16_t rdlength;

        offset = net_dns_skip_name(payload, size, offset);
        if (offset + 10u > size) {
            return;
        }
        type = ((uint16_t)payload[offset] << 8) | payload[offset + 1u];
        class_code = ((uint16_t)payload[offset + 2u] << 8) | payload[offset + 3u];
        rdlength = ((uint16_t)payload[offset + 8u] << 8) | payload[offset + 9u];
        offset += 10u;
        if (offset + rdlength > size) {
            return;
        }
        if (type == 1u && class_code == 1u && rdlength == 4u) {
            dns_wait.ip = ((uint32_t)payload[offset] << 24) |
                          ((uint32_t)payload[offset + 1u] << 16) |
                          ((uint32_t)payload[offset + 2u] << 8) |
                          payload[offset + 3u];
            dns_wait.found = 1;
            ++debug_info.dns_rx;
            return;
        }
        offset += rdlength;
    }
}

static void net_handle_udp(uint32_t index,
                           const net_ipv4_header_t *ip,
                           uint32_t ip_header_size,
                           uint32_t ip_total_size) {
    const net_udp_header_t *udp;
    const uint8_t *payload;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t udp_length;

    if (ip_total_size < ip_header_size + sizeof(net_udp_header_t)) {
        return;
    }

    udp = (const net_udp_header_t *)((const uint8_t *)ip + ip_header_size);
    src_port = net_ntohs(udp->src_port);
    dst_port = net_ntohs(udp->dst_port);
    udp_length = net_ntohs(udp->length);
    if (udp_length < sizeof(net_udp_header_t) ||
        ip_total_size < ip_header_size + udp_length) {
        return;
    }
    payload = (const uint8_t *)udp + sizeof(net_udp_header_t);

    ++debug_info.rx_udp;
    if (src_port == NET_DHCP_SERVER_PORT && dst_port == NET_DHCP_CLIENT_PORT) {
        net_handle_dhcp(index, payload, udp_length - sizeof(net_udp_header_t));
    } else if (src_port == NET_DNS_PORT && dns_wait.active && dst_port == dns_wait.local_port) {
        net_handle_dns(index, payload, udp_length - sizeof(net_udp_header_t));
    }
}

static void net_handle_ipv4(uint32_t index, const uint8_t *payload, uint32_t size) {
    const net_ipv4_header_t *ip;
    uint32_t ip_header_size;
    uint32_t ip_total_size;

    if (size < sizeof(net_ipv4_header_t)) {
        return;
    }

    ip = (const net_ipv4_header_t *)payload;
    ip_header_size = (uint32_t)(ip->ver_ihl & 0x0Fu) * 4u;
    ip_total_size = net_ntohs(ip->total_length);
    if ((ip->ver_ihl >> 4) != 4u ||
        ip_header_size < sizeof(net_ipv4_header_t) ||
        ip_total_size < ip_header_size ||
        size < ip_total_size) {
        return;
    }

    if (net_ntohl(ip->dst) != local_ip &&
        net_ntohl(ip->dst) != 0xFFFFFFFFu &&
        !(local_ip == 0 && ip->proto == NET_IP_PROTO_UDP)) {
        return;
    }

    if (ip->proto == NET_IP_PROTO_TCP) {
        net_handle_tcp(index, ip, ip_header_size, ip_total_size);
    } else if (ip->proto == NET_IP_PROTO_UDP) {
        net_handle_udp(index, ip, ip_header_size, ip_total_size);
    }
}

void net_receive_frame(uint32_t index, const void *data, uint32_t size) {
    const uint8_t *frame = (const uint8_t *)data;
    const net_eth_header_t *eth;
    const uint8_t *payload;
    uint16_t type;
    uint32_t payload_offset = sizeof(net_eth_header_t);

    if (index >= device_count || data == 0 || size < sizeof(net_eth_header_t)) {
        return;
    }

    eth = (const net_eth_header_t *)frame;
    type = net_ntohs(eth->type);
    debug_info.last_eth_type = type;
    debug_info.last_inner_eth_type = type;
    if ((type == NET_ETH_TYPE_VLAN || type == NET_ETH_TYPE_QINQ) && size >= sizeof(net_eth_header_t) + 4u) {
        type = ((uint16_t)frame[16] << 8) | frame[17];
        payload_offset += 4u;
        debug_info.last_inner_eth_type = type;
    }

    if (size < payload_offset) {
        return;
    }
    payload = frame + payload_offset;
    if (type == NET_ETH_TYPE_ARP) {
        ++debug_info.rx_arp;
        net_handle_arp(index, payload, size - payload_offset);
    } else if (type == NET_ETH_TYPE_IPV4) {
        ++debug_info.rx_ipv4;
        net_handle_ipv4(index, payload, size - payload_offset);
    } else {
        ++debug_info.rx_other;
    }
}

int net_arp_probe(uint32_t index, uint32_t ip, uint8_t mac[NET_MAC_SIZE]) {
    if (!mac || index >= device_count) {
        return -1;
    }

    return net_arp_resolve(index, ip, mac);
}

void net_debug_info(net_debug_info_t *out) {
    if (!out) {
        return;
    }

    *out = debug_info;
}

static int net_wait_for_tcp_connected(uint32_t index) {
    unsigned long long start = timer_ticks();

    while (timer_ticks() - start < NET_TIMEOUT_TICKS) {
        (void)net_poll_device(index);
        net_service_background();
        if (tcp_get.connected) {
            return 0;
        }
        if (tcp_get.reset) {
            return -1;
        }
    }

    return -1;
}

static int net_wait_for_tcp_stream_connected(uint32_t index) {
    unsigned long long start = timer_ticks();

    while (timer_ticks() - start < NET_TIMEOUT_TICKS) {
        (void)net_poll_device(index);
        net_service_background();
        if (tcp_stream.connected) {
            return 0;
        }
        if (tcp_stream.reset) {
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
        net_service_background();
        if (tcp_get.closed) {
            return 0;
        }
        if (tcp_get.full) {
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

int net_tcp_stream_connect(uint32_t index,
                           uint32_t ip,
                           uint16_t port,
                           uint8_t *rx_buffer,
                           uint32_t rx_capacity) {
    uint8_t mac[6];
    uint32_t arp_ip;

    if (index >= device_count || ip == 0 || port == 0 || rx_buffer == 0 || rx_capacity == 0) {
        return -1;
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

    net_zero(&tcp_stream, sizeof(tcp_stream));
    tcp_stream.active = 1;
    tcp_stream.device = index;
    tcp_stream.remote_ip = ip;
    tcp_stream.remote_port = port;
    tcp_stream.local_port = next_local_port++;
    tcp_stream.seq = 0x20000000u + (uint32_t)timer_ticks();
    tcp_stream.tx_acked = tcp_stream.seq;
    net_copy(tcp_stream.remote_mac, mac, 6u);
    tcp_stream.rx_buffer = rx_buffer;
    tcp_stream.rx_capacity = rx_capacity;

    if (net_send_tcp(index, mac, ip, tcp_stream.local_port, port, tcp_stream.seq, 0, 0x02u, 0, 0) != 0) {
        tcp_stream.active = 0;
        return -4;
    }
    ++tcp_stream.seq;

    if (net_wait_for_tcp_stream_connected(index) != 0) {
        tcp_stream.active = 0;
        return -5;
    }

    return 0;
}

int net_tcp_stream_send(const void *data, uint32_t size) {
    const uint8_t *p = (const uint8_t *)data;

    if (!tcp_stream.active || !tcp_stream.connected || data == 0) {
        return -1;
    }

    while (size != 0) {
        uint32_t chunk = size;
        uint32_t send_seq;
        uint32_t target_ack;
        unsigned long long start;
        unsigned long long last_send;

        if (chunk > 1200u) {
            chunk = 1200u;
        }
        send_seq = tcp_stream.seq;
        target_ack = send_seq + chunk;
        start = timer_ticks();
        last_send = 0;

        while (tcp_stream.tx_acked < target_ack) {
            if (last_send == 0 || timer_ticks() - last_send >= 60ull) {
                if (net_send_tcp(tcp_stream.device,
                                 tcp_stream.remote_mac,
                                 tcp_stream.remote_ip,
                                 tcp_stream.local_port,
                                 tcp_stream.remote_port,
                                 send_seq,
                                 tcp_stream.ack,
                                 0x18u,
                                 p,
                                 chunk) != 0) {
                    return -2;
                }
                if (tcp_stream.seq < target_ack) {
                    tcp_stream.seq = target_ack;
                }
                ++debug_info.tcp_stream_tx;
                if (last_send != 0) {
                    ++debug_info.tcp_stream_retx;
                }
                last_send = timer_ticks();
            }
            (void)net_poll_device(tcp_stream.device);
            net_service_background();
            if (tcp_stream.reset) {
                return -3;
            }
            if (timer_ticks() - start >= NET_TIMEOUT_TICKS) {
                return -4;
            }
        }
        p += chunk;
        size -= chunk;
    }

    return 0;
}

void net_tls_debug_set(uint32_t state,
                       uint32_t error,
                       uint32_t last_got,
                       uint32_t body_size) {
    debug_info.tls_last_state = state;
    debug_info.tls_last_error = error;
    debug_info.tls_last_got = last_got;
    debug_info.tls_last_body_size = body_size;
}

int net_tcp_stream_recv(void *out, uint32_t capacity, uint32_t timeout_ticks) {
    uint8_t *dst = (uint8_t *)out;
    unsigned long long start;
    uint32_t copy_size;

    if (!tcp_stream.active || out == 0 || capacity == 0) {
        return -1;
    }

    start = timer_ticks();
    while (tcp_stream.rx_size == 0) {
        (void)net_poll_device(tcp_stream.device);
        net_service_background();
        if (tcp_stream.reset) {
            return -2;
        }
        if (tcp_stream.closed) {
            return 0;
        }
        if (timer_ticks() - start >= timeout_ticks) {
            return 0;
        }
    }

    copy_size = tcp_stream.rx_size < capacity ? tcp_stream.rx_size : capacity;
    net_copy(dst, tcp_stream.rx_buffer, copy_size);
    if (copy_size < tcp_stream.rx_size) {
        for (uint32_t i = copy_size; i < tcp_stream.rx_size; ++i) {
            tcp_stream.rx_buffer[i - copy_size] = tcp_stream.rx_buffer[i];
        }
    }
    tcp_stream.rx_size -= copy_size;
    return (int)copy_size;
}

void net_tcp_stream_close(void) {
    if (tcp_stream.active && tcp_stream.connected && !tcp_stream.closed) {
        (void)net_send_tcp(tcp_stream.device,
                           tcp_stream.remote_mac,
                           tcp_stream.remote_ip,
                           tcp_stream.local_port,
                           tcp_stream.remote_port,
                           tcp_stream.seq,
                           tcp_stream.ack,
                           0x11u,
                           0,
                           0);
        ++tcp_stream.seq;
    }
    tcp_stream.active = 0;
}

static int net_build_http_request(const char *path,
                                  uint32_t ip,
                                  const char *host,
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
    if (host && *host) {
        APPEND_TEXT(host);
    } else {
        APPEND_DEC(parts[0]); APPEND_CH('.');
        APPEND_DEC(parts[1]); APPEND_CH('.');
        APPEND_DEC(parts[2]); APPEND_CH('.');
        APPEND_DEC(parts[3]);
    }
    APPEND_TEXT("\r\nConnection: close\r\n\r\n");

#undef APPEND_DEC
#undef APPEND_TEXT
#undef APPEND_CH

    return 0;
}

static int net_wait_for_dhcp(uint32_t index) {
    unsigned long long start = timer_ticks();

    while (timer_ticks() - start < NET_TIMEOUT_TICKS) {
        (void)net_poll_device(index);
        net_service_background();
        if (dhcp_wait.found) {
            return 0;
        }
    }

    return -1;
}

int net_dhcp_configure(uint32_t index) {
    uint32_t old_ip;
    uint32_t xid;
    uint32_t offered_ip;
    uint32_t server_ip;

    if (index >= device_count) {
        return -1;
    }

    old_ip = local_ip;
    local_ip = 0;
    xid = 0x44484350u ^ (uint32_t)timer_ticks();
    net_zero(&dhcp_wait, sizeof(dhcp_wait));
    dhcp_wait.active = 1;
    dhcp_wait.device = index;
    dhcp_wait.xid = xid;
    dhcp_wait.wanted_type = 2u;

    if (net_send_dhcp(index, 1u, xid, 0, 0) != 0 || net_wait_for_dhcp(index) != 0) {
        dhcp_wait.active = 0;
        local_ip = old_ip;
        return -2;
    }

    offered_ip = dhcp_wait.offered_ip;
    server_ip = dhcp_wait.server_ip;
    net_zero(&dhcp_wait, sizeof(dhcp_wait));
    dhcp_wait.active = 1;
    dhcp_wait.device = index;
    dhcp_wait.xid = xid;
    dhcp_wait.wanted_type = 5u;

    if (net_send_dhcp(index, 3u, xid, offered_ip, server_ip) != 0 || net_wait_for_dhcp(index) != 0) {
        dhcp_wait.active = 0;
        local_ip = old_ip;
        return -3;
    }

    local_ip = dhcp_wait.offered_ip;
    if (dhcp_wait.subnet_mask != 0) {
        local_netmask = dhcp_wait.subnet_mask;
    }
    if (dhcp_wait.router != 0) {
        local_gateway = dhcp_wait.router;
    }
    if (dhcp_wait.dns != 0) {
        local_dns = dhcp_wait.dns;
    }
    dhcp_wait.active = 0;
    return 0;
}

static int net_hostname_is_ipv4(const char *name, uint32_t *out) {
    const char *s = name;

    if (net_parse_ipv4(&s, out) != 0) {
        return 0;
    }
    return *s == '\0';
}

static int net_build_dns_query(const char *name, uint16_t id, uint8_t *packet, uint32_t capacity, uint32_t *out_size) {
    uint32_t pos = 12u;
    const char *label = name;
    const char *s = name;

    if (!name || !*name || capacity < 32u) {
        return -1;
    }

    net_zero(packet, capacity);
    packet[0] = (uint8_t)(id >> 8);
    packet[1] = (uint8_t)id;
    packet[2] = 0x01u;
    packet[5] = 0x01u;

    for (;;) {
        if (*s == '.' || *s == '\0') {
            uint32_t len = (uint32_t)(s - label);
            if (len == 0u || len > 63u || pos + 1u + len + 5u >= capacity) {
                return -1;
            }
            packet[pos++] = (uint8_t)len;
            net_copy(&packet[pos], label, len);
            pos += len;
            if (*s == '\0') {
                break;
            }
            label = s + 1;
        }
        ++s;
    }

    packet[pos++] = 0;
    packet[pos++] = 0;
    packet[pos++] = 1u;
    packet[pos++] = 0;
    packet[pos++] = 1u;
    *out_size = pos;
    return 0;
}

int net_dns_resolve(uint32_t index, const char *name, uint32_t *out_ip) {
    uint8_t query[256];
    uint32_t query_size = 0;
    uint8_t mac[6];
    uint32_t arp_ip;
    uint16_t local_port;
    uint16_t id;
    unsigned long long start;

    if (!name || !out_ip || index >= device_count) {
        return -1;
    }
    if (net_hostname_is_ipv4(name, out_ip)) {
        return 0;
    }
    if (local_dns == 0) {
        return -2;
    }

    arp_ip = ((local_dns & local_netmask) == (local_ip & local_netmask)) ? local_dns : local_gateway;
    if (arp_ip == 0 || net_arp_resolve(index, arp_ip, mac) != 0) {
        return -3;
    }

    local_port = next_local_port++;
    id = (uint16_t)(0xD000u ^ (uint16_t)timer_ticks() ^ local_port);
    if (net_build_dns_query(name, id, query, sizeof(query), &query_size) != 0) {
        return -4;
    }

    net_zero(&dns_wait, sizeof(dns_wait));
    dns_wait.active = 1;
    dns_wait.device = index;
    dns_wait.id = id;
    dns_wait.local_port = local_port;
    ++debug_info.dns_tx;

    if (net_send_udp(index, mac, local_dns, local_port, NET_DNS_PORT, query, query_size) != 0) {
        dns_wait.active = 0;
        return -5;
    }

    start = timer_ticks();
    while (timer_ticks() - start < NET_TIMEOUT_TICKS) {
        (void)net_poll_device(index);
        net_service_background();
        if (dns_wait.found) {
            *out_ip = dns_wait.ip;
            dns_wait.active = 0;
            return 0;
        }
    }

    dns_wait.active = 0;
    return -6;
}

int net_http_get(uint32_t index,
                 const char *url,
                 char *out,
                 uint32_t out_capacity,
                 uint32_t *out_size) {
    uint32_t ip = 0;
    uint16_t port = 80u;
    const char *path = 0;
    char host[128];
    uint8_t mac[6];
    uint32_t arp_ip = 0;
    char request[512];
    uint32_t request_size;
    int is_https = 0;

    if (out_size) {
        *out_size = 0;
    }
    if (!url || !out || out_capacity == 0 || index >= device_count) {
        return -1;
    }
    out[0] = '\0';

    if (net_parse_url(url, &ip, &port, host, sizeof(host), &path, &is_https) != 0) {
        return -2;
    }
    if (ip == 0 && net_dns_resolve(index, host, &ip) != 0) {
        return -8;
    }
    if (is_https) {
        return net_tls_http_get(index, ip, port, host, path, out, out_capacity, out_size);
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

    if (net_build_http_request(path, ip, host, request, sizeof(request)) != 0) {
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
