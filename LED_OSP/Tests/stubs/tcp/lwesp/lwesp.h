#ifndef TEST_TCP_LWESP_H
#define TEST_TCP_LWESP_H
#include <stdint.h>
#include <stddef.h>
typedef void *lwesp_conn_p;
typedef struct { const uint8_t *data; size_t length; } lwesp_pbuf_t, *lwesp_pbuf_p;
typedef struct { lwesp_pbuf_p buff; lwesp_conn_p conn; } lwesp_evt_t;
typedef enum { lwespOK = 0, lwespERR = 1 } lwespr_t;
lwesp_conn_p lwesp_evt_conn_active_get_conn(lwesp_evt_t *evt);
lwesp_conn_p lwesp_evt_conn_send_get_conn(lwesp_evt_t *evt);
lwespr_t lwesp_evt_conn_send_get_result(lwesp_evt_t *evt);
size_t lwesp_evt_conn_send_get_length(lwesp_evt_t *evt);
lwesp_pbuf_p lwesp_evt_conn_recv_get_buff(lwesp_evt_t *evt);
lwesp_conn_p lwesp_evt_conn_recv_get_conn(lwesp_evt_t *evt);
lwesp_conn_p lwesp_evt_conn_close_get_conn(lwesp_evt_t *evt);
size_t lwesp_pbuf_length(lwesp_pbuf_p buff, uint8_t chain);
size_t lwesp_pbuf_copy(lwesp_pbuf_p buff, void *out, size_t count, size_t offset);
uint8_t lwesp_conn_is_active(lwesp_conn_p conn);
lwespr_t lwesp_conn_send(lwesp_conn_p conn, const void *data, size_t length, size_t *sent, uint8_t block);
lwespr_t lwesp_conn_recved(lwesp_conn_p conn, lwesp_pbuf_p buff);
lwespr_t lwesp_core_lock(void);
lwespr_t lwesp_core_unlock(void);
lwespr_t lwesp_conn_set_arg(lwesp_conn_p conn, void *arg);
void *lwesp_conn_get_arg(lwesp_conn_p conn);
lwespr_t lwesp_conn_close(lwesp_conn_p conn, uint32_t block);
#endif
