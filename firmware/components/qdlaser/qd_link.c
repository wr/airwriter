// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: J2 as a QuickDraw LaserWriter's link (qd_link.h).
//
// GP-SPI2 runs as a slave in mode 0, LSB first, SCLK from the clock pin through the GPIO matrix.
// Its MISO reaches the TxD pad through the matrix and MOSI is fed from the same pad, so every
// chunk comes back as the line carried it. A chunk is 64 lead marks, packets, then a tail of
// marks; the slave keeps clocking while chip select is low, so the chunk is ended by raising
// chip select inside its tail, from a timer armed the moment the chunk starts.
//
// Chip select uses no pad of its own. ESP-IDF's slave driver routes chip select from the pad
// it is given at every transaction's start and freezes it high at the end; given -1 it would
// write input signal 0 (the flash's SPIQ) instead. So the driver is given TxD, which sits at
// mark (high on a board whose TxD is not inverted: "not selected") whenever the driver reads
// it, and post_setup_cb drives the chip-select input from the matrix's constant 0 before TxD
// is handed to MISO; the end timer drives it from constant 1 (docs/traps.md).
#include "qd_link.h"
#include "qd_wire.h"
#include "serial_out.h"

#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "qd_link";

#define HOST       SPI2_HOST
#define LEAD_BITS  64        // marks while the pad goes over to the SPI
#define TAIL_BITS  4096      // 7.7 ms at 533 kHz; the end is aimed at the middle
#define Q_CHUNK    1024      // a question's chunks
#define JOB_CHUNK  8192      // a job's: a whole row packet (4,099 bytes) fits one
#define DUE_MS     1000      // a chunk ends within this of when it should, or the clock is gone

static qd_link_cfg_t s_c;
static int      s_open, s_spi, s_hz;
static size_t   s_chunk;                 // bytes in each buffer
static uint8_t *s_tx[2], *s_rx[2];
static spi_slave_transaction_t s_t[2];
static size_t   s_bits[2];               // each buffer's line image, in bits
static int64_t  s_due[2];                // when its end is due after it starts, in us
static volatile int64_t s_t0[2], s_ended[2];
static volatile int s_cur = -1;          // the buffer on the wire
static int      s_q[2], s_nq;            // live buffers, oldest first
static int      s_fill = -1;             // the buffer being filled, -1 none
static size_t   s_pos;                   // bits in it so far
static esp_timer_handle_t s_end;
static qd_link_stats_t s_st;

// TxD back to a plain GPIO holding mark.
static void IRAM_ATTR line_to_mark(void)
{
    esp_rom_gpio_connect_out_signal(s_c.tx_pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, s_c.tx_pin, false, false);
}

// From the slave driver's interrupt, as a chunk is loaded: select the slave, hand TxD to MISO
// a couple of lead marks later, and arm the chunk's end.
static void IRAM_ATTR on_setup(spi_slave_transaction_t *t)
{
    int i = t == &s_t[1];
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, FSPICS0_IN_IDX, false);
    s_t0[i] = esp_timer_get_time();
    s_cur = i;
    esp_rom_delay_us(4);
    esp_rom_gpio_connect_out_signal(s_c.tx_pin, FSPIQ_OUT_IDX, s_c.invert_tx, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, s_c.tx_pin, false, false);
    esp_timer_start_once(s_end, (uint64_t)s_due[i]);
}

// The end of the chunk on the wire: TxD to mark, then deselect, which completes it.
static void end_now(void)
{
    int i = s_cur;
    if (i >= 0) s_ended[i] = esp_timer_get_time();
    line_to_mark();
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, FSPICS0_IN_IDX, false);
}

static void end_cb(void *arg)
{
    (void)arg;
    end_now();
}

void qd_link_init(const qd_link_cfg_t *c)
{
    s_c = *c;
}

int qd_link_is_open(void)
{
    return s_open;
}

void qd_link_close(void)
{
    if (s_end) {
        esp_timer_stop(s_end);
        esp_timer_delete(s_end);
        s_end = NULL;
    }
    if (s_spi) {
        spi_slave_free(HOST);
        s_spi = 0;
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, FSPICS0_IN_IDX, false);
    }
    for (int i = 0; i < 2; i++) {
        heap_caps_free(s_tx[i]);
        heap_caps_free(s_rx[i]);
        s_tx[i] = s_rx[i] = NULL;
    }
    s_nq = 0;
    s_fill = -1;
    s_cur = -1;
    if (s_open) {
        s_open = 0;
        serial_out_lend(0);
    }
}

