#include "net.h"

static net_device_t devices[NET_MAX_DEVICES];
static uint32_t device_count;

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
