#include "config.h"
#include "db.h"
#include "http.h"
#include "logging.h"

#include <microhttpd.h>
#include <stdio.h>

int main(void)
{
    log_info("Program started");

    if (db_init() != 0) {
        log_error("Database initialization failed");
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION,
                                                 PORT,
                                                 NULL,
                                                 NULL,
                                                 &answer_to_connection,
                                                 NULL,
                                                 MHD_OPTION_END);
    if (daemon == NULL) {
        log_error("Failed to start MHD daemon");
        db_close();
        return 1;
    }

    log_info("MHD daemon started successfully");
    log_info("Server running on port %d. Press enter to stop.", PORT);
    printf("Open in browser: http://127.0.0.1:%d/\n", PORT);
    getchar();

    log_info("Stopping MHD daemon");
    MHD_stop_daemon(daemon);

    log_info("Closing database");
    db_close();

    log_info("Program ending");
    return 0;
}
