/** Drive the real worker, callbacks and async replies with reusable LwESP objects. */
#include "firmware_tcp.h"
#include "download_stream.h"
#include "spi.h"
#include "task.h"
#include "queue.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PICTURES, RECONNECT, INTERRUPTED, STALE, SECOND, INFO, OLD_REPLY, OLD_SEND,
       PREPARE_FAILURE, BAD_HEADER, WRITE_FAILURE, TIMEOUT, OVERFLOW, COPY_FAILURE,
       SEND_FAILURE, SEND_TIMEOUT, FW_ABORT_BLOCKED, FW_COMMITTED };
static const char *const names[] = {
    "pictures", "reconnect", "interrupted", "stale", "second", "info", "old_reply", "old_send",
    "prepare_failure", "bad_header", "write_failure", "timeout", "overflow", "copy_failure",
    "send_failure", "send_timeout", "fw_abort_blocked", "fw_committed"
};
typedef struct { void *arg; unsigned active, closes; } connection_t;
static connection_t connections[2];
static TaskFunction_t entry;
static uint8_t items[8][512], old_item[512], wire[2304];
static unsigned item_size, head, count, scenario, stage, attempts, offset, steps;
static unsigned ready, ack, done, shown, consumed, errors, aborted, begins, writes, finishes;
static unsigned queries, locks, tasks, queues, info_part, injected, pending, notifications, tx_waits;
static unsigned firmware_begins, firmware_finishes, firmware_aborts, committed_mask;
static const char *tx_pointer;
static char tx_copy[48];
static size_t tx_length;
static lwesp_conn_p tx_conn;
static firmware_update_t *protected_update;
static TickType_t now;
static jmp_buf stop;

