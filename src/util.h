#ifndef UTIL_H
#define UTIL_H

#include <microhttpd.h>
#include <stddef.h>

enum { BUFFER_INITIAL_CAPACITY = 1024 };

struct Buffer {
    char *data;
    size_t len;
    size_t cap;
};

int buffer_append(struct Buffer *b, const char *s);
int buffer_appendf(struct Buffer *b, const char *fmt, ...);
char *html_escape(const char *src);
int form_get_value(const char *form_body, const char *key, char *out, size_t out_size);
int queue_text_response(struct MHD_Connection *connection, unsigned int status, const char *content_type, char *body);
int queue_redirect_response(struct MHD_Connection *connection, const char *location);

#endif
