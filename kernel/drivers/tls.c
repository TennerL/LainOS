#include <stdint.h>
#include "bearssl.h"
#include "kernel.h"
#include "kmem.h"
#include "net.h"
#include "tls_roots.h"

#define TLS_IOBUF_SIZE BR_SSL_BUFSIZE_BIDI
#define TLS_TCP_RX_SIZE 32768u
#define TLS_TIMEOUT_TICKS 7500ull

typedef struct {
    int header_done;
    char header_tail[4];
    uint32_t header_tail_count;
    char header[1024];
    uint32_t header_size;
    int header_truncated;
    char *out;
    uint32_t out_capacity;
    uint32_t out_size;
    uint32_t body_bytes;
    uint32_t content_length;
    int content_length_known;
    int full;
    int done;
} tls_http_body_t;

static uint32_t tls_strlen(const char *s) {
    uint32_t len = 0;
    while (s && s[len]) {
        ++len;
    }
    return len;
}

static void tls_copy(void *dst, const void *src, uint32_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (uint32_t i = 0; i < size; ++i) {
        d[i] = s[i];
    }
}

static void tls_zero(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t *)ptr;

    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static int tls_header_match_at(const char *header, uint32_t pos, const char *needle) {
    uint32_t i = 0;

    while (needle[i]) {
        char a = header[pos + i];
        char b = needle[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (a != b) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static void tls_parse_headers(tls_http_body_t *body) {
    static const char name[] = "content-length:";

    for (uint32_t i = 0; i + sizeof(name) - 1u < body->header_size; ++i) {
        uint32_t pos;
        uint32_t value = 0;
        int saw_digit = 0;

        if (i != 0 && body->header[i - 1u] != '\n') {
            continue;
        }
        if (!tls_header_match_at(body->header, i, name)) {
            continue;
        }

        pos = i + (uint32_t)sizeof(name) - 1u;
        while (pos < body->header_size &&
               (body->header[pos] == ' ' || body->header[pos] == '\t')) {
            ++pos;
        }
        while (pos < body->header_size &&
               body->header[pos] >= '0' &&
               body->header[pos] <= '9') {
            saw_digit = 1;
            value = value * 10u + (uint32_t)(body->header[pos] - '0');
            ++pos;
        }
        if (saw_digit) {
            body->content_length = value;
            body->content_length_known = 1;
        }
        return;
    }
}

static void tls_http_body_byte(tls_http_body_t *body, char ch) {
    if (!body->header_done) {
        if (body->header_size + 1u < sizeof(body->header)) {
            body->header[body->header_size++] = ch;
            body->header[body->header_size] = '\0';
        } else {
            body->header_truncated = 1;
        }
        if (body->header_tail_count < sizeof(body->header_tail)) {
            body->header_tail[body->header_tail_count++] = ch;
        } else {
            body->header_tail[0] = body->header_tail[1];
            body->header_tail[1] = body->header_tail[2];
            body->header_tail[2] = body->header_tail[3];
            body->header_tail[3] = ch;
        }

        if (body->header_tail_count == 4u &&
            body->header_tail[0] == '\r' &&
            body->header_tail[1] == '\n' &&
            body->header_tail[2] == '\r' &&
            body->header_tail[3] == '\n') {
            body->header_done = 1;
            tls_parse_headers(body);
            if (body->content_length_known && body->content_length == 0) {
                body->done = 1;
            }
        }
        return;
    }

    ++body->body_bytes;
    if (body->out_size + 1u >= body->out_capacity) {
        body->full = 1;
        body->done = 1;
        return;
    }
    body->out[body->out_size++] = ch;
    body->out[body->out_size] = '\0';
    if (body->content_length_known && body->body_bytes >= body->content_length) {
        body->done = 1;
    }
}

static int tls_build_http_request(const char *path,
                                  const char *host,
                                  char *request,
                                  uint32_t request_size) {
    uint32_t pos = 0;

    if (!path || !host || !request || request_size < 64u) {
        return -1;
    }

#define APPEND_CH(ch) do { if (pos + 1u >= request_size) return -1; request[pos++] = (ch); request[pos] = '\0'; } while (0)
#define APPEND_TEXT(text) do { const char *ap = (text); while (*ap) { APPEND_CH(*ap++); } } while (0)

    APPEND_TEXT("GET ");
    APPEND_TEXT(path);
    APPEND_TEXT(" HTTP/1.0\r\nHost: ");
    APPEND_TEXT(host);
    APPEND_TEXT("\r\nAccept: text/html,image/*,*/*\r\nAccept-Encoding: gzip, deflate\r\nUser-Agent: LainOS-ZBrowser/0.1\r\nConnection: close\r\n\r\n");

#undef APPEND_TEXT
#undef APPEND_CH

    return 0;
}

int net_tls_http_get(uint32_t index,
                     uint32_t ip,
                     uint16_t port,
                     const char *host,
                     const char *path,
                     char *out,
                     uint32_t out_capacity,
                     uint32_t *out_size,
                     net_http_info_t *info) {
    br_ssl_client_context *cc;
    br_x509_minimal_context *xc;
    uint8_t *iobuf;
    uint8_t *rxbuf;
    char request[512];
    uint32_t request_len;
    uint32_t request_pos = 0;
    int request_flushed = 0;
    tls_http_body_t body;
    unsigned long long last_progress;
    int result = -10;
    net_http_info_t final_info;
    uint32_t decoded_size;

    if (out_size) {
        *out_size = 0;
    }
    if (info) {
        tls_zero(info, sizeof(*info));
    }
    if (!host || !path || !out || out_capacity == 0) {
        net_http_parse_info(0, 0, 0, 0, 0, -1, info);
        return -1;
    }
    out[0] = '\0';

    if (tls_build_http_request(path, host, request, sizeof(request)) != 0) {
        net_http_parse_info(0, 0, 0, 0, 0, -2, info);
        return -2;
    }
    request_len = tls_strlen(request);

    cc = (br_ssl_client_context *)kmalloc(sizeof(*cc));
    xc = (br_x509_minimal_context *)kmalloc(sizeof(*xc));
    iobuf = (uint8_t *)kmalloc(TLS_IOBUF_SIZE);
    rxbuf = (uint8_t *)kmalloc(TLS_TCP_RX_SIZE);
    if (!cc || !xc || !iobuf || !rxbuf) {
        result = -11;
        net_http_parse_info(0, 0, 0, 0, 0, result, info);
        goto cleanup;
    }

    if (net_tcp_stream_connect(index, ip, port, rxbuf, TLS_TCP_RX_SIZE) != 0) {
        result = -5;
        net_http_parse_info(0, 0, 0, 0, 0, result, info);
        goto cleanup;
    }

    br_ssl_client_init_full(cc, xc, tls_trust_anchors, tls_trust_anchors_num);
    {
        rtc_time_t now;
        if (clock_get_rtc_time(&now) != 0) {
            result = -12;
            net_http_parse_info(0, 0, 0, 0, 0, result, info);
            goto cleanup;
        }
        br_x509_minimal_set_time(xc,
                                 clock_days_since_year0(now.year, now.month, now.day),
                                 clock_seconds_since_midnight(now.hour, now.minute, now.second));
    }
    br_ssl_engine_set_buffer(&cc->eng, iobuf, TLS_IOBUF_SIZE, 1);

    {
        uint32_t seed[8];
        seed[0] = (uint32_t)timer_ticks();
        seed[1] = timer_frequency();
        seed[2] = net_ipv4_address();
        seed[3] = ip;
        seed[4] = (uint32_t)(uintptr_t)cc;
        seed[5] = (uint32_t)(uintptr_t)iobuf;
        seed[6] = (uint32_t)(uintptr_t)&seed;
        seed[7] = 0x544C5331u;
        br_ssl_engine_inject_entropy(&cc->eng, seed, sizeof(seed));
    }

    if (!br_ssl_client_reset(cc, host, 0)) {
        result = -10;
        net_http_parse_info(0, 0, 0, 0, 0, result, info);
        goto cleanup;
    }

    tls_zero(&body, sizeof(body));
    body.out = out;
    body.out_capacity = out_capacity;
    last_progress = timer_ticks();
    net_tls_debug_set(0, 0, 0, 0);

    for (;;) {
        unsigned state = br_ssl_engine_current_state(&cc->eng);
        int progressed = 0;
        int got = 0;

        net_tls_debug_set(state,
                          br_ssl_engine_last_error(&cc->eng),
                          0,
                          body.out_size);

        if (state == BR_SSL_CLOSED) {
            result = br_ssl_engine_last_error(&cc->eng) == 0 ? 0 : -10;
            break;
        }

        if ((state & BR_SSL_SENDREC) != 0) {
            size_t len = 0;
            unsigned char *buf = br_ssl_engine_sendrec_buf(&cc->eng, &len);
            if (buf != 0 && len != 0) {
                int send_status = net_tcp_stream_send(buf, (uint32_t)len);
                if (send_status != 0) {
                    net_tls_debug_set(state,
                                      (uint32_t)(0u - (uint32_t)send_status),
                                      0,
                                      body.out_size);
                    result = -4;
                    break;
                }
                br_ssl_engine_sendrec_ack(&cc->eng, len);
                progressed = 1;
            }
        } else if ((state & BR_SSL_RECVAPP) != 0) {
            size_t len = 0;
            unsigned char *buf = br_ssl_engine_recvapp_buf(&cc->eng, &len);
            if (buf != 0 && len != 0) {
                for (uint32_t i = 0; i < (uint32_t)len; ++i) {
                    tls_http_body_byte(&body, (char)buf[i]);
                }
                br_ssl_engine_recvapp_ack(&cc->eng, len);
                progressed = 1;
                if (body.done) {
                    result = 0;
                    break;
                }
            }
        } else if ((state & BR_SSL_SENDAPP) != 0 && (request_pos < request_len || !request_flushed)) {
            if (request_pos < request_len) {
                size_t len = 0;
                unsigned char *buf = br_ssl_engine_sendapp_buf(&cc->eng, &len);
                uint32_t n = request_len - request_pos;
                if (n > (uint32_t)len) {
                    n = (uint32_t)len;
                }
                if (buf != 0 && n != 0) {
                    tls_copy(buf, request + request_pos, n);
                    br_ssl_engine_sendapp_ack(&cc->eng, n);
                    request_pos += n;
                    progressed = 1;
                }
            } else if (!request_flushed) {
                br_ssl_engine_flush(&cc->eng, 0);
                request_flushed = 1;
                progressed = 1;
            }
        } else if ((state & BR_SSL_RECVREC) != 0) {
            size_t len = 0;
            unsigned char *buf = br_ssl_engine_recvrec_buf(&cc->eng, &len);
            if (len > 1200u) {
                len = 1200u;
            }
            got = net_tcp_stream_recv(buf, (uint32_t)len, 4u);
            net_tls_debug_set(state,
                              br_ssl_engine_last_error(&cc->eng),
                              got > 0 ? (uint32_t)got : 0,
                              body.out_size);
            if (got < 0) {
                result = -6;
                break;
            }
            if (got > 0) {
                br_ssl_engine_recvrec_ack(&cc->eng, (size_t)got);
                progressed = 1;
            }
        }

        if (progressed) {
            last_progress = timer_ticks();
            statusbar_update_if_due();
        } else if (timer_ticks() - last_progress > TLS_TIMEOUT_TICKS) {
            net_tls_debug_set(state,
                              br_ssl_engine_last_error(&cc->eng),
                              got > 0 ? (uint32_t)got : 0,
                              body.out_size);
            result = body.header_done ? 0 : -6;
            break;
        }
    }

    net_http_parse_info(body.header,
                        body.header_size,
                        body.out_size,
                        body.full,
                        body.header_truncated,
                        result,
                        &final_info);
    if (result == 0 && (final_info.flags & NET_HTTP_FLAG_CHUNKED) != 0) {
        if (net_http_decode_chunked(out, body.out_size, out_capacity, &decoded_size) == 0) {
            body.out_size = decoded_size;
            final_info.flags |= NET_HTTP_FLAG_DECHUNKED;
            final_info.body_size = decoded_size;
        } else {
            final_info.flags |= NET_HTTP_FLAG_CHUNK_DECODE_ERROR;
        }
    }
    if (result == 0 && (final_info.flags & (NET_HTTP_FLAG_GZIP | NET_HTTP_FLAG_DEFLATE)) != 0) {
        if (net_http_decode_content(out, body.out_size, out_capacity, final_info.flags, &decoded_size) == 0) {
            body.out_size = decoded_size;
            final_info.flags |= NET_HTTP_FLAG_DECOMPRESSED;
            final_info.body_size = decoded_size;
        } else {
            final_info.flags |= NET_HTTP_FLAG_DECOMPRESS_ERROR;
            final_info.error = -15;
            result = -15;
        }
    }
    if (result == 0 && out_size) {
        *out_size = body.out_size;
    }
    if (info) {
        *info = final_info;
    }
    if (info && result == -4) {
        info->tls_error = br_ssl_engine_last_error(&cc->eng);
        if (info->tls_error == 0) {
            net_debug_info_t debug;
            net_debug_info(&debug);
            info->tls_error = 1000u + debug.tcp_stream_last_error;
        }
    }

cleanup:
    net_tcp_stream_close();
    kfree(rxbuf);
    kfree(iobuf);
    kfree(xc);
    kfree(cc);
    return result;
}
