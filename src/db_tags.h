#ifndef DB_TAGS_H
#define DB_TAGS_H

#include <sqlite3.h>

int db_tags_get_or_assign(sqlite3 *db, const char *nickname, const char *client_id, int *out_tag);
int db_tags_backfill(sqlite3 *db);

#endif
