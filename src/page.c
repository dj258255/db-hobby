#include "page.h"

#include <string.h>

/* 페이지 맨 앞에 얹히는 헤더. (PostgreSQL의 PageHeaderData와 같은 발상 — 페이지
 * 버퍼에 구조체를 그대로 덮어 해석한다.) */
typedef struct {
    uint16_t num_slots; /* 슬롯 개수(삭제된 것 포함) */
    uint16_t free_end;  /* 레코드 영역의 시작 offset. 초기값 PAGE_SIZE, 레코드가 쌓일수록 줄어든다. */
} SlotPageHeader;

/* 슬롯 하나: 레코드가 페이지 내 어디에 얼마나 있는지. offset 0 = 삭제된 빈 슬롯. */
typedef struct {
    uint16_t offset;
    uint16_t length;
} Slot;

#define HEADER_SIZE ((uint16_t)sizeof(SlotPageHeader))
#define SLOT_SIZE ((uint16_t)sizeof(Slot))

static SlotPageHeader *header(void *page) {
    return (SlotPageHeader *)page;
}

static const SlotPageHeader *header_const(const void *page) {
    return (const SlotPageHeader *)page;
}

/* i번 슬롯의 위치. 슬롯 배열은 헤더 바로 뒤에서 시작해 아래로 자란다. */
static Slot *slot_at(void *page, uint16_t i) {
    return (Slot *)((uint8_t *)page + HEADER_SIZE + (size_t)i * SLOT_SIZE);
}

static const Slot *slot_at_const(const void *page, uint16_t i) {
    return (const Slot *)((const uint8_t *)page + HEADER_SIZE + (size_t)i * SLOT_SIZE);
}

/* 슬롯 배열이 끝나는 곳 = 다음 슬롯이 들어갈 자리 = 빈 공간의 시작. */
static uint16_t free_start(const SlotPageHeader *h) {
    return HEADER_SIZE + h->num_slots * SLOT_SIZE;
}

void slotpage_init(void *page) {
    SlotPageHeader *h = header(page);
    h->num_slots = 0;
    h->free_end = PAGE_SIZE;
}

uint16_t slotpage_free_space(const void *page) {
    const SlotPageHeader *h = header_const(page);
    return h->free_end - free_start(h);
}

uint16_t slotpage_num_slots(const void *page) {
    return header_const(page)->num_slots;
}

int slotpage_insert(void *page, const void *rec, uint16_t len) {
    SlotPageHeader *h = header(page);
    /* 새 레코드 + 새 슬롯이 들어갈 공간이 있어야 한다. */
    uint16_t need = len + SLOT_SIZE;
    if (slotpage_free_space(page) < need) {
        return -1; /* 공간 부족 */
    }
    /* 레코드는 빈 공간의 끝(free_end)에서 len만큼 아래로 내려 쓴다. */
    uint16_t rec_off = h->free_end - len;
    memcpy((uint8_t *)page + rec_off, rec, len);
    h->free_end = rec_off;

    /* 새 슬롯을 슬롯 배열 끝에 추가. (삭제된 슬롯 재사용은 일단 안 한다 — 단순화.) */
    uint16_t idx = h->num_slots;
    Slot *s = slot_at(page, idx);
    s->offset = rec_off;
    s->length = len;
    h->num_slots++;
    return (int)idx;
}

int slotpage_get(const void *page, uint16_t slot, const void **rec_out, uint16_t *len_out) {
    const SlotPageHeader *h = header_const(page);
    if (slot >= h->num_slots) {
        return -1;
    }
    const Slot *s = slot_at_const(page, slot);
    if (s->offset == 0) {
        return -1; /* 삭제된 슬롯 */
    }
    *rec_out = (const uint8_t *)page + s->offset;
    *len_out = s->length;
    return 0;
}

int slotpage_delete(void *page, uint16_t slot) {
    SlotPageHeader *h = header(page);
    if (slot >= h->num_slots) {
        return -1;
    }
    /* offset 0으로 표시만 한다. 차지하던 공간은 회수하지 않는다 —
     * 진짜 DB는 나중에 compaction/VACUUM으로 정리한다. */
    Slot *s = slot_at(page, slot);
    s->offset = 0;
    s->length = 0;
    return 0;
}

int slotpage_overwrite(void *page, uint16_t slot, const void *rec, uint16_t len) {
    SlotPageHeader *h = header(page);
    if (slot >= h->num_slots) {
        return -1;
    }
    Slot *s = slot_at(page, slot);
    if (s->offset == 0 || s->length != len) {
        return -1; /* 삭제된 슬롯이거나 길이가 달라 이웃을 침범할 위험 */
    }
    memcpy((uint8_t *)page + s->offset, rec, len);
    return 0;
}
