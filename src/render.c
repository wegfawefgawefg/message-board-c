#include "render.h"

#include "db.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEMPLATE_MARKER "{{MESSAGES}}"

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

static char *load_page_template(void)
{
    char *tmpl = read_file_to_string("assets/index.html");
    if (tmpl != NULL) {
        return tmpl;
    }

    return read_file_to_string("build/assets/index.html");
}

char *render_home_page(void)
{
    char *messages = db_render_messages_html();
    if (messages == NULL) {
        return NULL;
    }

    char *tmpl = load_page_template();
    if (tmpl == NULL) {
        free(messages);
        return NULL;
    }

    char *marker = strstr(tmpl, TEMPLATE_MARKER);
    if (marker == NULL) {
        free(messages);
        free(tmpl);
        return NULL;
    }

    size_t before_len = (size_t)(marker - tmpl);
    size_t marker_len = strlen(TEMPLATE_MARKER);

    struct Buffer out = {0};
    int rc = 0;

    char saved = tmpl[before_len];
    tmpl[before_len] = '\0';
    rc |= buffer_append(&out, tmpl);
    tmpl[before_len] = saved;

    rc |= buffer_append(&out, messages);
    rc |= buffer_append(&out, marker + marker_len);

    free(messages);
    free(tmpl);

    if (rc != 0) {
        free(out.data);
        return NULL;
    }

    return out.data;
}
