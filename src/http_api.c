#include <libwebsockets.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <json.h>

#include "server.h"
#include "utils.h"
#include "pty.h"
#include "http_api.h"

// Context for PTY callbacks when using SSE
typedef struct {
    struct pss_sse *pss;
    bool closed;
} pty_sse_ctx_t;

// List of SSE clients
static struct pss_sse **sse_clients = NULL;
static int sse_client_count = 0;

// Initialize SSE client list
void sse_clients_init() {
    if (sse_clients == NULL) {
        sse_clients = xmalloc(sizeof(struct pss_sse*) * server->max_clients);
        sse_client_count = 0;
    }
}

// Add a client to the list
void sse_client_add(struct pss_sse *client) {
    for (int i = 0; i < server->max_clients; i++) {
        if (sse_clients[i] == NULL) {
            sse_clients[i] = client;
            sse_client_count++;
            return;
        }
    }
}

// Remove a client from the list
void sse_client_remove(struct pss_sse *client) {
    for (int i = 0; i < server->max_clients; i++) {
        if (sse_clients[i] == client) {
            sse_clients[i] = NULL;
            sse_client_count--;
            return;
        }
    }
}

// Find a client by ID
struct pss_sse *sse_client_find(const char *id) {
    for (int i = 0; i < server->max_clients; i++) {
        if (sse_clients[i] != NULL && strcmp(sse_clients[i]->path, id) == 0) {
            return sse_clients[i];
        }
    }
    return NULL;
}

// Initialize the SSE context
static pty_sse_ctx_t *pty_sse_ctx_init(struct pss_sse *pss) {
    pty_sse_ctx_t *ctx = xmalloc(sizeof(pty_sse_ctx_t));
    ctx->pss = pss;
    ctx->closed = false;
    return ctx;
}

// Free the SSE context
static void pty_sse_ctx_free(pty_sse_ctx_t *ctx) {
    free(ctx);
}

// Process read callback for SSE
static void sse_process_read_cb(pty_process *process, pty_buf_t *buf, bool eof) {
    pty_sse_ctx_t *ctx = (pty_sse_ctx_t *)process->ctx;
    if (ctx->closed) {
        pty_buf_free(buf);
        return;
    }

    if (eof && !process_running(process))
        ctx->pss->lws_close_status = process->exit_code == 0 ? 1000 : 1006;
    else
        ctx->pss->pty_buf = buf;
    
    lws_callback_on_writable(ctx->pss->wsi);
}

// Process exit callback for SSE
static void sse_process_exit_cb(pty_process *process) {
    pty_sse_ctx_t *ctx = (pty_sse_ctx_t *)process->ctx;
    if (ctx->closed) {
        lwsl_notice("SSE process killed with signal %d, pid: %d\n", process->exit_signal, process->pid);
        goto done;
    }

    lwsl_notice("SSE process exited with code %d, pid: %d\n", process->exit_code, process->pid);
    ctx->pss->process = NULL;
    ctx->pss->lws_close_status = process->exit_code == 0 ? 1000 : 1006;
    lws_callback_on_writable(ctx->pss->wsi);

done:
    pty_sse_ctx_free(ctx);
}

// Send SSE headers
static int send_sse_headers(struct lws *wsi) {
    unsigned char buffer[1024 + LWS_PRE], *p, *end;
    p = buffer + LWS_PRE;
    end = p + sizeof(buffer) - LWS_PRE;

    if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
        lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, 
                                    (unsigned char *)"text/event-stream", 17, &p, end) ||
        lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CACHE_CONTROL, 
                                    (unsigned char *)"no-cache", 8, &p, end) ||
        lws_finalize_http_header(wsi, &p, end) ||
        lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
        return -1;

    return 0;
}

// Send an SSE event
static int send_sse_event(struct lws *wsi, const char *event, const char *data, unsigned long id) {
    char *buffer;
    int len = 0;
    
    // Calculate buffer size needed
    len += 7 + 10; // "id: " + id (max 10 digits) + "\n"
    len += 7 + strlen(event); // "event: " + event + "\n"
    len += 6 + strlen(data); // "data: " + data + "\n\n"
    
    buffer = xmalloc(LWS_PRE + len + 1);
    if (!buffer) return -1;
    
    char *p = buffer + LWS_PRE;
    
    // Format the SSE message
    p += sprintf(p, "id: %lu\n", id);
    p += sprintf(p, "event: %s\n", event);
    p += sprintf(p, "data: %s\n\n", data);
    
    // Write the message
    int n = lws_write_http(wsi, (unsigned char *)(buffer + LWS_PRE), p - (buffer + LWS_PRE));
    free(buffer);
    
    if (n < 0) return -1;
    
    return 0;
}

