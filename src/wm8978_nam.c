// WM8978 line input -> embedded NAM2 -> WM8978 headphones.
// Continuous circular DMA keeps I2S running while the dual-core A2-fast NAM
// engine processes 48-frame blocks. The TX DMA loops over a 16-block ring;
// unlike the earlier version it is never stopped/restarted at block edges.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "wm8960_duplex.pio.h"
#include "wm8978_mclk.pio.h"
#include "bootsel_button.h"

void nam_fx_init(void);
void nam_fx_warmup(int blocks);
void nam_fx_process(int32_t *, const int32_t *, int);
void nam_fx_reset(void);

#define I2C_PORT i2c0
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#define I2S_BCLK_PIN 10
#define I2S_LRCLK_PIN 11
#define I2S_DAC_PIN 12
#define I2S_ADC_PIN 13
#define I2S_MCLK_PIN 14
#define WM8978_ADDR 0x1a
#define SAMPLE_RATE 48000u
#define NAM_FRAMES 48u
#define RING_FRAMES 1024u
#define RING_BYTES (RING_FRAMES * sizeof(uint32_t))
#define TX_RING_FRAMES 512u
// Keep a generous lead between the capture cursor and the TX DMA cursor.
// The dual-core NAM pipeline normally fits in one block, but an occasional
// flash/cache stall must not let the TX DMA consume an unprepared slot (which
// sounds like a buzzer). Eight blocks is ~8 ms at 48 kHz and still fits in the
// 512-frame circular buffer.
#if WM8978_LINE_BYPASS
#define NAM_INPUT_ATTENUATION 0.9f
#else
#define NAM_INPUT_ATTENUATION 0.5f
#endif
#define NAM_GATE_PEAK 96       // about -50 dBFS in the 16-bit codec stream
#ifndef NAM_SELF_TEST
#define NAM_SELF_TEST 0
#endif
#ifndef WM8978_LINE_BYPASS
#define WM8978_LINE_BYPASS 0
#endif

static uint32_t rx_ring[RING_FRAMES] __aligned(RING_BYTES);
static uint32_t tx_ring[TX_RING_FRAMES] __aligned(2048);
static volatile uint32_t tx_write_frames = 0;
static int rx_dma_chan, tx_dma_chan;
static PIO audio_pio;
static uint audio_sm;
static volatile bool g_nam_active = true;

static bool wm8978_write(uint8_t reg, uint16_t value) {
    if (reg >= 58 || value > 0x1ffu) return false;
    const uint8_t b[2] = {(uint8_t)((reg << 1) | ((value >> 8) & 1u)),
                          (uint8_t)value};
    return i2c_write_blocking(I2C_PORT, WM8978_ADDR, b, 2, false) == 2;
}
static bool write_checked(uint8_t reg, uint16_t value) {
    if (!wm8978_write(reg, value)) return false;
    sleep_ms(2);
    return true;
}

static void mclk_init(PIO pio) {
    const uint sm = pio_claim_unused_sm(pio, true);
    const uint off = pio_add_program(pio, &wm8978_mclk_program);
    pio_sm_config c = wm8978_mclk_program_get_default_config(off);
    sm_config_set_sideset_pins(&c, I2S_MCLK_PIN);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (2.0f * 12288000.0f));
    pio_gpio_init(pio, I2S_MCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_MCLK_PIN, 1, true);
    pio_sm_init(pio, sm, off, &c);
    pio_sm_set_enabled(pio, sm, true);
}

static bool wm8978_init_nam_path(void) {
    if (!write_checked(0, 0x000)) return false; sleep_ms(50);
    if (!write_checked(1, 0x01b)) return false;
    if (!write_checked(2, 0x1b3)) return false;
    if (!write_checked(3, 0x06f)) return false;
    if (!write_checked(4, 0x010)) return false;
    if (!write_checked(6, 0x000)) return false;
    if (!write_checked(10, 0x008)) return false;
    // Keep the DAC digitally muted while the PIO/DMA pipeline is being
    // started.  Enabling the codec output before the first valid I2S frames
    // causes the short startup burst heard on headphones.
    if (!write_checked(11, 0x000)) return false;
    if (!write_checked(12, 0x100)) return false;
    if (!write_checked(14, 0x108)) return false;
    if (!write_checked(15, 0x0ff)) return false;
    if (!write_checked(16, 0x1ff)) return false;
    if (!write_checked(43, 0x010)) return false;
    if (!write_checked(44, 0x000)) return false;
    if (!write_checked(45, 0x040)) return false;
    if (!write_checked(46, 0x140)) return false;
    if (!write_checked(47, 0x140)) return false;
    if (!write_checked(48, 0x140)) return false;
    if (!write_checked(49, 0x002)) return false;
    if (!write_checked(50, 0x001)) return false;
    if (!write_checked(51, 0x001)) return false;
    if (!write_checked(52, 0x03c)) return false;
    if (!write_checked(53, 0x13f)) return false;
    return true;
}

