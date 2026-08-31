// USB Audio 2.0 NAM amp-sim: class-compliant stereo 24-bit 48 kHz device
// (host -> "speaker" OUT, processed -> "mic" IN). The NAM A2-Lite model runs on
// the dual-core a2_fast pipeline (core1 = front half, core0 = USB + back half).
// BOOTSEL toggles the effect (active + LED on / bypass + LED off).
//
// IN delivery: the mic endpoint is fed exactly one host-frame's worth of samples
// per USB SOF (tud_audio_tx_done_isr, with CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL off),
// a jitter-free 48 samples/SOF at 48 kHz. The main loop drains the OUT FIFO, runs
// NAM, and hands whole 48-frame blocks to an SPSC ring that the per-SOF ISR drains.
#include <string.h>

#include "bsp/board_api.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "tusb.h"

#include "usb_descriptors.h"

#ifndef NAM_RUNTIME_SYS_KHZ
#define NAM_RUNTIME_SYS_KHZ 300000
#endif
#include "bootsel_button.h"

// NAM effect (C++), C-linkage.
void nam_fx_init(void);
void nam_fx_process(int32_t* out, const int32_t* in, int frames);
void nam_fx_reset(void);
bool nam_fx_load_weights(const float* weights, size_t count);

#define FRAME_LENGTH 48                            // NAM block = 48 stereo frames
#define BYTES_PER_FRAME (2 * (int)sizeof(int32_t)) // 2ch * 4B (32-bit PCM slots)
#define BLOCK_BYTES (FRAME_LENGTH * BYTES_PER_FRAME)

static int32_t scratch_in[FRAME_LENGTH * 2];
static int32_t scratch_out[FRAME_LENGTH * 2];
static volatile bool g_active = false; // NAM on / bypass

// WebUSB upload format: 16-byte little-endian header "NAMW", version, weight
// count and payload byte count, followed by float32 A2-Lite weights.
#define NAM_UPLOAD_MAX_WEIGHTS 8192u
static float upload_weights[NAM_UPLOAD_MAX_WEIGHTS];
static uint8_t upload_header[16];
static uint32_t upload_header_len = 0;
static uint32_t upload_count = 0;
static uint32_t upload_bytes = 0;
static uint32_t upload_received = 0;
static bool upload_in_progress = false;
static uint32_t upload_last_ms = 0;

static void vendor_reply(const char* msg) {
    if (tud_cdc_connected()) {
        tud_cdc_write_str(msg);
        tud_cdc_write_str("\n");
        tud_cdc_write_flush();
    }
}

static void vendor_task(void) {
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (upload_in_progress && now - upload_last_ms > 5000u) {
        upload_in_progress = false;
        upload_header_len = upload_received = 0;
        vendor_reply("ERR:TIMEOUT");
    }

    uint8_t buf[64];
    while (tud_cdc_available()) {
        uint32_t n = tud_cdc_read(buf, sizeof(buf));
        upload_last_ms = now;
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t b = buf[i];
            if (!upload_in_progress) {
                upload_header[upload_header_len++] = b;
                if (upload_header_len < sizeof(upload_header)) continue;
                if (memcmp(upload_header, "NAMW", 4) != 0 || upload_header[4] != 1) {
                    vendor_reply("ERR:HEADER"); upload_header_len = 0; continue;
                }
                memcpy(&upload_count, upload_header + 8, 4);
                memcpy(&upload_bytes, upload_header + 12, 4);
                if (upload_count == 0 || upload_count > NAM_UPLOAD_MAX_WEIGHTS || upload_bytes != upload_count * 4u) {
                    vendor_reply("ERR:SIZE"); upload_header_len = 0; continue;
                }
                upload_received = 0; upload_in_progress = true; upload_header_len = 0;
                continue;
            }
            if (upload_received < upload_bytes) {
                ((uint8_t*)upload_weights)[upload_received++] = b;
            }
            if (upload_received == upload_bytes) {
                g_active = false;
                board_led_write(false);
                multicore_lockout_start_blocking();
                bool ok = nam_fx_load_weights(upload_weights, upload_count);
                multicore_lockout_end_blocking();
                vendor_reply(ok ? "OK:MODEL" : "ERR:MODEL");
                upload_in_progress = false; upload_header_len = 0;
            }
        }
    }
}

// SPSC staging ring (processed stereo frames). Producer = main loop (audio_task);
// consumer = the per-SOF USB ISR (tud_audio_tx_done_isr). Both run on core0, so a
// volatile head/tail + a barrier before publishing the data is sufficient.
// Capacity is a multiple of FRAME_LENGTH; both indices only ever advance by whole
// 48-frame blocks, so neither producer nor consumer block ever straddles the wrap.
#define TX_RING_FRAMES (FRAME_LENGTH * 16u) // 768 frames (~16 ms headroom)
#define TX_PRIME_FRAMES (FRAME_LENGTH * 2u) // ~2 ms silence cushion at stream start
static int32_t tx_ring[TX_RING_FRAMES * 2]; // interleaved stereo
static volatile uint32_t txr_w = 0;          // producer counter (free-running, frames)
static volatile uint32_t txr_r = 0;          // consumer counter (free-running, frames)
static int32_t tx_last[2] = {0, 0};          // last delivered frame (repeat on underrun)