// Send terminal output as an SSE event
static int send_sse_output(struct lws *wsi, pty_buf_t *buf, unsigned long *event_id) {
    if (buf == NULL) return 0;
    
    // Base64 encode the terminal output
    size_t encoded_len = ((buf->len + 2) / 3) * 4 + 1; // Base64 encoding size
    char *encoded = xmalloc(encoded_len);
    if (!encoded) return -1;
    
    lws_b64_encode_string((const char *)buf->base, buf->len, encoded, encoded_len);
    
    // Send the event
    int result = send_sse_event(wsi, "output", encoded, (*event_id)++);
    free(encoded);
    
    return result;
}

// Spawn a process for SSE client
static bool sse_spawn_process(struct pss_sse *pss, uint16_t columns, uint16_t rows) {
    // Build command arguments (using the server's command)
    char **argv = xmalloc((server->argc + 1) * sizeof(char *));
    for (int i = 0; i < server->argc; i++) {
        argv[i] = server->argv[i];
    }
    argv[server->argc] = NULL;
    
    // Build environment variables
    char **envp = xmalloc(2 * sizeof(char *));
    envp[0] = xmalloc(36);
    snprintf(envp[0], 36, "TERM=%s", server->terminal_type);
    envp[1] = NULL;
    
    // Initialize and spawn the process
    pty_process *process = process_init((void *)pty_sse_ctx_init(pss), server->loop, argv, envp);
    if (server->cwd != NULL) process->cwd = strdup(server->cwd);
    if (columns > 0) process->columns = columns;
    if (rows > 0) process->rows = rows;
    
    if (pty_spawn(process, sse_process_read_cb, sse_process_exit_cb) != 0) {
        lwsl_err("pty_spawn for SSE: %d (%s)\n", errno, strerror(errno));
        process_free(process);
        return false;
    }
    
    lwsl_notice("started SSE process, pid: %d\n", process->pid);
    pss->process = process;
    lws_callback_on_writable(pss->wsi);
    
    return true;
}

// Handle SSE connection
int callback_sse(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    struct pss_sse *pss = (struct pss_sse *)user;
    
    switch (reason) {
        case LWS_CALLBACK_HTTP:
            // Initialize the SSE client
            pss->initialized = false;
            pss->authenticated = true; // For simplicity, we're not implementing auth here
            pss->wsi = wsi;
            pss->lws_close_status = LWS_CLOSE_STATUS_NOSTATUS;
            pss->sse_headers_sent = false;
            pss->event_id = 1;
            
            // Store the path as client ID
            snprintf(pss->path, sizeof(pss->path), "%s", (const char *)in);
            
            // Get client IP
            lws_get_peer_simple(lws_get_network_wsi(wsi), pss->address, sizeof(pss->address));
            lwsl_notice("SSE %s - %s\n", pss->path, pss->address);
            
            // Add to client list
            sse_client_add(pss);
            
            // Send SSE headers
            if (send_sse_headers(wsi) < 0) {
                return -1;
            }
            pss->sse_headers_sent = true;
            
            // Spawn the process with default terminal size
            if (!sse_spawn_process(pss, 80, 24)) {
                return -1;
            }
            
            // Request a callback when we can write
            lws_callback_on_writable(wsi);
            break;
            
        case LWS_CALLBACK_HTTP_WRITEABLE:
            if (!pss->sse_headers_sent) {
                if (send_sse_headers(wsi) < 0) {
                    return -1;
                }
                pss->sse_headers_sent = true;
            }
            
            // Send terminal output if available
            if (pss->pty_buf != NULL) {
                if (send_sse_output(wsi, pss->pty_buf, &pss->event_id) < 0) {
                    pty_buf_free(pss->pty_buf);
                    pss->pty_buf = NULL;
                    return -1;
                }
                
                pty_buf_free(pss->pty_buf);
                pss->pty_buf = NULL;
                pty_resume(pss->process);
            }
            
            // If the process has exited, send a close event
            if (pss->lws_close_status > LWS_CLOSE_STATUS_NOSTATUS) {
                send_sse_event(wsi, "close", 
                              pss->lws_close_status == 1000 ? "normal" : "abnormal", 
                              pss->event_id++);
                return -1;
            }
            
            // Request another callback when we can write again
            lws_callback_on_writable(wsi);
            break;
            
        case LWS_CALLBACK_CLOSED_HTTP:
            lwsl_notice("SSE connection closed from %s\n", pss->address);
            
            // Remove from client list
            sse_client_remove(pss);
            
            // Clean up resources
            if (pss->pty_buf != NULL) {
                pty_buf_free(pss->pty_buf);
                pss->pty_buf = NULL;
            }
            
            if (pss->process != NULL) {
                ((pty_sse_ctx_t *)pss->process->ctx)->closed = true;
                if (process_running(pss->process)) {
                    pty_pause(pss->process);
                    lwsl_notice("killing SSE process, pid: %d\n", pss->process->pid);
                    pty_kill(pss->process, server->sig_code);
                }
            }
            break;
            
        default:
            break;
    }
    
    return 0;
}

