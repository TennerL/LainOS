#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

#define IMAGE_OK 0
#define IMAGE_ERR_INPUT -1
#define IMAGE_ERR_FORMAT -2
#define IMAGE_ERR_UNSUPPORTED -3
#define IMAGE_ERR_OUTPUT -4
#define IMAGE_ERR_DECODE -5

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t components;
    uint32_t stride;
} image_info_t;

int image_probe(const uint8_t *data, uint32_t size, image_info_t *out_image);
int image_decode_rgb24(const uint8_t *data,
                       uint32_t size,
                       uint8_t *rgb,
                       uint32_t rgb_capacity,
                       image_info_t *out_image);
int image_decode_to_screen(const uint8_t *data,
                           uint32_t size,
                           uint32_t origin_x,
                           uint32_t origin_y,
                           image_info_t *out_image);

int jpg_probe(const uint8_t *data, uint32_t size, image_info_t *out_image);
int jpg_decode_rgb24(const uint8_t *data,
                     uint32_t size,
                     uint8_t *rgb,
                     uint32_t rgb_capacity,
                     image_info_t *out_image);
int jpg_decode_to_screen(const uint8_t *data,
                         uint32_t size,
                         uint32_t origin_x,
                         uint32_t origin_y,
                         image_info_t *out_image);
uint32_t jpg_entropy_error_detail(void);
uint32_t jpg_entropy_error_block(void);
uint32_t jpg_huffman_code_count(uint32_t slot, uint32_t length);
uint32_t jpg_huffman_value(uint32_t slot, uint32_t index);
int jpg_entropy_probe_first(const uint8_t *data, uint32_t size);
int jpg_decode_first_block_probe(const uint8_t *data, uint32_t size);
int jpg_decode_two_block_probe(const uint8_t *data, uint32_t size);
int jpg_decode_first_ac_probe(const uint8_t *data, uint32_t size);
uint32_t jpg_debug_scan_offset(void);
uint32_t jpg_debug_stream_pos(void);
uint32_t jpg_debug_bits_left(void);
uint32_t jpg_debug_zigzag(uint32_t index);
uint32_t jpg_debug_byte_at(const uint8_t *data, uint32_t pos);
int jpg_debug_first_entropy_bits(const uint8_t *data, uint32_t size);

#endif
