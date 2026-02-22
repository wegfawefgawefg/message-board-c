#include "db.h"

#include "db_tags.h"
#include "logging.h"
#include "util.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3 *db;

static int table_has_column(const char *column_name)
{
    sqlite3_stmt *stmt = NULL;
    int found = 0;

    if (sqlite3_prepare_v2(db, "PRAGMA table_info(messages)", -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if (name != NULL && strcmp(name, column_name) == 0) {
            found = 1;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return found;
}

static int exec_sql(const char *sql)
{
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        log_error("SQL error: %s", err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}

int db_init(void)
{
    log_info("Initializing database");

    if (sqlite3_open("messages.db", &db) != SQLITE_OK) {
        log_error("Cannot open database: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS messages("
        "content TEXT,"
        "timestamp TEXT,"
        "nickname TEXT DEFAULT 'anon',"
        "client_id TEXT DEFAULT 'legacy',"
        "user_tag INTEGER DEFAULT -1,"
        "created_at INTEGER DEFAULT (strftime('%s','now'))"
        ")";

    if (exec_sql(create_sql) != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (exec_sql("CREATE TABLE IF NOT EXISTS nickname_tags(nickname TEXT NOT NULL, client_id TEXT NOT NULL, tag INTEGER NOT NULL, UNIQUE(nickname, client_id), UNIQUE(nickname, tag))") != 0) {
        sqlite3_close(db);
        return -1;
    }

    if (!table_has_column("nickname") && exec_sql("ALTER TABLE messages ADD COLUMN nickname TEXT DEFAULT 'anon'") != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (!table_has_column("client_id") && exec_sql("ALTER TABLE messages ADD COLUMN client_id TEXT DEFAULT 'legacy'") != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (!table_has_column("user_tag") && exec_sql("ALTER TABLE messages ADD COLUMN user_tag INTEGER DEFAULT -1") != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (!table_has_column("created_at") && exec_sql("ALTER TABLE messages ADD COLUMN created_at INTEGER DEFAULT 0") != 0) {
        sqlite3_close(db);
        return -1;
    }

    if (exec_sql("UPDATE messages SET nickname='anon' WHERE nickname IS NULL OR nickname = ''") != 0 ||
        exec_sql("UPDATE messages SET client_id='legacy' WHERE client_id IS NULL OR client_id = ''") != 0 ||
        exec_sql("UPDATE messages SET created_at=strftime('%s','now') WHERE created_at IS NULL OR created_at = 0") != 0) {
        sqlite3_close(db);
        return -1;
    }

    if (db_tags_backfill(db) != 0) {
        sqlite3_close(db);
        return -1;
    }

    log_info("Database initialized successfully");
    return 0;
}

void db_close(void)
{
    if (db != NULL) {
        sqlite3_close(db);
        db = NULL;
    }
}

int db_insert_message(const char *nickname, const char *client_id, const char *content)
{
    int user_tag = -1;
    if (db_tags_get_or_assign(db, nickname, client_id, &user_tag) != 0) {
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO messages(content, timestamp, nickname, client_id, user_tag, created_at) "
        "VALUES(?, ?, ?, ?, ?, strftime('%s','now'))";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char timestamp[64];
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, timestamp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, nickname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, client_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, user_tag);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static char *json_escape(const char *src)
{
    struct Buffer out = {0};

    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
        int rc = 0;
        switch (*p) {
        case '\"':
            rc = buffer_append(&out, "\\\"");
            break;
        case '\\':
            rc = buffer_append(&out, "\\\\");
            break;
        case '\b':
            rc = buffer_append(&out, "\\b");
            break;
        case '\f':
            rc = buffer_append(&out, "\\f");
            break;
        case '\n':
            rc = buffer_append(&out, "\\n");
            break;
        case '\r':
            rc = buffer_append(&out, "\\r");
            break;
        case '\t':
            rc = buffer_append(&out, "\\t");
            break;
        default:
            if (*p < 0x20) {
                rc = buffer_appendf(&out, "\\u%04x", *p);
            } else {
                rc = buffer_appendf(&out, "%c", *p);
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

static unsigned int nickname_hue(const char *nickname)
{
    unsigned int hash = 5381u;
    for (const unsigned char *p = (const unsigned char *)nickname; *p != '\0'; ++p) {
        hash = ((hash << 5) + hash) ^ (unsigned int)(*p);
    }
    return hash % 360u;
}

char *db_render_messages_html(void)
{
    const char *sql =
        "SELECT m.nickname, m.content, m.timestamp, m.user_tag "
        "FROM ("
        "SELECT nickname, content, timestamp, user_tag, created_at "
        "FROM messages ORDER BY created_at DESC LIMIT 50"
        ") AS m "
        "ORDER BY m.created_at ASC";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return strdup("<li class=\"rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700 dark:border-red-900 dark:bg-red-950/40 dark:text-red-200\">Failed to load messages.</li>");
    }

    struct Buffer out = {0};
    if (buffer_append(&out, "") != 0) {
        sqlite3_finalize(stmt);
        return strdup("<li class=\"rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700 dark:border-red-900 dark:bg-red-950/40 dark:text-red-200\">Failed to render messages.</li>");
    }

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        row_count++;
        const char *nickname = (const char *)sqlite3_column_text(stmt, 0);
        const char *content = (const char *)sqlite3_column_text(stmt, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 2);
        int user_tag = sqlite3_column_int(stmt, 3);
        if (user_tag <= 0 || user_tag > 9999) {
            user_tag = 1;
        }
        unsigned int hue = nickname_hue(nickname ? nickname : "anon");

        char *nick_esc = html_escape(nickname ? nickname : "anon");
        char *content_esc = html_escape(content ? content : "");
        char *time_esc = html_escape(timestamp ? timestamp : "");

        if (nick_esc == NULL || content_esc == NULL || time_esc == NULL) {
            free(nick_esc);
            free(content_esc);
            free(time_esc);
            free(out.data);
            sqlite3_finalize(stmt);
            return strdup("<li class=\"rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700 dark:border-red-900 dark:bg-red-950/40 dark:text-red-200\">Failed to render messages.</li>");
        }

        int rc = buffer_appendf(
            &out,
            "<li class=\"msg-item mb-2 rounded-lg border border-slate-200 bg-white px-3 py-2 shadow-sm last:mb-0 dark:border-slate-700 dark:bg-slate-900\" style=\"border-left:4px solid hsl(%u 72%% 46%%)\">"
            "<div class=\"mb-1 grid grid-cols-[1fr_auto] items-center gap-x-2 text-xs\">"
            "<span class=\"font-semibold\" style=\"color:hsl(%u 75%% 30%%)\">%s</span>"
            "<span class=\"msg-tag rounded bg-slate-100 px-2 py-0.5 font-mono text-[11px] tracking-wide text-slate-700 dark:bg-slate-800 dark:text-slate-200\">#%04d</span>"
            "<span class=\"msg-time col-span-2 text-[11px] text-slate-500 dark:text-slate-400\">%s</span>"
            "</div>"
            "<div class=\"msg-content whitespace-pre-wrap break-words text-sm text-slate-800 dark:text-slate-200\">%s</div>"
            "</li>",
            hue,
            hue,
            nick_esc,
            user_tag,
            time_esc,
            content_esc);

        free(nick_esc);
        free(content_esc);
        free(time_esc);

        if (rc != 0) {
            free(out.data);
            sqlite3_finalize(stmt);
            return strdup("<li class=\"rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700 dark:border-red-900 dark:bg-red-950/40 dark:text-red-200\">Failed to render messages.</li>");
        }
    }

    sqlite3_finalize(stmt);

    if (row_count == 0) {
        free(out.data);
        return strdup("<li class=\"rounded-lg border border-dashed border-slate-300 bg-white px-3 py-4 text-center text-sm text-slate-500 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-300\">No messages yet.</li>");
    }

    return out.data;
}

char *db_render_messages_json(void)
{
    const char *sql =
        "SELECT m.nickname, m.content, m.timestamp, m.user_tag "
        "FROM ("
        "SELECT nickname, content, timestamp, user_tag, created_at "
        "FROM messages ORDER BY created_at DESC LIMIT 50"
        ") AS m "
        "ORDER BY m.created_at ASC";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return strdup("{\"error\":\"Failed to load messages\"}");
    }

    struct Buffer out = {0};
    if (buffer_append(&out, "[") != 0) {
        sqlite3_finalize(stmt);
        return strdup("[]");
    }

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *nickname = (const char *)sqlite3_column_text(stmt, 0);
        const char *content = (const char *)sqlite3_column_text(stmt, 1);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 2);
        int user_tag = sqlite3_column_int(stmt, 3);
        if (user_tag <= 0 || user_tag > 9999) {
            user_tag = 1;
        }

        char *nick_esc = json_escape(nickname ? nickname : "anon");
        char *content_esc = json_escape(content ? content : "");
        char *time_esc = json_escape(timestamp ? timestamp : "");
        if (nick_esc == NULL || content_esc == NULL || time_esc == NULL) {
            free(nick_esc);
            free(content_esc);
            free(time_esc);
            free(out.data);
            sqlite3_finalize(stmt);
            return strdup("[]");
        }

        int rc = 0;
        if (row_count > 0) {
            rc |= buffer_append(&out, ",");
        }
        rc |= buffer_appendf(
            &out,
            "{\"nickname\":\"%s\",\"tag\":%d,\"timestamp\":\"%s\",\"content\":\"%s\"}",
            nick_esc,
            user_tag,
            time_esc,
            content_esc);
        row_count++;

        free(nick_esc);
        free(content_esc);
        free(time_esc);

        if (rc != 0) {
            free(out.data);
            sqlite3_finalize(stmt);
            return strdup("[]");
        }
    }

    sqlite3_finalize(stmt);
    if (buffer_append(&out, "]") != 0) {
        free(out.data);
        return strdup("[]");
    }

    return out.data;
}