// Handle HTTP POST requests to send input to a terminal
int callback_http_api(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    if (reason != LWS_CALLBACK_HTTP_BODY) {
        return 0;
    }
    
    // Get the URL path
    char path[128];
    int path_len = lws_hdr_copy(wsi, path, sizeof(path), WSI_TOKEN_GET_URI);
    if (path_len <= 0) {
        lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid request");
        return -1;
    }
    
    // Extract the client ID from the path
    // Format: /api/terminal/{id}/input
    char *id_start = strstr(path, "/terminal/");
    if (!id_start) {
        lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid terminal ID");
        return -1;
    }
    
    id_start += 10; // Skip "/terminal/"
    char *id_end = strchr(id_start, '/');
    if (!id_end) {
        lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid API endpoint");
        return -1;
    }
    
    // Extract the client ID
    char client_id[64] = {0};
    strncpy(client_id, id_start, id_end - id_start);
    
    // Find the client
    struct pss_sse *client = sse_client_find(client_id);
    if (!client || !client->process) {
        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "Terminal not found");
        return -1;
    }
    
    // Check the endpoint
    char *endpoint = id_end + 1;
    if (strcmp(endpoint, "input") == 0) {
        // Send input to the terminal
        if (!server->writable) {
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, "Terminal is read-only");
            return -1;
        }
        
        // Create a buffer with the input data
        pty_buf_t *buf = pty_buf_init((char *)in, len);
        
        // Write to the terminal
        int err = pty_write(client->process, buf);
        if (err) {
            lwsl_err("uv_write: %s (%s)\n", uv_err_name(err), uv_strerror(err));
            lws_return_http_status(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to write to terminal");
            return -1;
        }
        
        // Return success
        unsigned char buffer[LWS_PRE + 256], *p, *end;
        p = buffer + LWS_PRE;
        end = p + 256;
        
        if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
            lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, 
                                        (unsigned char *)"application/json", 16, &p, end) ||
            lws_add_http_header_content_length(wsi, 15, &p, end) ||
            lws_finalize_http_header(wsi, &p, end) ||
            lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0) {
            return -1;
        }
        
        if (lws_write_http(wsi, (unsigned char *)"{\"status\":\"ok\"}", 15) < 0) {
            return -1;
        }
        
        return -1; // Return -1 to close the connection
    } else if (strcmp(endpoint, "resize") == 0) {
        // Parse the JSON data
        json_object *obj = json_tokener_parse((const char *)in);
        if (!obj) {
            lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid JSON");
            return -1;
        }
        
        // Extract columns and rows
        struct json_object *cols_obj = NULL, *rows_obj = NULL;
        uint16_t cols = 0, rows = 0;
        
        if (json_object_object_get_ex(obj, "columns", &cols_obj)) {
            cols = (uint16_t)json_object_get_int(cols_obj);
        }
        
        if (json_object_object_get_ex(obj, "rows", &rows_obj)) {
            rows = (uint16_t)json_object_get_int(rows_obj);
        }
        
        json_object_put(obj);
        
        if (cols <= 0 || rows <= 0) {
            lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid terminal size");
            return -1;
        }
        
        // Resize the terminal
        client->process->columns = cols;
        client->process->rows = rows;
        pty_resize(client->process);
        
        // Return success
        unsigned char buffer[LWS_PRE + 256], *p, *end;
        p = buffer + LWS_PRE;
        end = p + 256;
        
        if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
            lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, 
                                        (unsigned char *)"application/json", 16, &p, end) ||
            lws_add_http_header_content_length(wsi, 15, &p, end) ||
            lws_finalize_http_header(wsi, &p, end) ||
            lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0) {
            return -1;
        }
        
        if (lws_write_http(wsi, (unsigned char *)"{\"status\":\"ok\"}", 15) < 0) {
            return -1;
        }
        
        return -1; // Return -1 to close the connection
    } else {
        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "Endpoint not found");
        return -1;
    }
    
    return 0;
}