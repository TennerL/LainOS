#ifndef NET_H
#define NET_H

#include <stdint.h>

#define NET_MAX_DEVICES 4u
#define NET_MAC_SIZE 6u
#define NET_MIN_FRAME_SIZE 60u
#define NET_MAX_FRAME_SIZE 1518u
#define NET_HTTP_CONTENT_TYPE_SIZE 64u
#define NET_HTTP_LOCATION_SIZE 160u
#define NET_HTTP_FLAG_TRUNCATED 0x00000001u
#define NET_HTTP_FLAG_CONTENT_LENGTH 0x00000002u
#define NET_HTTP_FLAG_HEADER_TRUNCATED 0x00000004u

typedef int (*net_send_frame_t)(void *ctx, const void *data, uint32_t size);
typedef int (*net_poll_t)(void *ctx);

typedef struct {
    int present;
    char name[8];
    char driver[16];
    uint8_t mac[NET_MAC_SIZE];
    uint16_t vendor_id;
    uint16_t device_id;
    int link_up;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_errors;
    net_send_frame_t send_frame;
    net_poll_t poll;
    void *ctx;
} net_device_t;

typedef struct {
    uint64_t rx_arp;
    uint64_t rx_ipv4;
    uint64_t rx_udp;
    uint64_t rx_other;
    uint64_t arp_requests;
    uint64_t arp_replies;
    uint64_t arp_mismatches;
    uint64_t arp_tx_errors;
    uint64_t udp_tx_errors;
    uint64_t dhcp_tx;
    uint64_t dhcp_rx;
    uint64_t dns_tx;
    uint64_t dns_rx;
    uint16_t last_eth_type;
    uint16_t last_inner_eth_type;
    uint16_t last_arp_op;
    uint32_t last_arp_requested_ip;
    uint32_t last_arp_sender_ip;
    uint32_t last_arp_target_ip;
    uint64_t tcp_stream_rx;
    uint64_t tcp_stream_tx;
    uint64_t tcp_stream_retx;
    uint32_t tcp_stream_last_state;
    uint32_t tcp_stream_last_error;
    uint32_t tls_last_state;
    uint32_t tls_last_error;
    uint32_t tls_last_got;
    uint32_t tls_last_body_size;
} net_debug_info_t;

typedef struct {
    uint32_t present;
    uint32_t status;
    uint32_t ctrl;
    uint32_t ctrl_ext;
    uint32_t pci_command;
    uint32_t pmcsr;
    uint32_t rctl;
    uint32_t tctl;
    uint32_t rxdctl;
    uint32_t txdctl;
    uint32_t tarc0;
    uint32_t tarc1;
    uint32_t iosfpc;
    uint32_t pba;
    uint32_t gcr;
    uint32_t fwsm;
    uint32_t h2me;
    uint32_t phy_result;
    uint32_t phy_pm_ctrl;
    uint32_t phy_ulp_cfg;
    uint32_t reset_result;
    uint32_t rdh;
    uint32_t rdt;
    uint32_t tdh;
    uint32_t tdt;
    uint64_t tx_reg_base;
    uint32_t tx_reg_len;
    uint32_t rx_tail;
    uint32_t tx_tail;
    uint32_t last_tx_status;
    uint32_t last_tx_command;
    uint32_t last_tx_length;
    uint64_t last_tx_desc_addr;
    uint64_t last_tx_desc_phys;
    uint64_t last_tx_buffer_phys;
    uint8_t last_tx_desc_bytes[16];
    uint32_t first_rx_status;
    uint32_t first_rx_errors;
    uint32_t first_rx_length;
    uint64_t first_rx_desc_addr;
    uint8_t first_rx_bytes[16];
    uint32_t rx_scan_dd_count;
    uint32_t rx_scan_index;
    uint32_t rx_scan_status;
    uint32_t rx_scan_errors;
    uint32_t rx_scan_length;
    uint8_t rx_scan_bytes[16];
    uint32_t last_rx_status;
    uint32_t last_rx_errors;
    uint32_t last_rx_length;
    uint64_t last_rx_desc_addr;
    uint8_t last_rx_bytes[16];
    uint64_t rx_ring_phys;
    uint64_t tx_ring_phys;
    uint64_t first_rx_phys;
} e1000_debug_info_t;

