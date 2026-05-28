#ifndef LAINOS_FREESTANDING_ARPA_INET_H
#define LAINOS_FREESTANDING_ARPA_INET_H

#include <stdint.h>
#include <sys/socket.h>

struct in_addr;

int inet_aton(const char *cp, struct in_addr *inp);
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
uint16_t htons(uint16_t host16);
uint16_t ntohs(uint16_t net16);
uint32_t htonl(uint32_t host32);
uint32_t ntohl(uint32_t net32);

#endif
