#ifndef HTTP_API_H
#define HTTP_API_H

#include <libwebsockets.h>
#include <stdbool.h>
#include "pty.h"

// SSE client structure
struct pss_sse {
    bool initialized;
    bool authenticated;
    char user[30];
    char address[50];
    char path[128];
    
    struct lws *wsi;
    pty_process *process;
    pty_buf_t *pty_buf;
    
    int lws_close_status;
    bool sse_headers_sent;
    
    // For SSE event ID tracking
    unsigned long event_id;
};

// Initialize SSE client list
void sse_clients_init();

// SSE callback
int callback_sse(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

// HTTP API callback
int callback_http_api(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

#endif // HTTP_API_H