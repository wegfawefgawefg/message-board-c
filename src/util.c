#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int buffer_ensure(struct Buffer *b, size_t extra)
{
    size_t needed = b->len + extra + 1;
    if (needed <= b->cap) {
        return 0;
    }

    size_t new_cap = b->cap == 0 ? BUFFER_INITIAL_CAPACITY : b->cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    char *new_data = realloc(b->data, new_cap);
    if (new_data == NULL) {
        return -1;
    }

    b->data = new_data;
    b->cap = new_cap;
    return 0;
}

int buffer_append(struct Buffer *b, const char *s)
{
    size_t n = strlen(s);
    if (buffer_ensure(b, n) != 0) {
        return -1;
    }

    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int buffer_appendf(struct Buffer *b, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        return -1;
    }

    if (buffer_ensure(b, (size_t)needed) != 0) {
        va_end(args);
        return -1;
    }

    vsnprintf(b->data + b->len, b->cap - b->len, fmt, args);
    b->len += (size_t)needed;
    va_end(args);
    return 0;
}

char *html_escape(const char *src)
{
    struct Buffer out = {0};

    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
        int rc = 0;
        switch (*p) {
        case '&':
            rc = buffer_append(&out, "&amp;");
            break;
        case '<':
            rc = buffer_append(&out, "&lt;");
            break;
        case '>':
            rc = buffer_append(&out, "&gt;");
            break;
        case '"':
            rc = buffer_append(&out, "&quot;");
            break;
        default:
            rc = buffer_ensure(&out, 1);
            if (rc == 0) {
                out.data[out.len++] = (char)*p;
                out.data[out.len] = '\0';
            }
            break;
        }

        if (rc != 0) {
            free(out.data);
            return NULL;
        }
    }

    if (out.data == NULL) {
        out.data = strdup("");
    }
    return out.data;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode_inplace(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}

int form_get_value(const char *form_body, const char *key, char *out, size_t out_size)
{
    if (out_size == 0) {
        return 0;
    }

    char *copy = strdup(form_body);
    if (copy == NULL) {
        return 0;
    }

    int found = 0;
    char *saveptr = NULL;
    for (char *token = strtok_r(copy, "&", &saveptr); token != NULL; token = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(token, '=');
        if (eq == NULL) {
            continue;
        }

        *eq = '\0';
        char *k = token;
        char *v = eq + 1;

        url_decode_inplace(k);
        url_decode_inplace(v);

        if (strcmp(k, key) == 0) {
            snprintf(out, out_size, "%s", v);
            found = 1;
            break;
        }
    }

    free(copy);
    return found;
}

int queue_text_response(struct MHD_Connection *connection, unsigned int status, const char *content_type, char *body)
{
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(body), body, MHD_RESPMEM_MUST_FREE);
    if (response == NULL) {
        free(body);
        return MHD_NO;
    }

    MHD_add_response_header(response, "Content-Type", content_type);
    int ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

int queue_redirect_response(struct MHD_Connection *connection, const char *location)
{
    const char *body = "<html><body>Redirecting...</body></html>";
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(body), (void *)body, MHD_RESPMEM_PERSISTENT);
    if (response == NULL) {
        return MHD_NO;
    }

    MHD_add_response_header(response, "Location", location);
    int ret = MHD_queue_response(connection, MHD_HTTP_SEE_OTHER, response);
    MHD_destroy_response(response);
    return ret;
}
