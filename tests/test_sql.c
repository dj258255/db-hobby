#include "sql.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                       \
    do {                                       \
        if (cond) {                            \
            printf("  ok   %s\n", msg);        \
        } else {                               \
            printf("  FAIL %s\n", msg);        \
            failures++;                        \
        }                                      \
    } while (0)

int main(void) {
    char err[128];
    Statement st;

    /* CREATE */
    CHECK(sql_parse("CREATE TABLE users (id INT, name TEXT);", &st, err, sizeof(err)) == 0,
          "CREATE 파싱 성공");
    CHECK(st.type == STMT_CREATE, "타입 CREATE");
    CHECK(strcmp(st.create.table, "users") == 0, "테이블 users");
    CHECK(st.create.num_columns == 2, "컬럼 2개");
    CHECK(strcmp(st.create.columns[0].name, "id") == 0 && st.create.columns[0].type == COL_INT,
          "컬럼0 id INT");
    CHECK(strcmp(st.create.columns[1].name, "name") == 0 && st.create.columns[1].type == COL_TEXT,
          "컬럼1 name TEXT");

    /* INSERT */
    CHECK(sql_parse("INSERT INTO users VALUES (1, 'kim');", &st, err, sizeof(err)) == 0,
          "INSERT 파싱 성공");
    CHECK(st.type == STMT_INSERT && st.insert.num_values == 2, "INSERT 값 2개");
    CHECK(st.insert.values[0].type == VAL_INT && st.insert.values[0].int_val == 1, "값0 = 정수 1");
    CHECK(st.insert.values[1].type == VAL_TEXT && strcmp(st.insert.values[1].text_val, "kim") == 0,
          "값1 = 'kim'");

    /* SELECT (no WHERE) */
    CHECK(sql_parse("SELECT * FROM users", &st, err, sizeof(err)) == 0, "SELECT 파싱 성공");
    CHECK(st.type == STMT_SELECT && st.select.has_where == 0, "WHERE 없음");
    CHECK(strcmp(st.select.table, "users") == 0, "SELECT 테이블 users");

    /* SELECT ... WHERE int */
    CHECK(sql_parse("SELECT * FROM users WHERE id = 1;", &st, err, sizeof(err)) == 0,
          "SELECT WHERE 파싱 성공");
    CHECK(st.select.has_where == 1 && strcmp(st.select.where_col, "id") == 0, "WHERE col = id");
    CHECK(st.select.where_val.type == VAL_INT && st.select.where_val.int_val == 1, "WHERE 값 = 1");

    /* SELECT ... WHERE text (대소문자 키워드 섞기) */
    CHECK(sql_parse("select * from users where name = 'kim'", &st, err, sizeof(err)) == 0,
          "소문자 키워드도 파싱");
    CHECK(st.select.where_val.type == VAL_TEXT &&
              strcmp(st.select.where_val.text_val, "kim") == 0,
          "WHERE 값 = 'kim'");

    /* 오류 케이스 */
    CHECK(sql_parse("SELECT FROM users", &st, err, sizeof(err)) == -1, "잘못된 SELECT는 오류");
    CHECK(sql_parse("DROP TABLE x", &st, err, sizeof(err)) == -1, "미지원 문장은 오류");

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
