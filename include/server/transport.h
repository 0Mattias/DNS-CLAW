/*
 * server/transport.h — UDP / DoT / DoH listeners
 */
#ifndef CLAW_SERVER_TRANSPORT_H
#define CLAW_SERVER_TRANSPORT_H

#include <stdatomic.h>

/* Global shutdown flag and server fd (defined in main.c) */
extern atomic_int g_running;
extern int        g_server_fd;

/* Server configuration (defined in main.c) */
typedef struct {
    char api_key[512];
    char model[128];
    char tls_cert[256];
    char tls_key[256];
    int  use_dot;
    int  use_doh;
    int  port;
} server_config_t;

extern server_config_t g_config;

/* Transport threads */
void *udp_server_thread(void *arg);
void *dot_server_thread(void *arg);
void *doh_server_thread(void *arg);

#endif /* CLAW_SERVER_TRANSPORT_H */
