#ifndef MINIDB_SERVER_H
#define MINIDB_SERVER_H

#include "db.h"

/*
 * PostgreSQL wire protocol(v3) 서버 — 진짜 psql이 db-hobby에 붙는다.
 *
 * 구조: 단일 스레드 poll() 이벤트 루프. 스레드가 없으므로 진짜 병렬은 아니지만,
 * 커넥션마다 18편의 세션을 하나씩 배정해(커넥션 = 세션) 여러 클라이언트의
 * 트랜잭션이 인터리브된다 — psql 두 개로 "reader가 writer를 안 막는다"를
 * 네트워크 너머에서 시연할 수 있다.
 *
 * 지원: startup(3.0)·SSLRequest 거절('N')·simple query('Q')·Terminate('X'),
 * RowDescription/DataRow(전 컬럼 TEXT)·CommandComplete·ErrorResponse·Notice.
 * extended query(Parse/Bind)는 없다 — psql의 기본 대화는 simple query라 충분.
 * SELECT 결과는 실행기의 텍스트 출력(헤더/행/"(N행)" 꼬리)을 파싱해 행으로 보낸다
 * — TEXT 값에 " | "가 들어가면 컬럼이 갈라지는 것이 알려진 한계.
 */

/* 127.0.0.1:port로 서버를 연다. 커넥션당 세션 배정, 끊기면 열린 트랜잭션 롤백.
 * 정상 종료 없음(시그널로 끝냄). 리슨 소켓을 못 열면 -1. */
int server_run(Database *db, int port);

#endif /* MINIDB_SERVER_H */
