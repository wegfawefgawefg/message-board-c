#include "http.h"

#include "config.h"
#include "db.h"
#include "logging.h"
#include "render.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct ConnectionInfo {
    char *body;
    size_t body_len;
};

struct SseClient {
    unsigned long seen_version;
    char pending[128];
    size_t pending_len;
    size_t pending_off;
};

static pthread_mutex_t sse_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sse_cond = PTHREAD_COND_INITIALIZER;
static unsigned long sse_version = 0;

static int append_upload_data(struct ConnectionInfo *ci, const char *data, size_t size)
{
    char *new_body = realloc(ci->body, ci->body_len + size + 1);
    if (new_body == NULL) {
        return -1;
    }

    ci->body = new_body;
    memcpy(ci->body + ci->body_len, data, size);
    ci->body_len += size;
    ci->body[ci->body_len] = '\0';
    return 0;
}

static void sse_notify_message(void)
{
    pthread_mutex_lock(&sse_mutex);
    sse_version++;
    pthread_cond_broadcast(&sse_cond);
    pthread_mutex_unlock(&sse_mutex);
}

static int handle_get_home(struct MHD_Connection *connection)
{
    char *page = render_home_page();
    if (page == NULL) {
        return MHD_NO;
    }

    return queue_text_response(connection, MHD_HTTP_OK, "text/html; charset=utf-8", page);
}

static int handle_get_messages(struct MHD_Connection *connection)
{
    char *messages = db_render_messages_html();
    if (messages == NULL) {
        return MHD_NO;
    }

    return queue_text_response(connection, MHD_HTTP_OK, "text/html; charset=utf-8", messages);
}

static int handle_get_messages_json(struct MHD_Connection *connection)
{
    char *messages = db_render_messages_json();
    if (messages == NULL) {
        return MHD_NO;
    }

    return queue_text_response(connection, MHD_HTTP_OK, "application/json; charset=utf-8", messages);
}

static int handle_get_favicon(struct MHD_Connection *connection)
{
    char *body = strdup("");
    if (body == NULL) {
        return MHD_NO;
    }
    return queue_text_response(connection, MHD_HTTP_NO_CONTENT, "image/x-icon", body);
}

static void sse_free_callback(void *cls)
{
    free(cls);
}

static ssize_t sse_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;

    struct SseClient *client = (struct SseClient *)cls;
    if (client == NULL || max == 0) {
        return 0;
    }

    if (client->pending_off >= client->pending_len) {
        int has_update = 0;

        pthread_mutex_lock(&sse_mutex);
        unsigned long current = client->seen_version;

        if (sse_version != current) {
            has_update = 1;
            client->seen_version = sse_version;
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 15;

            int wait_rc = 0;
            while (sse_version == current && wait_rc != ETIMEDOUT) {
                wait_rc = pthread_cond_timedwait(&sse_cond, &sse_mutex, &ts);
            }

            if (sse_version != current) {
                has_update = 1;
                client->seen_version = sse_version;
            }
        }

        pthread_mutex_unlock(&sse_mutex);

        if (has_update) {
            client->pending_len = (size_t)snprintf(client->pending,
                                                   sizeof(client->pending),
                                                   "event: message\ndata: %lu\n\n",
                                                   client->seen_version);
        } else {
            client->pending_len = (size_t)snprintf(client->pending,
                                                   sizeof(client->pending),
                                                   ": ping\n\n");
        }
        client->pending_off = 0;
    }

    size_t remaining = client->pending_len - client->pending_off;
    size_t n = remaining < max ? remaining : max;
    memcpy(buf, client->pending + client->pending_off, n);
    client->pending_off += n;
    return (ssize_t)n;
}

static int handle_get_events(struct MHD_Connection *connection)
{
    struct SseClient *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return MHD_NO;
    }

    pthread_mutex_lock(&sse_mutex);
    client->seen_version = sse_version;
    pthread_mutex_unlock(&sse_mutex);
    client->pending_len = (size_t)snprintf(client->pending, sizeof(client->pending), ": connected\n\n");
    client->pending_off = 0;

    struct MHD_Response *response = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN,
        256,
        &sse_reader,
        client,
        &sse_free_callback);
    if (response == NULL) {
        free(client);
        return MHD_NO;
    }

    MHD_add_response_header(response, "Content-Type", "text/event-stream");
    MHD_add_response_header(response, "Cache-Control", "no-cache");
    MHD_add_response_header(response, "Connection", "keep-alive");
    MHD_add_response_header(response, "X-Accel-Buffering", "no");

    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

static char *read_file_to_string(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }

    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    size_t read_n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read_n != (size_t)size) {
        free(buf);
        return NULL;
    }

    buf[size] = '\0';
    return buf;
}

static int handle_get_asset(struct MHD_Connection *connection, const char *url)
{
    const char *path = NULL;
    const char *content_type = NULL;

    if (strcmp(url, "/assets/styles.css") == 0) {
        path = "assets/styles.css";
        content_type = "text/css; charset=utf-8";
    } else if (strcmp(url, "/assets/app.js") == 0) {
        path = "assets/app.js";
        content_type = "application/javascript; charset=utf-8";
    } else {
        return MHD_NO;
    }

    char *body = read_file_to_string(path);
    if (body == NULL) {
        return MHD_NO;
    }

    return queue_text_response(connection, MHD_HTTP_OK, content_type, body);
}

