/* net.h — UDP socket helpers. */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

/* Bind a UDP socket to ip:port (ip may be "0.0.0.0").  Returns fd or -1. */
int udp_bind(const char *ip, int port);

/* Create an unbound UDP socket suitable for sendto/recvfrom to a remote. -1 on err. */
int udp_socket(void);

/* Create a UDP socket connected to host:port (host may be a name or IPv4
 * literal; resolved via getaddrinfo).  Use send()/recv() afterward. -1 on err. */
int udp_connect(const char *host, int port);

/* Send to ip:port from fd. Returns bytes sent or -1. */
int udp_sendto(int fd, const void *buf, size_t len, const char *ip, int port);

/* Receive a datagram; fills src_ip (>=16 bytes) and *src_port. Returns len or -1. */
int udp_recvfrom(int fd, void *buf, size_t cap, char *src_ip, int *src_port);

#endif
