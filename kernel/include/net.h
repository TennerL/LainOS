#ifndef NET_H
#define NET_H

#include <stdint.h>

#define NET_MAX_DEVICES 4u
#define NET_MAC_SIZE 6u
#define NET_MIN_FRAME_SIZE 60u
#define NET_MAX_FRAME_SIZE 1518u

typedef int (*net_send_frame_t)(void *ctx, const void *data, uint32_t size);
typedef int (*net_poll_t)(void *ctx);

typedef struct {
    int present;
    char name[8];
    uint8_t mac[NET_MAC_SIZE];
    int link_up;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_errors;
    net_send_frame_t send_frame;
    net_poll_t poll;
    void *ctx;
} net_device_t;

void net_init(void);
int net_register_device(const char *name,
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
void e1000_init(void);

#endif