static int handle_post_submit(struct MHD_Connection *connection, const struct ConnectionInfo *ci)
{
    char nickname[MAX_NICKNAME] = {0};
    char client_id[MAX_CLIENT_ID] = {0};
    char message[MAX_MESSAGE] = {0};
    char ajax[8] = {0};

    if (!form_get_value(ci->body ? ci->body : "", "nickname", nickname, sizeof(nickname)) ||
        !form_get_value(ci->body ? ci->body : "", "client_id", client_id, sizeof(client_id)) ||
        !form_get_value(ci->body ? ci->body : "", "message", message, sizeof(message))) {
        char *body = strdup("Bad request");
        if (body == NULL) {
            return MHD_NO;
        }
        return queue_text_response(connection, MHD_HTTP_BAD_REQUEST, "text/plain; charset=utf-8", body);
    }

    form_get_value(ci->body ? ci->body : "", "ajax", ajax, sizeof(ajax));

    if (nickname[0] == '\0' || client_id[0] == '\0' || message[0] == '\0') {
        char *body = strdup("Missing nickname, client_id, or message");
        if (body == NULL) {
            return MHD_NO;
        }
        return queue_text_response(connection, MHD_HTTP_BAD_REQUEST, "text/plain; charset=utf-8", body);
    }

    if (db_insert_message(nickname, client_id, message) != 0) {
        log_error("Failed inserting message");
        char *body = strdup("Failed to save message");
        if (body == NULL) {
            return MHD_NO;
        }
        return queue_text_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "text/plain; charset=utf-8", body);
    }

    sse_notify_message();

    log_info("POST /post\tuser=%s\tclient=%s\tlen=%zu", nickname, client_id, strlen(message));

    if (strcmp(ajax, "1") == 0) {
        char *body = strdup("{\"ok\":true}");
        if (body == NULL) {
            return MHD_NO;
        }
        return queue_text_response(connection, MHD_HTTP_OK, "application/json; charset=utf-8", body);
    }

    return queue_redirect_response(connection, "/");
}

enum MHD_Result answer_to_connection(void *cls,
                                     struct MHD_Connection *connection,
                                     const char *url,
                                     const char *method,
                                     const char *version,
                                     const char *upload_data,
                                     size_t *upload_data_size,
                                     void **con_cls)
{
    (void)cls;
    (void)version;

    if (*con_cls == NULL) {
        struct ConnectionInfo *ci = calloc(1, sizeof(*ci));
        if (ci == NULL) {
            return MHD_NO;
        }
        *con_cls = ci;
        return MHD_YES;
    }

    struct ConnectionInfo *ci = (struct ConnectionInfo *)*con_cls;

    if (strcmp(method, "POST") == 0) {
        if (*upload_data_size != 0) {
            if (append_upload_data(ci, upload_data, *upload_data_size) != 0) {
                free(ci->body);
                free(ci);
                *con_cls = NULL;
                return MHD_NO;
            }
            *upload_data_size = 0;
            return MHD_YES;
        }

        int ret = MHD_NO;
        if (strcmp(url, "/post") == 0) {
            ret = handle_post_submit(connection, ci);
        } else {
            char *body = strdup("Not found");
            if (body != NULL) {
                ret = queue_text_response(connection, MHD_HTTP_NOT_FOUND, "text/plain; charset=utf-8", body);
            }
        }

        free(ci->body);
        free(ci);
        *con_cls = NULL;
        return ret;
    }

    int ret = MHD_NO;
    if (strcmp(method, "GET") == 0 && strcmp(url, "/") == 0) {
        ret = handle_get_home(connection);
        log_info("GET /\t200");
    } else if (strcmp(method, "GET") == 0 && strcmp(url, "/events") == 0) {
        ret = handle_get_events(connection);
        log_info("GET /events\t%s", ret == MHD_NO ? "500" : "200");
    } else if (strcmp(method, "GET") == 0 && strcmp(url, "/messages") == 0) {
        ret = handle_get_messages(connection);
        log_info("GET /messages\t200");
    } else if (strcmp(method, "GET") == 0 && strcmp(url, "/messages.json") == 0) {
        ret = handle_get_messages_json(connection);
        log_info("GET /messages.json\t200");
    } else if (strcmp(method, "GET") == 0 && strcmp(url, "/favicon.ico") == 0) {
        ret = handle_get_favicon(connection);
        log_info("GET /favicon.ico\t204");
    } else if (strcmp(method, "GET") == 0 && strncmp(url, "/assets/", 8) == 0) {
        ret = handle_get_asset(connection, url);
        if (ret == MHD_NO) {
            char *body = strdup("Not found");
            if (body != NULL) {
                ret = queue_text_response(connection, MHD_HTTP_NOT_FOUND, "text/plain; charset=utf-8", body);
            }
        }
        log_info("GET %s\t%s", url, ret == MHD_NO ? "404" : "200");
    } else {
        char *body = strdup("Not found");
        if (body != NULL) {
            ret = queue_text_response(connection, MHD_HTTP_NOT_FOUND, "text/plain; charset=utf-8", body);
        }
        log_info("%s %s\t404", method, url);
    }

    free(ci->body);
    free(ci);
    *con_cls = NULL;
    return ret;
}
