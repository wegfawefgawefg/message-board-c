#ifndef DB_H
#define DB_H

int db_init(void);
void db_close(void);
int db_insert_message(const char *nickname, const char *client_id, const char *content);
char *db_render_messages_html(void);
char *db_render_messages_json(void);

#endif
