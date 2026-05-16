#ifndef TLS_ROOTS_H
#define TLS_ROOTS_H

#include <stddef.h>
#include "bearssl.h"

extern const br_x509_trust_anchor tls_trust_anchors[];
extern const size_t tls_trust_anchors_num;

#endif