static void i2s_duplex_init(PIO pio, uint sm, uint off) {
    pio_sm_config c = wm8960_duplex_program_get_default_config(off);
    sm_config_set_out_pins(&c, I2S_DAC_PIN, 1);
    sm_config_set_in_pins(&c, I2S_ADC_PIN);
    sm_config_set_sideset_pins(&c, I2S_BCLK_PIN);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) /
                              ((float)SAMPLE_RATE * 32.0f * 4.0f));
    pio_gpio_init(pio, I2S_DAC_PIN); pio_gpio_init(pio, I2S_ADC_PIN);
    pio_gpio_init(pio, I2S_BCLK_PIN); pio_gpio_init(pio, I2S_LRCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_DAC_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_ADC_PIN, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_BCLK_PIN, 2, true);
    pio_sm_init(pio, sm, off, &c);
    pio_sm_exec(pio, sm, pio_encode_set(pio_y, 14));
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 16));
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 15));
}

static void dma_init(void) {
    rx_dma_chan = dma_claim_unused_channel(true);
    tx_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config rx = dma_channel_get_default_config(rx_dma_chan);
    channel_config_set_transfer_data_size(&rx, DMA_SIZE_32);
    channel_config_set_read_increment(&rx, false);
    channel_config_set_write_increment(&rx, true);
    // rx_ring is 1024 frames = 4096 bytes; use a 12-bit (4096-byte)
    // destination ring.  Using 11 here wrapped at 512 frames while the
    // producer tracked 1024, causing stale/unwritten samples at each wrap.
    channel_config_set_ring(&rx, true, 12);
    channel_config_set_dreq(&rx, pio_get_dreq(audio_pio, audio_sm, false));
    dma_channel_configure(rx_dma_chan, &rx, rx_ring,
                          &audio_pio->rxf[audio_sm], 0x00ffffffu, false);
    dma_channel_config tx = dma_channel_get_default_config(tx_dma_chan);
    channel_config_set_transfer_data_size(&tx, DMA_SIZE_32);
    channel_config_set_read_increment(&tx, true);
    channel_config_set_write_increment(&tx, false);
    channel_config_set_ring(&tx, false, 11); // ring the 2048-byte source buffer
    channel_config_set_dreq(&tx, pio_get_dreq(audio_pio, audio_sm, true));
    // A 24-bit maximum transfer count runs for almost six minutes at 48 kHz;
    // the read-address ring wraps every 512 frames.  Blocks are copied with
    // modulo indexing below, so a 48-frame block may safely cross the ring end.
    dma_channel_configure(tx_dma_chan, &tx, &audio_pio->txf[audio_sm],
                          tx_ring, 0x00ffffffu, false);
}

static inline int16_t sat16(int32_t v) {
    if (v > 32767) return 32767; if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline uint32_t tx_dma_frame(void) {
    const uintptr_t base = (uintptr_t)tx_ring;
    const uintptr_t addr = (uintptr_t)dma_hw->ch[tx_dma_chan].read_addr;
    return (uint32_t)((addr - base) / sizeof(uint32_t)) & (TX_RING_FRAMES - 1u);
}

static void process_audio(void) {
    static uint32_t processed = 0;
#if NAM_SELF_TEST
    static uint32_t test_phase = 0;
#endif
    static int32_t in_q31[NAM_FRAMES * 2], out_q31[NAM_FRAMES * 2];
    const uintptr_t base = (uintptr_t)rx_ring;
    for (;;) {
        const uintptr_t wa = dma_hw->ch[rx_dma_chan].write_addr;
        const uint32_t wf = (uint32_t)((wa - base) / sizeof(uint32_t)) & (RING_FRAMES - 1u);
        const uint32_t rf = processed & (RING_FRAMES - 1u);
#if !NAM_SELF_TEST
        if (((wf - rf) & (RING_FRAMES - 1u)) < NAM_FRAMES) return;
#endif
        for (uint k = 0; k < NAM_FRAMES; ++k) {
#if NAM_SELF_TEST
            const float phase = (float)test_phase * (6.28318530718f / 4294967296.0f);
            const int16_t s = (int16_t)(9000.0f * sinf(phase));
            test_phase += 39370534u; // 440 Hz at 48 kHz
#else
            const int16_t s = (int16_t)(rx_ring[(rf + k) & (RING_FRAMES - 1u)] >> 16);
#endif
            const int32_t q = (int32_t)((float)s * NAM_INPUT_ATTENUATION * 65536.0f);
            in_q31[2 * k] = q; in_q31[2 * k + 1] = q;
        }
#if NAM_SELF_TEST
        // Transport-only diagnostic: keep the generated tone, but bypass the
        // NAM engine completely.  This isolates DMA/I2S timing from inference.
        memcpy(out_q31, in_q31, sizeof(out_q31));
#elif WM8978_LINE_BYPASS
        // Real codec-input transport diagnostic: pass ADC samples through the
        // same continuous TX ring, without invoking NAM.
        memcpy(out_q31, in_q31, sizeof(out_q31));
#else
        if (g_nam_active) {
            // A high-gain capture magnifies the codec's idle noise. Feed a
            // genuinely silent block to NAM below the measured noise floor;
            // this keeps the amp model quiet between notes without affecting
            // normal guitar levels.
            int peak = 0;
            for (uint k = 0; k < NAM_FRAMES; ++k) {
                const int v = in_q31[2 * k] >> 16;
                const int a = v < 0 ? -v : v;
                if (a > peak) peak = a;
            }
            if (peak < NAM_GATE_PEAK)
                memset(in_q31, 0, sizeof(in_q31));
            nam_fx_process(out_q31, in_q31, NAM_FRAMES);
        } else memcpy(out_q31, in_q31, sizeof(out_q31));
#endif
        // Publish one complete output block. TX DMA only ever reads a block
        // after its completion IRQ, so it cannot observe a half-written frame.
        const uint32_t wf_tx = tx_write_frames;
        // Keep at least three complete blocks between the DMA read cursor and
        // the producer.  If inference falls behind, drop input rather than
        // overwrite a block currently being transmitted.
        const uint32_t distance = (wf_tx - tx_dma_frame()) & (TX_RING_FRAMES - 1u);
        if (distance >= (TX_RING_FRAMES - NAM_FRAMES)) {
            processed += NAM_FRAMES;
            continue;
        }
        for (uint k = 0; k < NAM_FRAMES; ++k) {
            const int16_t l = sat16(out_q31[2 * k] >> 16);
            const int16_t r = sat16(out_q31[2 * k + 1] >> 16);
            tx_ring[(wf_tx + k) & (TX_RING_FRAMES - 1u)] =
                ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
        }
        __dmb(); tx_write_frames = wf_tx + NAM_FRAMES; processed += NAM_FRAMES;
    }
}

static void button_task(void) {
    static bool prev; static uint32_t last;
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last < 50u) return; last = now;
    multicore_lockout_start_blocking();
    const bool down = bb_get_bootsel_button();
    multicore_lockout_end_blocking();
    if (down && !prev) { g_nam_active = !g_nam_active; if (g_nam_active) nam_fx_reset(); }
    prev = down;
}