lwespr_t lwesp_core_lock(void) { ++locks; return lwespOK; }
lwespr_t lwesp_core_unlock(void) { assert(locks); --locks; return lwespOK; }
lwespr_t lwesp_conn_set_arg(lwesp_conn_p c, void *a) { ((connection_t *)c)->arg = a; return lwespOK; }
void *lwesp_conn_get_arg(lwesp_conn_p c) { return ((connection_t *)c)->arg; }
uint8_t lwesp_conn_is_active(lwesp_conn_p c) { return (uint8_t)((connection_t *)c)->active; }
lwesp_pbuf_p lwesp_evt_conn_recv_get_buff(lwesp_evt_t *e) { return e->buff; }
lwesp_conn_p lwesp_evt_conn_recv_get_conn(lwesp_evt_t *e) { return e->conn; }
lwesp_conn_p lwesp_evt_conn_active_get_conn(lwesp_evt_t *e) { return e->conn; }
lwesp_conn_p lwesp_evt_conn_close_get_conn(lwesp_evt_t *e) { return e->conn; }
lwesp_conn_p lwesp_evt_conn_send_get_conn(lwesp_evt_t *e) { return e->conn; }
lwespr_t lwesp_evt_conn_send_get_result(lwesp_evt_t *e)
{ (void)e; return scenario == SEND_FAILURE && attempts == 0 ? lwespERR : lwespOK; }
size_t lwesp_evt_conn_send_get_length(lwesp_evt_t *e) { (void)e; return tx_length; }
size_t lwesp_pbuf_length(lwesp_pbuf_p p, uint8_t chain) { assert(chain == 1); return p->length; }
size_t lwesp_pbuf_copy(lwesp_pbuf_p p, void *out, size_t n, size_t at)
{
    assert(at + n <= p->length);
    if (scenario == COPY_FAILURE && attempts == 0 && stage == 2) return n - 1;
    memcpy(out, p->data + at, n); return n;
}
lwespr_t lwesp_conn_recved(lwesp_conn_p c, lwesp_pbuf_p p)
{ assert(locks && c && p); ++consumed; return lwespOK; }
static void opened(connection_t *c)
{
    lwesp_evt_t e = {NULL, c};
    c->active = 1; c->arg = NULL; /* Library resets the exact same connection address. */
    lwesp_core_lock(); Firmware_TCP_OnActive(&e); lwesp_core_unlock();
}
static void closed(connection_t *c)
{
    lwesp_evt_t e = {NULL, c};
    c->active = 0;
    lwesp_core_lock(); Firmware_TCP_OnClosed(&e); lwesp_core_unlock();
}
lwespr_t lwesp_conn_close(lwesp_conn_p c, uint32_t block)
{ assert(!block && locks); ++((connection_t *)c)->closes; closed(c); return lwespOK; }
static void received(connection_t *c, const uint8_t *bytes, unsigned n)
{
    lwesp_pbuf_t p = {bytes, n}; lwesp_evt_t e = {&p, c};
    lwesp_core_lock(); Firmware_TCP_OnReceive(&e); lwesp_core_unlock();
}
QueueHandle_t xQueueCreate(unsigned n, unsigned size)
{ assert(n == 8 && size <= sizeof(items[0])); ++queues; item_size = size; return (void *)1; }
void vQueueDelete(QueueHandle_t q) { (void)q; }
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q) { assert(q == (void *)1); return 8 - count; }
BaseType_t xQueueSend(QueueHandle_t q, const void *p, TickType_t wait)
{
    assert(q == (void *)1 && !wait);
    if (count == 8) return 0;
    memcpy(items[(head + count) % 8], p, item_size); ++count; return pdPASS;
}
TickType_t xTaskGetTickCount(void) { return now; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint16_t stack, void *arg,
                      UBaseType_t priority, TaskHandle_t *handle)
{
    assert(!strcmp(name,"FW_TCP") && stack >= 512 && !arg && priority == 1 && handle);
    ++tasks; *handle = (void *)3; entry = fn; return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t handle)
{ assert(handle == (void *)3); ++notifications; return pdPASS; }
lwespr_t lwesp_conn_send(lwesp_conn_p c, const void *p, size_t n, size_t *sent, uint8_t block)
{
    const char *s = p;
    assert(locks && !pending && !block && !sent && c == &connections[0]);
    assert(((connection_t *)c)->arg && ((connection_t *)c)->active && strlen(s) == n);
    pending = 1; tx_pointer = p; tx_length = n; tx_conn = c; tx_waits = 0;
    memcpy(tx_copy, p, n + 1);
    if (!strcmp(s, "READY\r\n")) { ++ready; stage = 2; }
    else if (!strncmp(s, "ACK ", 4))
    {
        unsigned v; assert(sscanf(s + 4, "%u", &v) == 1);
        assert(v == offset); ++ack;
    }
    else if (!strcmp(s, "DONE\r\n"))
    {
        ++done;
        stage = scenario == PICTURES && done == 1 ? 1 : 3;
        offset = 0;
    }
    else if (!strcmp(s, "EPD1 800 480 256\r\n")) { ++queries; stage = 3; }
    else if (!strncmp(s, "ERR ", 4))
    {
        const char *expected = scenario == PREPARE_FAILURE ? "ERR PIC_PREPARE META_ERASE\r\n" :
          scenario == BAD_HEADER ? "ERR PIC_HEADER \r\n" :
          scenario == WRITE_FAILURE ? "ERR PIC_WRITE BODY_WRITE\r\n" :
          scenario == TIMEOUT ? "ERR TIMEOUT\r\n" : "ERR TRANSPORT\r\n";
        assert(attempts == 0 && !strcmp(s, expected)); ++errors; stage = 4;
    }
    else assert(0 && "unexpected protocol reply");
    return lwespOK;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    lwesp_evt_t e = {NULL, tx_conn};
    assert(!locks && clear == pdTRUE && wait == 100);
    now += wait;
    if (!pending) { unsigned n = notifications; notifications = 0; return n; }
    assert(!strcmp(tx_pointer, tx_copy)); /* Stack/overwrite would corrupt this async send. */
    ++tx_waits;
    if ((scenario == OLD_SEND || scenario == SEND_TIMEOUT) && !injected)
    {
        unsigned before = count;
        closed(&connections[0]); opened(&connections[0]);
        assert(!connections[0].arg && connections[0].closes == 1);
        received(&connections[0], (const uint8_t *)"INFO", 4);
        assert(before == count); injected = 1; stage = 4;
    }
    if (scenario == SEND_TIMEOUT && attempts == 0 && tx_waits <= 301)
        return 0; /* Reply times out; End still cannot release or overwrite its buffer. */
    /* An unrelated client's SEND event cannot free the active response buffer. */
    e.conn = &connections[1]; lwesp_core_lock(); Firmware_TCP_OnSent(&e); lwesp_core_unlock();
    e.conn = tx_conn; lwesp_core_lock(); Firmware_TCP_OnSent(&e); lwesp_core_unlock();
    pending = 0;
    if (scenario == SEND_FAILURE && attempts == 0) stage = 4;
    return 1;
}
static void put32(uint8_t *p, uint32_t n)
{ unsigned i; for (i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i)); }
static void header(void)
{
    unsigned slot = scenario == PICTURES ? done : attempts;
    memset(wire, 0, 32);
    if (scenario == FW_ABORT_BLOCKED || (scenario == FW_COMMITTED && attempts == 0))
    {
        put32(wire, FIRMWARE_MAGIC); put32(wire + 4, 1); put32(wire + 8, 3); put32(wire + 12, 123);
        received(&connections[0], wire, 16); return;
    }
    put32(wire, DOWNLOAD_PICTURE_MAGIC); put32(wire + 4, 1); put32(wire + 8, slot);
    put32(wire + 12, scenario == BAD_HEADER && attempts == 0 ? 799 : 800);
    put32(wire + 16, 480); put32(wire + 20, 48000); put32(wire + 24, 123);
    received(&connections[0], wire, 32);
}
static void body(void)
{
    unsigned n = 48000 - offset, i;
    if (n > 256) n = 256;
    for (i = 0; i < n; ++i) wire[i] = (uint8_t)((offset + i) % 251);
    offset += n; received(&connections[0], wire, n);
}
static void next_step(void)
{
    assert(++steps < 15000 && "worker must recover instead of entering permanent terminal loop");
    if (stage == 0)
    {
        opened(&connections[0]); assert(connections[0].arg); stage = 1;
        if (scenario == STALE && attempts == 1)
            assert(xQueueSend((void *)1, old_item, 0) == pdPASS);
    }
    else if (stage == 1)
    {
        if (scenario == INFO || (scenario == OLD_REPLY && attempts == 1) ||
            (scenario == FW_COMMITTED && attempts == 1))
        {
            static const unsigned lengths[] = {1, 2, 1};
            static const unsigned offsets[] = {0, 1, 3};
            assert(info_part < 3);
            received(&connections[0], (const uint8_t *)"INFO" + offsets[info_part], lengths[info_part]);
            ++info_part;
        }
        else header();
        if (scenario == SECOND && !injected)
        {
            unsigned before = count;
            opened(&connections[1]); assert(!connections[1].arg && connections[1].closes == 1);
            received(&connections[1], (const uint8_t *)"FWUPgarbage", 11); closed(&connections[1]);
            assert(count == before); injected = 1;
        }
    }
    else if (stage == 2)
    {
        if (scenario == FW_ABORT_BLOCKED)
        { closed(&connections[0]); stage = 4; }
        else if (scenario == FW_COMMITTED && attempts == 0)
        { offset = 3; received(&connections[0], (const uint8_t *)"abc", 3); }
        else if ((scenario == INTERRUPTED || scenario == STALE) && attempts == 0 && offset == 256)
        {
            body(); assert(count == 1); memcpy(old_item, items[head], item_size);
            body(); body(); /* Old data already queued when the peer disappears. */
            closed(&connections[0]); stage = 4;
        }
        else if (scenario == TIMEOUT && attempts == 0)
            now += 30000;
        else if (scenario == OVERFLOW && attempts == 0)
            received(&connections[0], wire, sizeof(wire));
        else body();
    }
    else if (stage == 3)
    { closed(&connections[0]); stage = 4; }
    else
    {
        if (connections[0].arg) return; /* Worker has not finished cleanup yet. */
        assert(!pending);
        if (scenario == FW_ABORT_BLOCKED)
        {
            assert(protected_update && protected_update->state == FIRMWARE_RECEIVING);
            assert(protected_update->total_size == 3 && firmware_aborts == 1);
            opened(&connections[0]); assert(!connections[0].arg);
            received(&connections[0], (const uint8_t *)"INFO", 4);
            assert(!count && protected_update->state == FIRMWARE_RECEIVING &&
                   protected_update->total_size == 3 && firmware_begins == 1);
            longjmp(stop, 1);
        }
        ++attempts; offset = 0; info_part = 0;
        if ((scenario == PICTURES && done == 2) ||
            (scenario == RECONNECT && done == 20) || (scenario == INFO && queries == 20) ||
            (scenario == OLD_REPLY && queries == 1) || (scenario == FW_COMMITTED && queries == 1) ||
            (scenario != PICTURES && scenario != RECONNECT && scenario != INFO &&
             scenario != OLD_REPLY && scenario != FW_COMMITTED && done == 1))
            longjmp(stop, 1);
        stage = 0;
    }
}
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait)
{
    assert(q == (void *)1 && !locks && (wait == 0 || wait == 100));
    if (!count && wait) { now += wait; next_step(); }
    if (!count) return 0;
    memcpy(out, items[head], item_size); head = (head + 1) % 8; --count; return pdPASS;
}
uint8_t Picture_Begin(picture_write_t *ctx, uint32_t slot, uint32_t crc)
{
    assert(!locks && crc == 123); ++begins;
    if (scenario == PREPARE_FAILURE && attempts == 0)
    { ctx->error = "META_ERASE"; ctx->error_address = 0x300000; return 0; }
    ctx->slot = slot; ctx->received_size = 0; ctx->active = 1;
    if (scenario == OLD_REPLY && !injected)
    {
        closed(&connections[0]); opened(&connections[0]); assert(!connections[0].arg);
        received(&connections[0], (const uint8_t *)"INFO", 4);
        injected = 1; stage = 4;
    }
    return 1;
}
uint8_t Picture_Write(picture_write_t *ctx, const uint8_t *p, uint32_t n)
{
    unsigned i; assert(!locks); ++writes;
    if (scenario == WRITE_FAILURE && attempts == 0)
    { ctx->error = "BODY_WRITE"; ctx->error_address = 0x301000; return 0; }
    for (i = 0; i < n; ++i) assert(p[i] == (uint8_t)((ctx->received_size + i) % 251));
    ctx->received_size += n; return 1;
}
uint8_t Picture_Finish(picture_write_t *ctx)
{
    assert(!locks && ctx->received_size == 48000); ctx->active = 0; ++finishes;
    committed_mask |= 1U << ctx->slot; return 1;
}
void Picture_Abort(picture_write_t *ctx) { assert(!locks); ctx->active = 0; ++aborted; }
uint8_t W25Q128_ReadStatus(uint8_t *status) { *status = 0; return 1; }
const spi_diagnostic_t *SPI_GetLastDiagnostic(void)
{ static const spi_diagnostic_t diagnostic = {0}; return &diagnostic; }
uint8_t EPD_ShowPicture(uint16_t slot)
{ assert(committed_mask & (1U << slot)); ++shown; return 1; }
uint8_t Firmware_Begin(firmware_update_t *u, uint32_t n, uint32_t crc)
{
    assert(!locks && (scenario == FW_ABORT_BLOCKED || scenario == FW_COMMITTED) && n == 3 && crc == 123);
    ++firmware_begins; protected_update = u; u->state = FIRMWARE_RECEIVING;
    u->total_size = n; u->received_size = 0; return 1;
}
uint8_t Firmware_Write(firmware_update_t *u, const uint8_t *p, uint32_t n)
{ assert(!locks && n == 3 && !memcmp(p, "abc", 3)); u->received_size += n; return 1; }
uint8_t Firmware_Finish(firmware_update_t *u)
{ assert(!locks && u->received_size == 3); u->state = FIRMWARE_READY; ++firmware_finishes; return 1; }
uint8_t Firmware_Abort(firmware_update_t *u)
{
    assert(!locks); ++firmware_aborts;
    if (scenario == FW_ABORT_BLOCKED) return 0;
    u->state = FIRMWARE_IDLE; return 1;
}
int main(int argc, char **argv)
{
    unsigned i;
    if (argc == 2 && !strcmp(argv[1], "--list"))
    { for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) puts(names[i]); return 0; }
    if (argc == 2)
    {
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (!strcmp(names[i], argv[1])) { scenario = i; break; }
        assert(i < sizeof(names) / sizeof(names[0]));
    }
    assert(Firmware_TCP_Init((uint8_t)(scenario == FW_ABORT_BLOCKED || scenario == FW_COMMITTED)));
    assert(Firmware_TCP_Init(0)); assert(tasks == 1 && queues == 1);
    if (setjmp(stop) == 0) entry(NULL);
    assert(!locks && !pending && tasks == 1 && queues == 1);
    if (scenario == INFO)
        assert(queries == 20 && begins == 0 && writes == 0 && aborted == 0 && firmware_begins == 0);
    else if (scenario == OLD_REPLY)
        assert(ready == 0 && ack == 0 && done == 0 && queries == 1 && aborted == 1);
    else if (scenario == FW_ABORT_BLOCKED)
        assert(firmware_aborts == 1 && firmware_finishes == 0 && queries == 0);
    else if (scenario == FW_COMMITTED)
        assert(firmware_finishes == 1 && firmware_aborts == 0 && queries == 1 && done == 1);
    else
    {
        unsigned expected = scenario == RECONNECT ? 20 : scenario == PICTURES ? 2 : 1;
        assert(done == expected && shown == expected && finishes == expected);
        if (scenario == RECONNECT || scenario == PICTURES)
            assert(ready == expected && ack == expected * 188 && aborted == 0);
        else if (scenario == SECOND) assert(aborted == 0 && connections[1].closes == 1);
        else assert(aborted == 1U);
    }
    if (scenario == RECONNECT) assert(committed_mask == 0xFFFFFU);
    printf("PASS TCP %s: DONE=%u INFO=%u ACK=%u preserved=%X\n", names[scenario], done, queries, ack, committed_mask);
    return 0;
}
