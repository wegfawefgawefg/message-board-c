#include "db_tags.h"

#include "config.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int fnv1a_32(const char *s)
{
    unsigned int hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
        hash ^= (unsigned int)(*p);
        hash *= 16777619u;
    }
    return hash;
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
            int hi = (src[1] >= '0' && src[1] <= '9') ? (src[1] - '0')
                     : (src[1] >= 'a' && src[1] <= 'f') ? (src[1] - 'a' + 10)
                     : (src[1] >= 'A' && src[1] <= 'F') ? (src[1] - 'A' + 10)
                     : -1;
            int lo = (src[2] >= '0' && src[2] <= '9') ? (src[2] - '0')
                     : (src[2] >= 'a' && src[2] <= 'f') ? (src[2] - 'a' + 10)
                     : (src[2] >= 'A' && src[2] <= 'F') ? (src[2] - 'A' + 10)
                     : -1;
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

static int parse_form_value(const char *form_body, const char *key, char *out, size_t out_size)
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

int db_tags_get_or_assign(sqlite3 *db, const char *nickname, const char *client_id, int *out_tag)
{
    sqlite3_stmt *stmt = NULL;
    int had_invalid_row = 0;

    const char *select_sql =
        "SELECT tag FROM nickname_tags WHERE nickname = ? AND client_id = ? LIMIT 1";
    if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, nickname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, client_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int existing_tag = sqlite3_column_int(stmt, 0);
        if (existing_tag > 0 && existing_tag <= 9999) {
            *out_tag = existing_tag;
            sqlite3_finalize(stmt);
            return 0;
        }
        had_invalid_row = 1;
    }
    sqlite3_finalize(stmt);

    const char *insert_sql =
        "INSERT INTO nickname_tags(nickname, client_id, tag) VALUES(?, ?, ?)";
    const char *delete_sql =
        "DELETE FROM nickname_tags WHERE nickname = ? AND client_id = ?";

    if (had_invalid_row) {
        if (sqlite3_prepare_v2(db, delete_sql, -1, &stmt, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_text(stmt, 1, nickname, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, client_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    int start_tag = (int)(fnv1a_32(client_id) % 9999u) + 1;
    for (int offset = 0; offset < 9999; ++offset) {
        int tag = ((start_tag - 1 + offset) % 9999) + 1;
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_text(stmt, 1, nickname, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, client_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, tag);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc == SQLITE_DONE) {
            *out_tag = tag;
            return 0;
        }

        if (rc != SQLITE_CONSTRAINT) {
            return -1;
        }

        if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_text(stmt, 1, nickname, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, client_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int existing_tag = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            if (existing_tag > 0 && existing_tag <= 9999) {
                *out_tag = existing_tag;
                return 0;
            }
            continue;
        }
        sqlite3_finalize(stmt);
    }

    return -1;
}

int db_tags_backfill(sqlite3 *db)
{
    sqlite3_stmt *select_stmt = NULL;
    sqlite3_stmt *update_stmt = NULL;

    const char *select_sql =
        "SELECT DISTINCT nickname, client_id, content FROM messages "
        "WHERE nickname IS NOT NULL AND nickname != '' "
        "AND client_id IS NOT NULL AND client_id != ''";
    const char *update_sql =
        "UPDATE messages SET nickname = ?, client_id = ?, user_tag = ?, content = ? "
        "WHERE rowid = ?";

    const char *select_rows_sql =
        "SELECT rowid, nickname, client_id, user_tag, content FROM messages";

    if (sqlite3_prepare_v2(db, select_sql, -1, &select_stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    while (sqlite3_step(select_stmt) == SQLITE_ROW) {
        const char *nickname = (const char *)sqlite3_column_text(select_stmt, 0);
        const char *client_id = (const char *)sqlite3_column_text(select_stmt, 1);
        if (nickname == NULL || client_id == NULL) {
            continue;
        }
        int tag = -1;
        if (db_tags_get_or_assign(db, nickname, client_id, &tag) != 0) {
            continue;
        }

        sqlite3_stmt *u = NULL;
        if (sqlite3_prepare_v2(db,
                               "UPDATE messages SET user_tag = ? WHERE nickname = ? AND client_id = ? AND (user_tag IS NULL OR user_tag <= 0)",
                               -1,
                               &u,
                               NULL) == SQLITE_OK) {
            sqlite3_bind_int(u, 1, tag);
            sqlite3_bind_text(u, 2, nickname, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(u, 3, client_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
    }
    sqlite3_finalize(select_stmt);

    if (sqlite3_exec(db, "DELETE FROM nickname_tags", NULL, NULL, NULL) != SQLITE_OK) {
        return -1;
    }

    if (sqlite3_prepare_v2(db,
                           "SELECT DISTINCT nickname, client_id FROM messages "
                           "WHERE nickname IS NOT NULL AND nickname != '' "
                           "AND client_id IS NOT NULL AND client_id != '' "
                           "ORDER BY nickname, client_id",
                           -1,
                           &select_stmt,
                           NULL) != SQLITE_OK) {
        return -1;
    }

    while (sqlite3_step(select_stmt) == SQLITE_ROW) {
        const char *nickname = (const char *)sqlite3_column_text(select_stmt, 0);
        const char *client_id = (const char *)sqlite3_column_text(select_stmt, 1);
        if (nickname == NULL || client_id == NULL) {
            continue;
        }

        int tag = -1;
        if (db_tags_get_or_assign(db, nickname, client_id, &tag) != 0) {
            sqlite3_finalize(select_stmt);
            return -1;
        }
    }
    sqlite3_finalize(select_stmt);

    if (sqlite3_exec(db,
                     "UPDATE messages "
                     "SET user_tag = COALESCE(("
                     "  SELECT t.tag FROM nickname_tags t "
                     "  WHERE t.nickname = messages.nickname AND t.client_id = messages.client_id"
                     "), 1)",
                     NULL,
                     NULL,
                     NULL) != SQLITE_OK) {
        return -1;
    }

    if (sqlite3_prepare_v2(db, select_rows_sql, -1, &select_stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(select_stmt);
        return 0;
    }

    while (sqlite3_step(select_stmt) == SQLITE_ROW) {
        sqlite3_int64 rowid = sqlite3_column_int64(select_stmt, 0);
        const char *nickname = (const char *)sqlite3_column_text(select_stmt, 1);
        const char *client_id = (const char *)sqlite3_column_text(select_stmt, 2);
        int user_tag = sqlite3_column_int(select_stmt, 3);
        const char *content = (const char *)sqlite3_column_text(select_stmt, 4);

        char nick_buf[MAX_NICKNAME] = {0};
        char cid_buf[MAX_CLIENT_ID] = {0};
        char msg_buf[MAX_MESSAGE] = {0};

        snprintf(nick_buf, sizeof(nick_buf), "%s", nickname ? nickname : "anon");
        snprintf(cid_buf, sizeof(cid_buf), "%s", client_id ? client_id : "legacy");
        snprintf(msg_buf, sizeof(msg_buf), "%s", content ? content : "");

        if (strstr(msg_buf, "&client_id=") != NULL && strstr(msg_buf, "&message=") != NULL) {
            parse_form_value(msg_buf, "nickname", nick_buf, sizeof(nick_buf));
            if (nick_buf[0] == '\0') {
                snprintf(nick_buf, sizeof(nick_buf), "anon");
            }
            parse_form_value(msg_buf, "client_id", cid_buf, sizeof(cid_buf));
            if (cid_buf[0] == '\0') {
                snprintf(cid_buf, sizeof(cid_buf), "legacy");
            }
            parse_form_value(content ? content : "", "message", msg_buf, sizeof(msg_buf));
        }

        if (user_tag <= 0 || user_tag > 9999) {
            if (db_tags_get_or_assign(db, nick_buf, cid_buf, &user_tag) != 0) {
                user_tag = 1;
            }
        }

        sqlite3_reset(update_stmt);
        sqlite3_clear_bindings(update_stmt);
        sqlite3_bind_text(update_stmt, 1, nick_buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update_stmt, 2, cid_buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update_stmt, 3, user_tag);
        sqlite3_bind_text(update_stmt, 4, msg_buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update_stmt, 5, rowid);
        sqlite3_step(update_stmt);
    }

    sqlite3_finalize(update_stmt);
    sqlite3_finalize(select_stmt);
    return 0;
}