// MIC (IN) streaming alt-setting is active: only then do we feed the IN FIFO.
volatile bool g_mic_streaming = false;

static inline void tx_ring_push_block(const int32_t* blk /* 48 frames */) {
    const uint32_t idx = (txr_w % TX_RING_FRAMES) * 2; // block-aligned: never wraps
    memcpy(&tx_ring[idx], blk, BLOCK_BYTES);
    __dmb();          // publish payload before advancing the write index
    txr_w += FRAME_LENGTH;
}

// Called from tud_audio_set_itf_cb (usb_descriptors.c) on alt-setting changes.
void usb_audio_streaming_changed(uint8_t itf, bool on) {
    if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
        if (on && !g_mic_streaming) { // (re)start: empty ring + small silence cushion
            txr_r = txr_w = 0;
            static const int32_t silence[FRAME_LENGTH * 2] = {0};
            for (uint32_t f = 0; f < TX_PRIME_FRAMES; f += FRAME_LENGTH)
                tx_ring_push_block(silence);
        }
        g_mic_streaming = on;
    }
}

// Per-SOF IN delivery (0.20.0 renamed tud_audio_tx_done_pre_load_cb). With flow
// control off we put EXACTLY one host-frame block (48 @ 48 kHz) into the IN FIFO
// each SOF — deterministic, jitter-free. On underrun, repeat the last frame so a
// momentary processing stall is a held sample rather than a click.
bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id,
                           uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport; (void)n_bytes_sent; (void)func_id; (void)ep_in; (void)cur_alt_setting;

    int32_t pkt[FRAME_LENGTH * 2];
    if ((txr_w - txr_r) >= FRAME_LENGTH) {
        const uint32_t idx = (txr_r % TX_RING_FRAMES) * 2; // block-aligned: never wraps
        memcpy(pkt, &tx_ring[idx], BLOCK_BYTES);
        __dmb();
        txr_r += FRAME_LENGTH;
        tx_last[0] = pkt[2 * (FRAME_LENGTH - 1)];
        tx_last[1] = pkt[2 * (FRAME_LENGTH - 1) + 1];
    } else { // underrun: ramp the last sample to silence (never hold loud DC)
        for (int k = 0; k < FRAME_LENGTH; k++) {
            const int32_t gain = FRAME_LENGTH - 1 - k;
            pkt[2 * k] = (int32_t)(((int64_t)tx_last[0] * gain) / FRAME_LENGTH);
            pkt[2 * k + 1] = (int32_t)(((int64_t)tx_last[1] * gain) / FRAME_LENGTH);
        }
        tx_last[0] = tx_last[1] = 0;
    }
    tud_audio_write((uint8_t*)pkt, BLOCK_BYTES);
    return true;
}

// Main-loop producer: drain whole 48-frame blocks off the OUT (speaker) FIFO, run
// NAM (or pass through), and stage them for the per-SOF ISR to deliver.
void audio_task(void) {
    // Always drain the speaker OUT FIFO. If we wait for the host to open the
    // mic IN stream first, CoreAudio can deadlock while starting the duplex
    // device: OUT waits for firmware reads while IN waits for OUT to start.
    while (tud_audio_available() >= BLOCK_BYTES) {
        if (g_mic_streaming &&
            (TX_RING_FRAMES - (txr_w - txr_r)) < FRAME_LENGTH)
            break;
        if (tud_audio_read(scratch_in, BLOCK_BYTES) != BLOCK_BYTES)
            break;
        if (!g_mic_streaming)
            continue;
        if (g_active)
            nam_fx_process(scratch_out, scratch_in, FRAME_LENGTH);
        else
            memcpy(scratch_out, scratch_in, BLOCK_BYTES);
        tx_ring_push_block(scratch_out);
    }
}

// BOOTSEL edge-detect toggle + LED. Reading BOOTSEL toggles the QSPI CS line, so
// we park core1 (multicore_lockout) for the duration to keep it off the flash.
static void button_task(void) {
    static bool prev = false;
    static uint32_t last_ms = 0;
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_ms < 50) return; // poll at ~20 Hz
    last_ms = now;

    multicore_lockout_start_blocking();
    const bool pressed = bb_get_bootsel_button();
    multicore_lockout_end_blocking();

    if (pressed && !prev) { // rising edge -> toggle
        g_active = !g_active;
        if (g_active) nam_fx_reset(); // re-prime the pipeline
        board_led_write(g_active);
    }
    prev = pressed;
}

int main(void) {
    // Overclock to 300 MHz (2x the 150 MHz default) for dual-core pipeline headroom.
#ifndef NAM_SKIP_CLOCK_SETUP
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(NAM_RUNTIME_SYS_KHZ, true);
#endif

    board_init();
    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
    board_init_after_tusb();

#ifndef NAM_SKIP_MODEL_INIT
    nam_fx_init();          // create models + launch core1 front worker
#endif
    board_led_write(false); // effect starts bypassed (LED off)

    while (1) {
        tud_task();
        vendor_task();
        audio_task();
        button_task();
    }
}
