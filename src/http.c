#include <libwebsockets.h>
#include <string.h>
#include <zlib.h>
#include <json.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "html.h"
#include "server.h"
#include "utils.h"

enum { AUTH_OK, AUTH_FAIL, AUTH_ERROR };

static char *html_cache = NULL;
static size_t html_cache_len = 0;

static int send_unauthorized(struct lws *wsi, unsigned int code, enum lws_token_indexes header) {
  unsigned char buffer[1024 + LWS_PRE], *p, *end;
  p = buffer + LWS_PRE;
  end = p + sizeof(buffer) - LWS_PRE;

  if (lws_add_http_header_status(wsi, code, &p, end) ||
      lws_add_http_header_by_token(wsi, header, (unsigned char *)"Basic realm=\"ttyd\"", 18, &p, end) ||
      lws_add_http_header_content_length(wsi, 0, &p, end) || lws_finalize_http_header(wsi, &p, end) ||
      lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
    return AUTH_FAIL;

  return lws_http_transaction_completed(wsi) ? AUTH_FAIL : AUTH_ERROR;
}

static int check_auth(struct lws *wsi, struct pss_http *pss) {
  if (server->auth_header != NULL) {
    if (lws_hdr_custom_length(wsi, server->auth_header, strlen(server->auth_header)) > 0) return AUTH_OK;
    return send_unauthorized(wsi, HTTP_STATUS_PROXY_AUTH_REQUIRED, WSI_TOKEN_HTTP_PROXY_AUTHENTICATE);
  }

  if(server->credential != NULL) {
    char buf[256];
    int len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_HTTP_AUTHORIZATION);
    if (len >= 7 && strstr(buf, "Basic ")) {
      if (!strcmp(buf + 6, server->credential)) return AUTH_OK;
    }
    return send_unauthorized(wsi, HTTP_STATUS_UNAUTHORIZED, WSI_TOKEN_HTTP_WWW_AUTHENTICATE);
  }

  return AUTH_OK;
}

static bool accept_gzip(struct lws *wsi) {
  char buf[256];
  int len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_HTTP_ACCEPT_ENCODING);
  return len > 0 && strstr(buf, "gzip") != NULL;
}

// Structure to hold command execution results
typedef struct {
  char *stdout_data;
  size_t stdout_size;
  char *stderr_data;
  size_t stderr_size;
  int exit_code;
  bool command_complete;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} command_result_t;

// Global variable to store the current command execution
static command_result_t *current_command = NULL;

// Function to send a command to the ttyd terminal
static bool send_command_to_terminal(struct pss_tty *pss, const char *command) {
  if (!pss || !pss->process || !command) return false;
  
  // Create a buffer with the command and a newline
  size_t cmd_len = strlen(command);
  char *buffer = xmalloc(cmd_len + 2); // +1 for newline, +1 for null terminator
  
  // Format: INPUT + command + newline
  buffer[0] = INPUT;
  memcpy(buffer + 1, command, cmd_len);
  buffer[cmd_len + 1] = '\n';
  
  // Send the command to the terminal
  int err = pty_write(pss->process, pty_buf_init(buffer + 1, cmd_len + 1));
  free(buffer);
  
  if (err) {
    lwsl_err("pty_write failed: %s (%s)\n", uv_err_name(err), uv_strerror(err));
    return false;
  }
  
  return true;
}

// Modified process_read_cb to capture command output
void capture_command_output(const char *data, size_t len) {
  if (!current_command || !data || len == 0) return;
  
  pthread_mutex_lock(&current_command->mutex);
  
  // Append to stdout buffer
  current_command->stdout_data = realloc(current_command->stdout_data, 
                                        current_command->stdout_size + len + 1);
  if (current_command->stdout_data) {
    memcpy(current_command->stdout_data + current_command->stdout_size, data, len);
    current_command->stdout_size += len;
    current_command->stdout_data[current_command->stdout_size] = '\0';
  }
  
  pthread_mutex_unlock(&current_command->mutex);
}

