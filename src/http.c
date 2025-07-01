#include <libwebsockets.h>
#include <string.h>
#include <zlib.h>
#include <json.h> // For JSON parsing
#include <unistd.h> // For pipe, fork, exec
#include <sys/wait.h> // For waitpid
#include <errno.h> // For errno

#include "html.h"
#include "server.h"
#include "utils.h"
#include "pty.h" // For PTY functions

enum { AUTH_OK, AUTH_FAIL, AUTH_ERROR };

// Helper function to execute command and capture output
struct command_result {
    char *stdout_str;
    char *stderr_str;
    int exit_code;
};

//static struct command_result execute_command(const char *command_str) {
static struct command_result execute_command(char *const argv[]) { // Changed signature
    struct command_result result = {NULL, NULL, -1};
    int stdout_pipe[2];
    int stderr_pipe[2];
    pid_t pid;

    if (pipe(stdout_pipe) == -1 || pipe(stderr_pipe) == -1) {
        perror("pipe");
        return result; // Indicate error
    }

    pid = fork();
    if (pid == -1) {
        perror("fork");
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return result; // Indicate error
    }

    if (pid == 0) { // Child process
        close(stdout_pipe[0]); // Close read end of stdout pipe
        dup2(stdout_pipe[1], STDOUT_FILENO); // Redirect stdout
        close(stdout_pipe[1]); // Close write end of stdout pipe

        close(stderr_pipe[0]); // Close read end of stderr pipe
        dup2(stderr_pipe[1], STDERR_FILENO); // Redirect stderr
        close(stderr_pipe[1]); // Close write end of stderr pipe

        // execvp expects a null-terminated array of strings (char *argv[])
        // command_str is now expected to be a json array of strings ["command", "arg1", "arg2"]
        // We need to parse this outside and pass the array to execute_command,
        // or parse it here. For simplicity, let's assume command_str is just the command
        // and we will modify the calling code to pass arguments properly.
        // For now, let's adjust this to take char *const argv[]
        // No, let's change the input to execute_command to be char *const *argv
        // The parsing of JSON into argv will happen in LWS_CALLBACK_HTTP_BODY_COMPLETION.
        // This function will now expect `char *const argv[]`

        // The function signature needs to change.
        // static struct command_result execute_command(const char *command_str) ->
        // static struct command_result execute_command(char *const argv[])
        // This change will be made in multiple steps. First, let's adjust the execvp call assuming argv is passed.
        // For now, we'll keep the old parsing as a placeholder until the calling code is updated.
        // THIS SECTION WILL BE REPLACED AFTER JSON PARSING IS UPDATED.
        // The above comments are now outdated. We directly use argv.

        if (!argv || !argv[0]) {
            fprintf(stderr, "No command provided to execute_command\n");
            _exit(127); // Should not happen if called correctly
        }

        execvp(argv[0], argv);
        perror("execvp"); // Log to ttyd's stderr, not client's
        _exit(127); // Exit child if execvp fails
    } else { // Parent process
        close(stdout_pipe[1]); // Close write end of stdout pipe
        close(stderr_pipe[1]); // Close write end of stderr pipe

        char buffer[4096];
        ssize_t bytes_read;

        // Read stdout
        result.stdout_str = strdup(""); // Initialize to empty string
        while ((bytes_read = read(stdout_pipe[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            char *temp = result.stdout_str;
            result.stdout_str = xmalloc(strlen(temp) + bytes_read + 1);
            strcpy(result.stdout_str, temp);
            strcat(result.stdout_str, buffer);
            free(temp);
        }
        close(stdout_pipe[0]);

        // Read stderr
        result.stderr_str = strdup(""); // Initialize to empty string
        while ((bytes_read = read(stderr_pipe[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            char *temp = result.stderr_str;
            result.stderr_str = xmalloc(strlen(temp) + bytes_read + 1);
            strcpy(result.stderr_str, temp);
            strcat(result.stderr_str, buffer);
            free(temp);
        }
        close(stderr_pipe[0]);

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else {
            result.exit_code = -1; // Indicate error or signal termination
        }
    }
    return result;
}


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

      p = buffer + LWS_PRE;
      end = p + sizeof(buffer) - LWS_PRE;

      if (strcmp(pss->path, "/api/command") == 0) {
        // This callback is called when the HTTP headers are received.
        // We need to wait for the request body to be received before processing the command.
        // For now, we'll just set a flag or state in pss to indicate that we're expecting a body for this request.
        // The actual processing will happen in LWS_CALLBACK_HTTP_BODY.
        // However, libwebsockets handles HTTP POST body differently.
        // We need to read it in LWS_CALLBACK_HTTP_BODY_COMPLETION or by checking lws_remaining_packet_payload in LWS_CALLBACK_HTTP.
        // For simplicity, we'll assume the body is small and comes in one go for now.
        // A more robust solution would handle fragmented POST bodies.

        // Check if it's a POST request
        if (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) == 0) {
            // Not a POST request, or some other method.
            const char *err_msg = "{\"error\": \"Invalid request method. Only POST is supported.\"}";
            size_t n = strlen(err_msg);
            if (lws_add_http_header_status(wsi, HTTP_STATUS_METHOD_NOT_ALLOWED, &p, end) ||
                lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
                lws_add_http_header_content_length(wsi, (unsigned long)n, &p, end) ||
                lws_finalize_http_header(wsi, &p, end) ||
                lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
                return 1;
            pss->buffer = pss->ptr = strdup(err_msg);
            pss->len = n;
            lws_callback_on_writable(wsi);
            return 0; // Or 1 for error?
        }

        // Indicate that we expect a body. We'll store the body in pss->post_data
        pss->post_data = NULL;
        pss->post_data_len = 0;
        // libwebsockets will call LWS_CALLBACK_HTTP_BODY / LWS_CALLBACK_HTTP_BODY_COMPLETION
        // We will handle the command execution there.
        // For now, just acknowledge the request.
        // A proper response will be sent after processing the body.
        // This part might need to be restructured if body handling is complex.
        return 0; // Defer response until body is processed

      } else if (strcmp(pss->path, endpoints.token) == 0) {
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
    case LWS_CALLBACK_HTTP_BODY:
        if (strcmp(pss->path, "/api/command") == 0) {
            // Append received data to pss->post_data
            if (len > 0) {
                pss->post_data = xrealloc(pss->post_data, pss->post_data_len + len + 1);
                memcpy(pss->post_data + pss->post_data_len, in, len);
                pss->post_data_len += len;
                pss->post_data[pss->post_data_len] = '\0'; // Null-terminate for string operations
            }
        }
        break;

    case LWS_CALLBACK_HTTP_BODY_COMPLETION:
        if (strcmp(pss->path, "/api/command") == 0) {
            // POST body is completely received. Now process the command.
            // char *cmd_to_exec = NULL; // Old way
            char **cmd_argv = NULL;
            int cmd_argc = 0;

            if (pss->post_data != NULL) {
                json_object *jobj = json_tokener_parse(pss->post_data);
                if (jobj != NULL) {
                    json_object *j_command_arr;
                    if (json_object_object_get_ex(jobj, "command", &j_command_arr) &&
                        json_object_is_type(j_command_arr, json_type_array)) {

                        cmd_argc = json_object_array_length(j_command_arr);
                        if (cmd_argc > 0) {
                            cmd_argv = xmalloc(sizeof(char *) * (cmd_argc + 1));
                            for (int i = 0; i < cmd_argc; i++) {
                                json_object *j_arg = json_object_array_get_idx(j_command_arr, i);
                                if (json_object_is_type(j_arg, json_type_string)) {
                                    cmd_argv[i] = strdup(json_object_get_string(j_arg));
                                } else {
                                    // Arg is not a string, error out
                                    lwsl_err("Command argument is not a string at index %d\n", i);
                                    for (int k = 0; k < i; k++) free(cmd_argv[k]);
                                    free(cmd_argv);
                                    cmd_argv = NULL;
                                    cmd_argc = 0;
                                    break;
                                }
                            }
                            if (cmd_argv) cmd_argv[cmd_argc] = NULL; // Null-terminate the array
                        }
                    }
                    json_object_put(jobj); // free jobj
                }
            }

            if (cmd_argv == NULL || cmd_argc == 0) {
                const char *err_msg = "{\"error\": \"Invalid or missing command array in JSON payload. Expected: {\\\"command\\\": [\\\"cmd\\\", \\\"arg1\\\"]}\"}";
                p = buffer + LWS_PRE; // Re-init p and end for this scope
                end = p + sizeof(buffer) - LWS_PRE;
                size_t n_err = strlen(err_msg);
                if (lws_add_http_header_status(wsi, HTTP_STATUS_BAD_REQUEST, &p, end) ||
                    lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
                    lws_add_http_header_content_length(wsi, (unsigned long)n_err, &p, end) ||
                    lws_finalize_http_header(wsi, &p, end) ||
                    lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0) {
                     if (pss->post_data) free(pss->post_data);
                     return 1;
                }
                pss->buffer = pss->ptr = strdup(err_msg);
                pss->len = n_err;
                lws_callback_on_writable(wsi);
                if (pss->post_data) free(pss->post_data);
                return 0;
            }

            struct command_result cmd_res = execute_command(cmd_argv);

            // Free cmd_argv and its contents
            if (cmd_argv) {
                for (int i = 0; i < cmd_argc; i++) {
                    free(cmd_argv[i]);
                }
                free(cmd_argv);
            }

            json_object *json_resp = json_object_new_object();
            json_object_object_add(json_resp, "stdout", json_object_new_string(cmd_res.stdout_str ? cmd_res.stdout_str : ""));
            json_object_object_add(json_resp, "stderr", json_object_new_string(cmd_res.stderr_str ? cmd_res.stderr_str : ""));
            json_object_object_add(json_resp, "exit_code", json_object_new_int(cmd_res.exit_code));

            const char *response_str = json_object_to_json_string_ext(json_resp, JSON_C_TO_STRING_PLAIN);
            size_t n_resp = strlen(response_str);

            p = buffer + LWS_PRE; // Re-init p and end
            end = p + sizeof(buffer) - LWS_PRE;

            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
                lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, (unsigned char *)"application/json;charset=utf-8", 30, &p, end) ||
                lws_add_http_header_content_length(wsi, (unsigned long)n_resp, &p, end) ||
                lws_finalize_http_header(wsi, &p, end) ||
                lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0) {
                // cmd_argv is already freed at this point if it was allocated
                if (cmd_res.stdout_str) free(cmd_res.stdout_str);
                if (cmd_res.stderr_str) free(cmd_res.stderr_str);
                json_object_put(json_resp);
                if (pss->post_data) free(pss->post_data);
                return 1;
            }
            pss->buffer = pss->ptr = strdup(response_str); // strdup because response_str is from json_object_to_json_string_ext
            pss->len = n_resp;
            lws_callback_on_writable(wsi);

            // cmd_argv is already freed
            if (cmd_res.stdout_str) free(cmd_res.stdout_str);
            if (cmd_res.stderr_str) free(cmd_res.stderr_str);
            json_object_put(json_resp); // free json_resp and associated string from json_object_to_json_string_ext
            if (pss->post_data) {
                free(pss->post_data);
                pss->post_data = NULL;
                pss->post_data_len = 0;
            }
        }
        break;

    default:
      break;
  }

  return 0;

  /* if we're on HTTP1.1 or 2.0, will keep the idle connection alive */
try_to_reuse:
  if (lws_http_transaction_completed(wsi)) return -1;

  return 0;
}