typedef struct {
    uint32_t status_code;
    uint32_t content_length;
    uint32_t body_size;
    uint32_t header_size;
    uint32_t flags;
    int32_t error;
    uint32_t tls_error;
    char content_type[NET_HTTP_CONTENT_TYPE_SIZE];
    char location[NET_HTTP_LOCATION_SIZE];
} net_http_info_t;

void net_init(void);
int net_register_device(const char *name,
                        const char *driver,
                        uint16_t vendor_id,
                        uint16_t device_id,
                        const uint8_t mac[NET_MAC_SIZE],
                        int link_up,
                        net_send_frame_t send_frame,
                        net_poll_t poll,
                        void *ctx);
uint32_t net_device_count(void);
net_device_t *net_get_device(uint32_t index);
const net_device_t *net_get_device_const(uint32_t index);
void net_set_link(uint32_t index, int link_up);
void net_record_rx(uint32_t index);
void net_record_rx_drop(uint32_t index);
void net_record_tx(uint32_t index);
void net_record_tx_error(uint32_t index);
int net_send_frame(uint32_t index, const void *data, uint32_t size);
int net_poll_device(uint32_t index);
unsigned int net_poll_all_devices(void);
void net_receive_frame(uint32_t index, const void *data, uint32_t size);
int net_parse_ipv4_addr(const char *text, uint32_t *out);
void net_set_ipv4_config(uint32_t address, uint32_t netmask, uint32_t gateway);
uint32_t net_ipv4_address(void);
uint32_t net_ipv4_netmask(void);
uint32_t net_ipv4_gateway(void);
void net_set_dns_server(uint32_t dns);
uint32_t net_dns_server(void);
int net_dhcp_configure(uint32_t index);
int net_dns_resolve(uint32_t index, const char *name, uint32_t *out_ip);
int net_http_get(uint32_t index,
                 const char *url,
                 char *out,
                 uint32_t out_capacity,
                 uint32_t *out_size);
int net_http_get_ex(uint32_t index,
                    const char *url,
                    char *out,
                    uint32_t out_capacity,
                    uint32_t *out_size,
                    net_http_info_t *info);
int net_tls_http_get(uint32_t index,
                     uint32_t ip,
                     uint16_t port,
                     const char *host,
                     const char *path,
                     char *out,
                     uint32_t out_capacity,
                     uint32_t *out_size,
                     net_http_info_t *info);
void net_http_parse_info(const char *header,
                         uint32_t header_size,
                         uint32_t body_size,
                         int body_truncated,
                         int header_truncated,
                         int error,
                         net_http_info_t *info);
int net_tcp_stream_connect(uint32_t index,
                           uint32_t ip,
                           uint16_t port,
                           uint8_t *rx_buffer,
                           uint32_t rx_capacity);
int net_tcp_stream_send(const void *data, uint32_t size);
int net_tcp_stream_recv(void *out, uint32_t capacity, uint32_t timeout_ticks);
void net_tcp_stream_close(void);
void net_tls_debug_set(uint32_t state,
                       uint32_t error,
                       uint32_t last_got,
                       uint32_t body_size);
int net_arp_probe(uint32_t index, uint32_t ip, uint8_t mac[NET_MAC_SIZE]);
void net_debug_info(net_debug_info_t *out);
void e1000_init(void);
int e1000_debug_info(uint32_t index, e1000_debug_info_t *out);
int e1000_reset_controller(uint32_t index);
uint32_t intel_net_unsupported_count(void);
int intel_net_unsupported_info(uint32_t index,
                               uint16_t *vendor_id,
                               uint16_t *device_id,
                               const char **name,
                               const char **needed_driver);

#endif