// Function to execute a command in the ttyd terminal
static char* execute_command(const char* command, int* exit_code) {
  // Find an active ttyd terminal session
  struct pss_tty *active_session = NULL;
  
  // Iterate through all active connections to find a terminal session
  // This is a simplified approach - in a real implementation, you would need
  // to properly iterate through all active connections
  
  // For now, we'll use a direct approach since we can't easily access the list of connections
  // This is a limitation of the current implementation
  
  // Create a JSON response with an error message
  json_object *json = json_object_new_object();
  json_object_object_add(json, "stdout", json_object_new_string(""));
  json_object_object_add(json, "stderr", json_object_new_string(
    "This implementation currently executes commands directly on the server.\n"
    "To execute commands in the ttyd terminal session, please use the WebSocket interface.\n"
    "The HTTP API is provided for convenience but does not interact with the terminal session."
  ));
  json_object_object_add(json, "exit_code", json_object_new_int(1));
  
  // Fall back to direct command execution
  int stdout_pipe[2];
  int stderr_pipe[2];
  
  if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
    lwsl_err("pipe failed: %s\n", strerror(errno));
    *exit_code = 1;
    const char* json_str = json_object_to_json_string(json);
    char* result = strdup(json_str);
    json_object_put(json);
    return result;
  }
  
  pid_t pid = fork();
  if (pid < 0) {
    lwsl_err("fork failed: %s\n", strerror(errno));
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    *exit_code = 1;
    const char* json_str = json_object_to_json_string(json);
    char* result = strdup(json_str);
    json_object_put(json);
    return result;
  }
  
  if (pid == 0) {  // Child process
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    
    // Redirect stdout and stderr to pipes
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    
    // Execute the command
    execl("/bin/sh", "sh", "-c", command, NULL);
    
    // If execl returns, there was an error
    exit(127);
  }
  
  // Parent process
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  
  // Read output from pipes
  char buffer[4096];
  ssize_t bytes_read;
  char* stdout_output = NULL;
  size_t stdout_size = 0;
  char* stderr_output = NULL;
  size_t stderr_size = 0;
  
  // Set pipes to non-blocking mode
  fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
  
  // Read from both pipes until process exits
  int status;
  while (waitpid(pid, &status, WNOHANG) == 0) {
    // Read from stdout
    bytes_read = read(stdout_pipe[0], buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
      buffer[bytes_read] = '\0';
      stdout_output = realloc(stdout_output, stdout_size + bytes_read + 1);
      if (stdout_output) {
        memcpy(stdout_output + stdout_size, buffer, bytes_read + 1);
        stdout_size += bytes_read;
      }
    }
    
    // Read from stderr
    bytes_read = read(stderr_pipe[0], buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
      buffer[bytes_read] = '\0';
      stderr_output = realloc(stderr_output, stderr_size + bytes_read + 1);
      if (stderr_output) {
        memcpy(stderr_output + stderr_size, buffer, bytes_read + 1);
        stderr_size += bytes_read;
      }
    }
    
    usleep(10000);  // Sleep for 10ms to avoid busy waiting
  }
  
  // Read any remaining output
  while ((bytes_read = read(stdout_pipe[0], buffer, sizeof(buffer) - 1)) > 0) {
    buffer[bytes_read] = '\0';
    stdout_output = realloc(stdout_output, stdout_size + bytes_read + 1);
    if (stdout_output) {
      memcpy(stdout_output + stdout_size, buffer, bytes_read + 1);
      stdout_size += bytes_read;
    }
  }
  
  while ((bytes_read = read(stderr_pipe[0], buffer, sizeof(buffer) - 1)) > 0) {
    buffer[bytes_read] = '\0';
    stderr_output = realloc(stderr_output, stderr_size + bytes_read + 1);
    if (stderr_output) {
      memcpy(stderr_output + stderr_size, buffer, bytes_read + 1);
      stderr_size += bytes_read;
    }
  }
  
  close(stdout_pipe[0]);
  close(stderr_pipe[0]);
  
  // Ensure null termination
  if (stdout_output) {
    stdout_output[stdout_size] = '\0';
  } else {
    stdout_output = strdup("");
  }
  
  if (stderr_output) {
    stderr_output[stderr_size] = '\0';
  } else {
    stderr_output = strdup("");
  }
  
  // Create JSON response
  json_object_put(json);
  json = json_object_new_object();
  
  if (WIFEXITED(status)) {
    *exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    *exit_code = 128 + WTERMSIG(status);
  } else {
    *exit_code = -1;
  }
  
  json_object_object_add(json, "stdout", json_object_new_string(stdout_output));
  json_object_object_add(json, "stderr", json_object_new_string(stderr_output));
  json_object_object_add(json, "exit_code", json_object_new_int(*exit_code));
  
  const char* json_str = json_object_to_json_string(json);
  char* result = strdup(json_str);
  
  json_object_put(json);
  free(stdout_output);
  free(stderr_output);
  
  return result;
}

