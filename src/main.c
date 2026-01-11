#include "db.h"

#include <stdio.h>
#include <string.h>

/* minidb REPL — SQL을 한 줄씩 받아 실행한다.
 *   빌드/실행:  make repl && ./build/minidb mydata.db
 */
int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "minidb.db";

    Database db;
    if (db_open(&db, path) != 0) {
        fprintf(stderr, "DB 열기 실패: %s\n", path);
        return 1;
    }

    printf("minidb — SQL을 입력하세요. (Ctrl-D로 종료)\n");
    char line[2048];
    while (1) {
        printf("minidb> ");
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