int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20); sleep_ms(10);
    set_sys_clock_khz(300000, true);
    audio_pio = pio0; mclk_init(audio_pio);
    i2c_init(I2C_PORT, 100000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN); gpio_pull_up(I2C_SCL_PIN);
    if (!wm8978_init_nam_path()) while (true) tight_loop_contents();
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    const uint off = pio_add_program(audio_pio, &wm8960_duplex_program);
    i2s_duplex_init(audio_pio, audio_sm, off);
    memset(rx_ring, 0, sizeof(rx_ring)); memset(tx_ring, 0, sizeof(tx_ring));
#if NAM_SELF_TEST
    // Static transport-only tone: 468.75 Hz is exactly 5 cycles per the
    // 512-frame DMA ring, so the ring boundary is phase-continuous.
    for (uint i = 0; i < TX_RING_FRAMES; ++i) {
        const float phase = 6.28318530718f * 5.0f * (float)i /
                            (float)TX_RING_FRAMES;
        const int16_t s = (int16_t)(9000.0f * sinf(phase));
        tx_ring[i] = ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
    }
    tx_write_frames = 0;
#else
    // Start the continuous TX ring with two silent blocks as a safety lead.
    tx_write_frames = 2u * NAM_FRAMES;
#endif
    // Partition creation launches the second NAM core and can take tens of
    // milliseconds. The line-bypass diagnostic does not need the model at
    // all; skipping it keeps this transport test deterministic and leaves
    // the DMA/PIO timing uncontended. Production NAM builds still initialize
    // and warm up the dual-core engine before starting audio.
#if !WM8978_LINE_BYPASS
    nam_fx_init();
    nam_fx_warmup(160); // ~160 ms of silent history for the high-gain model
#endif
    for (uint i = 0; i < 4; ++i) pio_sm_put_blocking(audio_pio, audio_sm, 0);
    pio_sm_set_enabled(audio_pio, audio_sm, true);
    (void)pio_sm_get_blocking(audio_pio, audio_sm);
    dma_init();
    dma_start_channel_mask((1u << rx_dma_chan) | (1u << tx_dma_chan));
    // Let the producer establish a valid lead in the TX ring before opening
    // the DAC.  This removes the power-on pop without changing steady-state
    // latency or gain.
    for (uint i = 0; i < 100; ++i) {
#if !NAM_SELF_TEST
        process_audio();
#endif
        sleep_ms(1);
    }
    if (!write_checked(11, 0x0ff)) while (true) tight_loop_contents();
    if (!write_checked(12, 0x1ff)) while (true) tight_loop_contents();
    while (true) {
#if !NAM_SELF_TEST
        process_audio();
#endif
        button_task(); tight_loop_contents();
    }
}
