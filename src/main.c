#include "db.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* db-hobby — REPL 또는 PostgreSQL wire protocol 서버.
 *   REPL:   ./build/db-hobby mydata.db
 *   서버:   ./build/db-hobby mydata.db --serve [port]   (기본 5433)
 *           -> psql "host=127.0.0.1 port=5433 dbname=db-hobby"
 */
int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "db-hobby.db";

    /* --serve [port] 가 있으면 서버 모드 */
    int serve = 0, port = 5433;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--serve") == 0) {
            serve = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                port = atoi(argv[i + 1]);
            }
        }
    }

    Database db;
    if (db_open(&db, path) != 0) {
        fprintf(stderr, "DB 열기 실패: %s\n", path);
        return 1;
    }

    if (serve) {
        int rc = server_run(&db, port);
        if (rc != 0) {
            fprintf(stderr, "서버 시작 실패 (포트 %d 사용 중?)\n", port);
        }
        db_close(&db);
        return rc == 0 ? 0 : 1;
    }

    printf("db-hobby — SQL을 입력하세요. (Ctrl-D로 종료)\n");
    char line[2048];
    while (1) {
        printf("db-hobby> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') {
            line[--n] = '\0';
        }
        if (n == 0) {
            continue;
        }
        db_exec(&db, line, stdout);
    }

    db_close(&db);
    printf("\n안녕히.\n");
    return 0;
}