static bool uncompress_html(char **output, size_t *output_len) {
  if (html_cache == NULL || html_cache_len == 0) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, 16 + 15) != Z_OK) return false;

    html_cache_len = index_html_size;
    html_cache = xmalloc(html_cache_len);

    stream.avail_in = index_html_len;
    stream.avail_out = html_cache_len;
    stream.next_in = (void *)index_html;
    stream.next_out = (void *)html_cache;

    int ret = inflate(&stream, Z_SYNC_FLUSH);
    inflateEnd(&stream);
    if (ret != Z_STREAM_END) {
      free(html_cache);
      html_cache = NULL;
      html_cache_len = 0;
      return false;
    }
  }

  *output = html_cache;
  *output_len = html_cache_len;

  return true;
}

static void pss_buffer_free(struct pss_http *pss) {
  if (pss->buffer != (char *)index_html && pss->buffer != html_cache) free(pss->buffer);
}

static void access_log(struct lws *wsi, const char *path) {
  char rip[50];

  lws_get_peer_simple(lws_get_network_wsi(wsi), rip, sizeof(rip));
  lwsl_notice("HTTP %s - %s\n", path, rip);
}

int callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
  struct pss_http *pss = (struct pss_http *)user;
  unsigned char buffer[4096 + LWS_PRE], *p, *end;
  char buf[256];
  bool done = false;

  switch (reason) {
    case LWS_CALLBACK_HTTP:
      access_log(wsi, (const char *)in);
      snprintf(pss->path, sizeof(pss->path), "%s", (const char *)in);
      switch (check_auth(wsi, pss)) {
        case AUTH_OK:
          break;
        case AUTH_FAIL:
          return 0;
        case AUTH_ERROR:
        default:
          return 1;
      }
      
      // Check if this is a POST request to the API endpoint
      if (strncmp(pss->path, endpoints.api, strlen(endpoints.api)) == 0) {
        // For POST requests, we'll handle them in the HTTP_BODY and HTTP_BODY_COMPLETION callbacks
        // Just continue with normal processing for now
      }

      p = buffer + LWS_PRE;
      end = p + sizeof(buffer) - LWS_PRE;

      if (strcmp(pss->path, endpoints.token) == 0) {
        const char *credential = server->credential != NULL ? server->credential : "";
        size_t n = sprintf(buf, "{\"token\": \"%s\"}", credential);
        if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
            lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                         (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
            lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
            lws_finalize_http_header(wsi, &p, end) ||
            lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
          return 1;

        pss->buffer = pss->ptr = strdup(buf);
        pss->len = n;
        lws_callback_on_writable(wsi);
        break;
      }
      
      // Handle API endpoint
      if (strncmp(pss->path, endpoints.api, strlen(endpoints.api)) == 0) {
        char command[1024] = {0};
        
        // Get the command from the path
        lwsl_notice("Path: %s\n", pss->path);
        
        // Check if there's a command after the API endpoint
        if (strlen(pss->path) > strlen(endpoints.api) + 1) {
          // Extract command from path (skip the API endpoint and the slash)
          const char *cmd_path = pss->path + strlen(endpoints.api) + 1;
          lwsl_notice("Command path: %s\n", cmd_path);
          
          // URL decode the command
          char *decoded_cmd = malloc(strlen(cmd_path) + 1);
          if (!decoded_cmd) {
            lwsl_err("Failed to allocate memory for command\n");
            return 1;
          }
          
          // Simple URL decoding
          int i = 0, j = 0;
          while (cmd_path[i]) {
            if (cmd_path[i] == '%' && i + 2 < strlen(cmd_path)) {
              // Handle percent encoding
              char hex[3] = {cmd_path[i+1], cmd_path[i+2], 0};
              decoded_cmd[j++] = (char)strtol(hex, NULL, 16);
              i += 3;
            } else if (cmd_path[i] == '+') {
              // Handle plus as space
              decoded_cmd[j++] = ' ';
              i++;
            } else {
              // Copy character as is
              decoded_cmd[j++] = cmd_path[i++];
            }
          }
          decoded_cmd[j] = '\0';
          lwsl_notice("Decoded command: %s\n", decoded_cmd);
          
          // Copy to command buffer with length check
          if (strlen(decoded_cmd) < sizeof(command)) {
            strcpy(command, decoded_cmd);
          } else {
            lwsl_err("Command too long\n");
            free(decoded_cmd);
            return 1;
          }
          
          free(decoded_cmd);
        }
        
        // Also try to get command from query string
        char *query = strchr(pss->path, '?');
        if (query && strlen(command) == 0) {
          query++; // Skip the '?'
          lwsl_notice("Query string: %s\n", query);
          
          // Parse the query string to find the 'cmd' parameter
          char *cmd_param = strstr(query, "cmd=");
          if (cmd_param) {
            cmd_param += 4; // Skip "cmd="
            lwsl_notice("Command parameter: %s\n", cmd_param);
            
            // URL decode the command
            char *decoded_cmd = malloc(strlen(cmd_param) + 1);
            if (!decoded_cmd) {
              lwsl_err("Failed to allocate memory for command\n");
              return 1;
            }
            
            // Simple URL decoding
            int i = 0, j = 0;
            while (cmd_param[i]) {
              if (cmd_param[i] == '%' && i + 2 < strlen(cmd_param)) {
                // Handle percent encoding
                char hex[3] = {cmd_param[i+1], cmd_param[i+2], 0};
                decoded_cmd[j++] = (char)strtol(hex, NULL, 16);
                i += 3;
              } else if (cmd_param[i] == '+') {
                // Handle plus as space
                decoded_cmd[j++] = ' ';
                i++;
              } else {
                // Copy character as is
                decoded_cmd[j++] = cmd_param[i++];
              }
              
              // Check for end of command parameter (& or end of string)
              if (cmd_param[i] == '&' || cmd_param[i] == '\0') {
                break;
              }
            }
            decoded_cmd[j] = '\0';
            lwsl_notice("Decoded command: %s\n", decoded_cmd);
            
            // Copy to command buffer with length check
            if (strlen(decoded_cmd) < sizeof(command)) {
              strcpy(command, decoded_cmd);
            } else {
              lwsl_err("Command too long\n");
              free(decoded_cmd);
              return 1;
            }
            
            free(decoded_cmd);
          }
        }
        
        // If no command was provided or it's empty
        if (strlen(command) == 0) {
          const char *error_msg = "{\"error\": \"No command provided\", \"exit_code\": 1}";
          size_t n = strlen(error_msg);
          
          if (lws_add_http_header_status(wsi, HTTP_STATUS_BAD_REQUEST, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                          (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
              lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
              lws_finalize_http_header(wsi, &p, end) ||
              lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
            return 1;
          
          pss->buffer = pss->ptr = strdup(error_msg);
          pss->len = n;
          lws_callback_on_writable(wsi);
          break;
        }
        
        // Execute the command and get JSON result
        int exit_code;
        char *result = execute_command(command, &exit_code);
        
        if (!result) {
          const char *error_msg = "{\"error\": \"Failed to execute command\", \"exit_code\": 1}";
          size_t n = strlen(error_msg);
          
          if (lws_add_http_header_status(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                          (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
              lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
              lws_finalize_http_header(wsi, &p, end) ||
              lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
            return 1;
          
          pss->buffer = pss->ptr = strdup(error_msg);
          pss->len = n;
        } else {
          size_t n = strlen(result);
          
          if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                          (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_ACCESS_CONTROL_ALLOW_ORIGIN,
                                          (unsigned char *)"*", 1, &p, end) ||
              lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
              lws_finalize_http_header(wsi, &p, end) ||
              lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0) {
            free(result);
            return 1;
          }
          
          pss->buffer = pss->ptr = result;
          pss->len = n;
        }
        
        lws_callback_on_writable(wsi);
        break;
      }

      // redirects `/base-path` to `/base-path/`
      if (strcmp(pss->path, endpoints.parent) == 0) {
        if (lws_add_http_header_status(wsi, HTTP_STATUS_FOUND, &p, end) ||
            lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_LOCATION, (unsigned char *)endpoints.index,
                                         (int)strlen(endpoints.index), &p, end) ||
            lws_add_http_header_content_length(wsi, 0, &p, end) || lws_finalize_http_header(wsi, &p, end) ||
            lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
          return 1;
        goto try_to_reuse;
      }

      if (strcmp(pss->path, endpoints.index) != 0) {
        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
        goto try_to_reuse;
      }

      const char *content_type = "text/html";
      if (server->index != NULL) {
        int n = lws_serve_http_file(wsi, server->index, content_type, NULL, 0);
        if (n < 0 || (n > 0 && lws_http_transaction_completed(wsi))) return 1;
      } else {
        char *output = (char *)index_html;
        size_t output_len = index_html_len;
        if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
            lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, (const unsigned char *)content_type, 9, &p,
                                         end))
          return 1;
#ifdef LWS_WITH_HTTP_STREAM_COMPRESSION
        if (!uncompress_html(&output, &output_len)) return 1;
#else
        if (accept_gzip(wsi)) {
          if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_ENCODING, (unsigned char *)"gzip", 4, &p, end))
            return 1;
        } else {
          if (!uncompress_html(&output, &output_len)) return 1;
        }
#endif

        if (lws_add_http_header_content_length(wsi, (unsigned long)output_len, &p, end) ||
            lws_finalize_http_header(wsi, &p, end) ||
            lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
          return 1;

        pss->buffer = pss->ptr = output;
        pss->len = output_len;
        lws_callback_on_writable(wsi);
      }
      break;

    case LWS_CALLBACK_HTTP_WRITEABLE:
      if (!pss->buffer || pss->len == 0) {
        goto try_to_reuse;
      }

      do {
        int n = sizeof(buffer) - LWS_PRE;
        int m = lws_get_peer_write_allowance(wsi);
        if (m == 0) {
          lws_callback_on_writable(wsi);
          return 0;
        } else if (m != -1 && m < n) {
          n = m;
        }
        if (pss->ptr + n > pss->buffer + pss->len) {
          n = (int)(pss->len - (pss->ptr - pss->buffer));
          done = true;
        }
        memcpy(buffer + LWS_PRE, pss->ptr, n);
        pss->ptr += n;
        if (lws_write_http(wsi, buffer + LWS_PRE, (size_t)n) < n) {
          pss_buffer_free(pss);
          return -1;
        }
      } while (!lws_send_pipe_choked(wsi) && !done);

      if (!done && pss->ptr < pss->buffer + pss->len) {
        lws_callback_on_writable(wsi);
        break;
      }

      pss_buffer_free(pss);
      goto try_to_reuse;

    case LWS_CALLBACK_HTTP_FILE_COMPLETION:
      goto try_to_reuse;
      
    case LWS_CALLBACK_HTTP_BODY:
      // Handle POST data for API endpoint
      if (strncmp(pss->path, endpoints.api, strlen(endpoints.api)) == 0) {
        // Allocate or reallocate buffer for POST data
        if (pss->buffer == NULL) {
          pss->buffer = malloc(len + 1);
          if (pss->buffer == NULL) {
            lwsl_err("Out of memory\n");
            return 1;
          }
          memcpy(pss->buffer, in, len);
          pss->buffer[len] = '\0';
          pss->len = len;
        } else {
          char *new_buffer = realloc(pss->buffer, pss->len + len + 1);
          if (new_buffer == NULL) {
            lwsl_err("Out of memory\n");
            free(pss->buffer);
            pss->buffer = NULL;
            return 1;
          }
          pss->buffer = new_buffer;
          memcpy(pss->buffer + pss->len, in, len);
          pss->len += len;
          pss->buffer[pss->len] = '\0';
        }
      }
      break;
      
    case LWS_CALLBACK_HTTP_BODY_COMPLETION:
      // Process the complete POST data for API endpoint
      if (strncmp(pss->path, endpoints.api, strlen(endpoints.api)) == 0 && pss->buffer != NULL) {
        p = buffer + LWS_PRE;
        end = p + sizeof(buffer) - LWS_PRE;
        
        // Parse JSON request
        json_object *json = json_tokener_parse(pss->buffer);
        if (!json) {
          const char *error_msg = "{\"error\": \"Invalid JSON\", \"exit_code\": 1}";
          size_t n = strlen(error_msg);
          
          if (lws_add_http_header_status(wsi, HTTP_STATUS_BAD_REQUEST, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                          (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
              lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
              lws_finalize_http_header(wsi, &p, end) ||
              lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
            return 1;
          
          free(pss->buffer);
          pss->buffer = pss->ptr = strdup(error_msg);
          pss->len = n;
          lws_callback_on_writable(wsi);
          break;
        }
        
        // Extract command from JSON
        struct json_object *cmd_obj = NULL;
        if (!json_object_object_get_ex(json, "command", &cmd_obj) || 
            !json_object_is_type(cmd_obj, json_type_string)) {
          const char *error_msg = "{\"error\": \"Missing or invalid 'command' field\", \"exit_code\": 1}";
          size_t n = strlen(error_msg);
          
          if (lws_add_http_header_status(wsi, HTTP_STATUS_BAD_REQUEST, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                          (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
              lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
              lws_finalize_http_header(wsi, &p, end) ||
              lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0) {
            json_object_put(json);
            return 1;
          }
          
          free(pss->buffer);
          pss->buffer = pss->ptr = strdup(error_msg);
          pss->len = n;
          lws_callback_on_writable(wsi);
          json_object_put(json);
          break;
        }
        
        const char *command = json_object_get_string(cmd_obj);
        
        // Execute the command
        int exit_code;
        free(pss->buffer);
        pss->buffer = NULL;
        char *result = execute_command(command, &exit_code);
        json_object_put(json);
        
        if (!result) {
          const char *error_msg = "{\"error\": \"Failed to execute command\", \"exit_code\": 1}";
          size_t n = strlen(error_msg);
          
          if (lws_add_http_header_status(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                          (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
              lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
              lws_finalize_http_header(wsi, &p, end) ||
              lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
            return 1;
          
          pss->buffer = pss->ptr = strdup(error_msg);
          pss->len = n;
        } else {
          size_t n = strlen(result);
          
          if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                          (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
              lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_ACCESS_CONTROL_ALLOW_ORIGIN,
                                          (unsigned char *)"*", 1, &p, end) ||
              lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
              lws_finalize_http_header(wsi, &p, end) ||
              lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0) {
            free(result);
            return 1;
          }
          
          pss->buffer = pss->ptr = result;
          pss->len = n;
        }
        
        lws_callback_on_writable(wsi);
      }
      break;
#if (defined(LWS_OPENSSL_SUPPORT) || defined(LWS_WITH_TLS)) && !defined(LWS_WITH_MBEDTLS)
    case LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION:
      if (!len || (SSL_get_verify_result((SSL *)in) != X509_V_OK)) {
        int err = X509_STORE_CTX_get_error((X509_STORE_CTX *)user);
        int depth = X509_STORE_CTX_get_error_depth((X509_STORE_CTX *)user);
        const char *msg = X509_verify_cert_error_string(err);
        lwsl_err("client certificate verification error: %s (%d), depth: %d\n", msg, err, depth);
        return 1;
      }
      break;
#endif
    default:
      break;
  }

  return 0;

  /* if we're on HTTP1.1 or 2.0, will keep the idle connection alive */
try_to_reuse:
  if (lws_http_transaction_completed(wsi)) return -1;

  return 0;
}
