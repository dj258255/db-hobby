/* replnet — WAL 로그 시핑을 소켓으로 (트랙 H1 후속: 네트워크 전송)
 *
 * 25편의 replica는 primary가 '같은 로컬 파일 시스템에 십(ship)해 둔' WAL 파일을
 * tail했다. 그래서 primary와 replica는 사실상 한 디스크를 공유해야 했다 — 진짜
 * 복제가 아니라 "파일 공유 흉내"였다. 이 모듈이 그 #1 프론티어("네트워크 전송
 * 없음")를 닫는다: primary의 WAL 바이트를 소켓으로 실어 나른다.
 *
 * 설계의 핵심 — replica.c를 한 줄도 안 고친다:
 *   walsender  : primary WAL 로그 파일에서 '새로 붙은' 바이트를 읽어 소켓으로 보냄.
 *   walreceiver: 소켓에서 받은 바이트를 replica의 '로컬' 로그 파일에 append.
 *   그 다음 replica는 기존 replica_apply를 그 로컬 로그에 그대로 돌린다.
 * 즉 네트워크 계층은 "바이트를 primary 로그 -> replica 로컬 로그로 옮기는 파이프"
 * 일 뿐이고, redo의 정확성(페이지 전체 물리 로깅 = idempotent, lsn 게이트)은
 * 전부 replica.c가 이미 책임진다. PostgreSQL의 walsender/walreceiver가 WAL을
 * 스트리밍하고 startup 프로세스가 replay하는 것과 같은 역할 분리다.
 *
 * 프레이밍: [길이 8B][그 길이만큼의 WAL 바이트]. 길이 헤더가 있어야 수신 측이
 * "한 청크가 어디서 끝나는지"를 안다(스트림엔 메시지 경계가 없으므로). 블로킹
 * 소켓 + 부분 read/write 루프(wal.c의 write_all/read_exact와 같은 규약)로 처리.
 *
 * 정직한 경계 (프론티어):
 *   - 동기 복제 아님. walsender는 보내기만 하고 replica의 apply ack를 기다려
 *     커밋을 막지 않는다(비동기, 최종 일관성). 동기 커밋(quorum ack)은 다음 일.
 *   - 복제 슬롯 없음. primary가 replica의 catch-up 전에 체크포인트로 로그를
 *     truncate하면 그 사이 구간을 놓칠 수 있다(replica.h와 같은 한계). sent_off는
 *     truncate로 파일이 되감기면 0으로 리셋해 정직하게 이어 붙이지만, 이미 잘려
 *     사라진 구간까지 되살리진 못한다.
 *   - 길이 헤더는 host 바이트 순서다(같은 호스트 socketpair/localhost 기준). 진짜
 *     이기종 TCP라면 htonll 같은 고정 바이트 순서가 필요하다 — 바이트 스트림
 *     자체는 동일하다(아래 send/recv는 어떤 SOCK_STREAM 위에서도 그대로 동작).
 *   - 인증·TLS·재연결·흐름 제어 없음. 학습용 최소 전송.
 */
#ifndef MINIDB_REPLNET_H
#define MINIDB_REPLNET_H

#include <sys/types.h>

/*
 * walsender: primary WAL 로그(log_fd)에서 *sent_off ~ EOF의 새 바이트를 읽어
 * [길이][바이트] 한 프레임으로 sock에 보내고, *sent_off를 보낸 만큼 전진시킨다.
 *
 * 반환: 이번에 보낸 WAL 바이트 수(>0), 새 바이트가 없으면 0, 오류 시 -1.
 * 한 번에 보내는 양은 REPLNET_CHUNK_MAX로 제한 — 큰 백로그는 여러 번 호출로 흘려보낸다.
 * 체크포인트 truncate로 로그가 *sent_off보다 작아지면 *sent_off를 0으로 되감는다
 * (replica_apply의 되감기 처리와 대칭 — lsn 게이트가 중복 적용을 막는다).
 */
int walsender_send(int sock, int log_fd, off_t *sent_off);

/*
 * walreceiver: sock에서 한 프레임([길이][바이트])을 받아 local_log_fd 끝에 append.
 * 반환: append한 바이트 수(>0), 깨끗한 EOF(peer가 프레임 경계에서 닫음)면 0, 오류 -1.
 * append 후 replica_apply를 부르는 건 호출자 몫(전송과 적용의 관심사 분리).
 */
int walreceiver_recv(int sock, int local_log_fd);

/* 한 프레임에 실어 나르는 WAL 바이트 상한(백로그가 크면 프레임을 쪼갠다). */
#define REPLNET_CHUNK_MAX (1u << 20) /* 1MiB */

#endif /* MINIDB_REPLNET_H */