int qd_link_open(int hz, int job)
{
    if (s_open || hz <= 0) return -1;
    s_hz = hz;
    s_chunk = job ? JOB_CHUNK : Q_CHUNK;
    serial_out_set_link(hz, 0);
    serial_out_lend(1);
    s_open = 1;                          // from here, close gives the pins back
    for (int i = 0; i < 2; i++) {
        s_tx[i] = heap_caps_aligned_alloc(4, s_chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_rx[i] = heap_caps_aligned_alloc(4, s_chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_tx[i] || !s_rx[i]) {
            ESP_LOGW(TAG, "no internal RAM for the link's %u-byte chunks", (unsigned)s_chunk);
            qd_link_close();
            return -1;
        }
    }
    esp_timer_create_args_t ta = { .callback = end_cb, .name = "qd_end" };
    if (esp_timer_create(&ta, &s_end) != ESP_OK) {
        qd_link_close();
        return -1;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = -1, .miso_io_num = -1, .sclk_io_num = s_c.clk_pin,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = (int)s_chunk,
    };
    spi_slave_interface_config_t slv = {
        .mode = 0, .spics_io_num = s_c.tx_pin, .queue_size = 2,
        .flags = SPI_SLAVE_BIT_LSBFIRST, .post_setup_cb = on_setup,
    };
    esp_err_t e = spi_slave_initialize(HOST, &bus, &slv, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SPI slave: %s", esp_err_to_name(e));
        qd_link_close();
        return -1;
    }
    s_spi = 1;
    // TxD as a GPIO at mark, its level read back into MOSI. IO17 is UART1's IO_MUX pin: the
    // pad goes to the GPIO function before any GPIO write can reach it.
    const int mark = !s_c.invert_tx;
    gpio_set_level(s_c.tx_pin, mark);
    esp_rom_gpio_pad_select_gpio(s_c.tx_pin);
    gpio_set_direction(s_c.tx_pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(s_c.tx_pin, mark);
    line_to_mark();
    esp_rom_gpio_connect_in_signal(s_c.tx_pin, FSPID_IN_IDX, false);
    s_nq = 0;
    s_fill = -1;
    s_cur = -1;
    return 0;
}

// ---- chunks

static size_t cap_bits(void) { return s_chunk * 8; }

// Whether a packet of n bytes still fits the chunk being filled, with the tail and the marks
// that round the chunk to a whole word.
static int fits(size_t n)
{
    return s_pos + qd_bits_len(n) + TAIL_BITS + 32 <= cap_bits();
}

static void chunk_begin(int i)
{
    s_fill = i;
    s_pos = 0;
    qd_bits_marks(s_tx[i], cap_bits(), &s_pos, LEAD_BITS);
}

// The chunk being filled gets its tail and goes on the wire after any live one.
static int chunk_send(void)
{
    int i = s_fill;
    qd_bits_marks(s_tx[i], cap_bits(), &s_pos, TAIL_BITS);
    qd_bits_marks(s_tx[i], cap_bits(), &s_pos, (32 - (s_pos & 31)) & 31);
    s_bits[i] = s_pos;
    s_due[i] = (int64_t)(s_pos - TAIL_BITS / 2) * 1000000 / s_hz;
    s_t[i] = (spi_slave_transaction_t){ .length = s_pos, .tx_buffer = s_tx[i], .rx_buffer = s_rx[i] };
    s_fill = -1;
    if (spi_slave_queue_trans(HOST, &s_t[i], pdMS_TO_TICKS(DUE_MS)) != ESP_OK) return QD_E_LINK;
    s_q[s_nq++] = i;
    return QD_OK;
}

// Bits that came back off the pad unlike what was sent, from the lead to `end`.
static uint32_t bad_bits(int i, size_t end)
{
    const uint8_t *t = s_tx[i], *r = s_rx[i];
    const uint8_t flip = s_c.invert_tx ? 0xFF : 0x00;
    uint32_t bad = 0;
    for (size_t k = LEAD_BITS / 8; k < end / 8; k++)
        bad += (uint32_t)__builtin_popcount((uint8_t)(t[k] ^ r[k] ^ flip));
    return bad;
}

// The oldest live chunk: waits for its end and checks what came back.
static int chunk_wait(void)
{
    int i = s_q[0];
    int64_t ahead_us = 0;                 // when it can end at the latest, counted from now
    for (int k = 0; k < s_nq; k++) ahead_us += s_due[s_q[k]];
    spi_slave_transaction_t *done = NULL;
    esp_err_t e = spi_slave_get_trans_result(HOST, &done, pdMS_TO_TICKS(ahead_us / 1000 + DUE_MS));
    if (e != ESP_OK || done != &s_t[i]) {
        s_st.stalls++;
        s_st.fail_why = e != ESP_OK ? 1 : 2;
        s_st.fail_got = done ? (uint32_t)done->trans_len : 0;
        s_st.fail_want = (uint32_t)s_bits[i];
        s_st.fail_took_us = (int32_t)(esp_timer_get_time() - s_t0[i]);
        s_st.fail_due_us = (int32_t)s_due[i];
        return QD_E_OFFLINE;
    }
    s_q[0] = s_q[1];
    s_nq--;
    int32_t late = (int32_t)(s_ended[i] - s_t0[i] - s_due[i]);
    if (late > s_st.worst_end_us) s_st.worst_end_us = late;
    s_st.chunks++;
    s_st.bits += (uint32_t)s_bits[i];
    // The read-back off the pad recorded nothing past the lead, with the clock still there: the
    // chunk went out unchecked. Board 1 does this on about one chunk in 20, never the first
    // after the link opens; sending such a chunk again printed its rows twice, so it did reach
    // the printer (W-1102 bench, 2026-11-13).
    if (done->trans_len <= LEAD_BITS && serial_out_clock(NULL)) {
        s_st.unchecked++;
        return QD_OK;
    }
    // Every packet clocked out, or the clock stopped under it.
    if (done->trans_len + TAIL_BITS < s_bits[i]) {
        s_st.stalls++;
        s_st.fail_why = 3;
        s_st.fail_got = (uint32_t)done->trans_len;
        s_st.fail_want = (uint32_t)s_bits[i];
        s_st.fail_took_us = (int32_t)(s_ended[i] - s_t0[i]);
        s_st.fail_due_us = (int32_t)s_due[i];
        return QD_E_OFFLINE;
    }
    size_t end = s_bits[i] - TAIL_BITS / 2;
    if (done->trans_len < end) end = done->trans_len;
    uint32_t bad = bad_bits(i, end);
    s_st.bad_bits += bad;
    return bad ? QD_E_LINK : QD_OK;
}

// After an error: every live chunk is ended at once and nothing is left queued, so the link
// can still carry the session's abort.
static void stop_all(void)
{
    s_fill = -1;
    while (s_nq) {
        end_now();
        spi_slave_transaction_t *done = NULL;
        spi_slave_get_trans_result(HOST, &done, pdMS_TO_TICKS(100));
        esp_timer_stop(s_end);           // the next chunk's, armed as it was loaded
        s_q[0] = s_q[1];
        s_nq--;
    }
    end_now();
    s_cur = -1;
}

static int failed(int rc)
{
    stop_all();
    return rc;
}

// ---- the session's line

static int lk_put(void *ctx, const uint8_t *pkt, size_t n)
{
    (void)ctx;
    if (!s_open) return QD_E_OFFLINE;
    int rc;
    if (s_fill >= 0 && !fits(n) && (rc = chunk_send())) return failed(rc);
    if (s_fill < 0) {
        if (s_nq == 2 && (rc = chunk_wait())) return failed(rc);
        if (!serial_out_clock(NULL)) {
            s_st.fail_why = 4;
            return failed(QD_E_OFFLINE);
        }
        chunk_begin(s_nq ? 1 - s_q[0] : 0);
        if (!fits(n)) {                  // bigger than a whole chunk: never on a question's link
            s_fill = -1;
            return QD_E_LINK;
        }
    }
    return qd_bits_put(s_tx[s_fill], cap_bits(), &s_pos, pkt, n) ? QD_E_LINK : QD_OK;
}

static int lk_flush(void *ctx)
{
    (void)ctx;
    if (!s_open) return QD_E_OFFLINE;
    int rc;
    if (s_fill >= 0 && (rc = chunk_send())) return failed(rc);
    while (s_nq)
        if ((rc = chunk_wait())) return failed(rc);
    return QD_OK;
}

static int lk_read(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    (void)ctx;
    int n = serial_out_read(b, cap, first_ms, gap_ms);
    return n > 0 ? n : 0;
}

static int lk_brk(void *ctx)
{
    (void)ctx;
    return serial_out_break_take();
}

static void lk_drain(void *ctx)
{
    (void)ctx;
    serial_out_rx_drop();
}

static void lk_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    vTaskDelay(ms ? (TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS) : 0);
}

static uint32_t lk_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const qd_ops_t k_ops = {
    .put = lk_put, .flush = lk_flush, .read = lk_read, .brk = lk_brk, .drain = lk_drain,
    .sleep_ms = lk_sleep, .now_ms = lk_now,
};

const qd_ops_t *qd_link_ops(void)
{
    return &k_ops;
}

void qd_link_stats(qd_link_stats_t *out, int reset)
{
    if (out) *out = s_st;
    if (reset) memset(&s_st, 0, sizeof s_st);
}
