// WM8978 LINE IN -> dual-core NAM2 -> headphones.
//
// This target deliberately does not use a writable continuous DMA sample
// ring.  Eight fixed 48-frame slots are chained by DMA in a permanent cycle:
// a slot is either owned by DMA or contains one complete immutable block.
// That is the same block ownership model used by the proven USB NAM path.
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "tusb.h"
#include "bsp/board_api.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include "bootsel_button.h"
#include "wm8960_duplex.pio.h"
#include "wm8978_mclk.pio.h"

void nam_fx_init(void);
void nam_fx_warmup(int blocks);
void nam_fx_process(int32_t *out, const int32_t *in, int frames);
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

#define FRAMES_PER_BLOCK 48u
#define BLOCK_COUNT 8u
#define NAM_INPUT_ATTENUATION 0.5f
#define NAM_GATE_PEAK 1000
#define NAM_GATE_ACTIVE_SAMPLES 8u
#define NAM_INPUT_CLIP_RMS 28000u
#define NAM_GATE_ATTACK_BLOCKS 4u // 4 ms debounce; preserve the tested guitar level
#define NAM_GATE_HOLD_BLOCKS 96u   // retain a natural ~96 ms note tail
#define NAM_REPRIME_BLOCKS 160u

// Every word is one 16-bit stereo I2S frame.  The PIO packs left in bits 31:16.
static uint32_t rx_blocks[BLOCK_COUNT][FRAMES_PER_BLOCK] __aligned(4);
static uint32_t tx_blocks[BLOCK_COUNT][FRAMES_PER_BLOCK] __aligned(4);

// One DMA engine is deliberately re-armed only after it completed a whole
// slot.  RP DMA chain channels do not reload their transfer count by
// themselves, so this is the safe way to get a permanent block schedule.
static int rx_dma;
static int tx_dma;
static volatile uint32_t rx_done_count;
static volatile uint32_t tx_done_count;
static volatile uint32_t rx_started_count;
static volatile uint32_t tx_started_count;
static volatile bool queue_fault;
static volatile uint32_t tx_underrun_count;

static PIO audio_pio;
static uint audio_sm;
static volatile bool nam_enabled = true;
static volatile bool tx_diag;
static volatile bool tx_nam_diag;
static uint32_t rx_read_count;
static uint32_t tx_write_count;
static uint32_t reprime_blocks;
static uint32_t nam_gate_attack;
static uint32_t nam_gate_hold;
static bool nam_gate_open;
static volatile uint32_t adc_peak_last;
static volatile uint32_t adc_nonzero_last;
static volatile uint32_t adc_active_last;
static volatile uint32_t adc_rms_last;
static volatile uint32_t adc_right_peak_last;
static volatile uint32_t adc_right_rms_last;

static uint32_t isqrt_u64(uint64_t x) {
    uint64_t res = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

static void set_nam_enabled(bool enabled) {
    nam_enabled = enabled;
    // A normal mode change always exits one-shot transport diagnostics.  In
    // particular, NAMDIG must not survive a later NAM/BYPASS command and
    // leave the ADC producer apparently idle while the synthetic path runs.
    tx_diag = false;
    tx_nam_diag = false;
    if (enabled) {
        nam_fx_reset();
        reprime_blocks = NAM_REPRIME_BLOCKS;
        nam_gate_attack = 0;
        nam_gate_hold = 0;
        nam_gate_open = false;
    }
    board_led_write(nam_enabled);
}

static bool wm8978_write(uint8_t reg, uint16_t value) {
    if (reg >= 58 || value > 0x1ffu) return false;
    const uint8_t bytes[2] = {
        (uint8_t)((reg << 1) | ((value >> 8) & 1u)), (uint8_t)value,
    };
    return i2c_write_blocking(I2C_PORT, WM8978_ADDR, bytes, 2, false) == 2;
}

static bool write_checked(uint8_t reg, uint16_t value) {
    if (!wm8978_write(reg, value)) return false;
    sleep_ms(2);
    return true;
}

// Exact final register state from the saved clean WM8978 loopback baseline.
// DAC digital volume begins muted; it is opened only after DMA has started.
static bool wm8978_init_line_path(void) {
    if (!write_checked(0, 0x000)) return false;
    sleep_ms(50);
    if (!write_checked(1, 0x01b)) return false;
    // Use the exact LINE IN route from the saved working WM8978 path.  The
    // module's 3.5 mm jack is wired to L2/R2 and does not populate a
    // microphone differential pair; enabling the mic PGA route here causes
    // a floating input to be amplified as buzz.
    if (!write_checked(2, 0x1b3)) return false;
    if (!write_checked(3, 0x06f)) return false;
    if (!write_checked(4, 0x010)) return false; // I2S, 16-bit, slave
    if (!write_checked(6, 0x000)) return false; // external MCLK
    if (!write_checked(10, 0x008)) return false;
    if (!write_checked(11, 0x000)) return false; // DAC mute during startup
    if (!write_checked(12, 0x100)) return false;
    // Keep the ADC high-pass enabled (HPFEN=1) so the line-input bias/Vmid
    // cannot appear as a large DC sample block; ADCOSR=1 selects 128x OSR.
    if (!write_checked(14, 0x108)) return false;
    if (!write_checked(15, 0x0ff)) return false;
    if (!write_checked(16, 0x1ff)) return false;
    if (!write_checked(43, 0x010)) return false;
    // Disable the unused microphone PGAs and use the dedicated L2/R2 line
    // boost setting from the baseline (0 dB, unmuted).
    if (!write_checked(44, 0x000)) return false;
    if (!write_checked(45, 0x040)) return false;
    if (!write_checked(46, 0x140)) return false;
    if (!write_checked(47, 0x140)) return false;
    if (!write_checked(48, 0x140)) return false;
    if (!write_checked(49, 0x002)) return false;
    if (!write_checked(50, 0x001)) return false; // DAC -> output mixer
    if (!write_checked(51, 0x001)) return false;
    if (!write_checked(52, 0x03c)) return false; // saved L/R balance
    if (!write_checked(53, 0x13f)) return false;
    return true;
}

static bool wm8978_unmute_dac(void) {
    return write_checked(11, 0x0ff) && write_checked(12, 0x1ff);
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

static void i2s_duplex_init(PIO pio, uint sm, uint off) {
    pio_sm_config c = wm8960_duplex_program_get_default_config(off);
    sm_config_set_out_pins(&c, I2S_DAC_PIN, 1);
    sm_config_set_in_pins(&c, I2S_ADC_PIN);
    sm_config_set_sideset_pins(&c, I2S_BCLK_PIN);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) /
                              ((float)SAMPLE_RATE * 32.0f * 4.0f));
    pio_gpio_init(pio, I2S_DAC_PIN);
    pio_gpio_init(pio, I2S_ADC_PIN);
    pio_gpio_init(pio, I2S_BCLK_PIN);
    pio_gpio_init(pio, I2S_LRCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_DAC_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_ADC_PIN, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_BCLK_PIN, 2, true);
    pio_sm_init(pio, sm, off, &c);
    pio_sm_exec(pio, sm, pio_encode_set(pio_y, 14));
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 16));
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 15));
}

static void __isr audio_dma_irq(void) {
    const uint32_t pending = dma_hw->ints0;
    dma_hw->ints0 = pending;

    if (pending & (1u << rx_dma)) {
        ++rx_done_count;
        const uint slot = rx_started_count++ & (BLOCK_COUNT - 1u);
        dma_channel_set_write_addr(rx_dma, rx_blocks[slot], false);
        dma_channel_set_trans_count(rx_dma, FRAMES_PER_BLOCK, true);
    }
    if (pending & (1u << tx_dma)) {
        ++tx_done_count;
        // The completed slot is now owned by the producer.  Do not clear it
        // here: the IRQ can race with the producer's next full-block write,
        // and clearing it concurrently can erase only the low 16 bits (right
        // I2S channel), which manifests as a disappearing right channel.
        const uint32_t next = tx_started_count++;
        const uint slot = next & (BLOCK_COUNT - 1u);
        // If the NAM producer has not prepared this sequence number yet,
        // reserve it as an explicit silent block.  Reading the previous
        // contents here repeats stale samples and is the source of the
        // intermittent buzz seen when inference briefly misses a deadline.
        // Advancing tx_write_count reserves the slot so the producer cannot
        // overwrite it while DMA is consuming the silence block.
        if (tx_write_count <= next) {
            memset(tx_blocks[slot], 0, sizeof(tx_blocks[slot]));
            tx_write_count = next + 1u;
            ++tx_underrun_count;
            queue_fault = true;
        }
        dma_channel_set_read_addr(tx_dma, tx_blocks[slot], false);
        dma_channel_set_trans_count(tx_dma, FRAMES_PER_BLOCK, true);
    }
}

static void audio_dma_init(void) {
    rx_dma = dma_claim_unused_channel(true);
    tx_dma = dma_claim_unused_channel(true);
    dma_channel_config rc = dma_channel_get_default_config(rx_dma);
    channel_config_set_transfer_data_size(&rc, DMA_SIZE_32);
    channel_config_set_read_increment(&rc, false);
    channel_config_set_write_increment(&rc, true);
    channel_config_set_dreq(&rc, pio_get_dreq(audio_pio, audio_sm, false));
    dma_channel_configure(rx_dma, &rc, rx_blocks[0], &audio_pio->rxf[audio_sm],
                          FRAMES_PER_BLOCK, false);
    dma_channel_set_irq0_enabled(rx_dma, true);

    dma_channel_config tc = dma_channel_get_default_config(tx_dma);
    channel_config_set_transfer_data_size(&tc, DMA_SIZE_32);
    channel_config_set_read_increment(&tc, true);
    channel_config_set_write_increment(&tc, false);
    channel_config_set_dreq(&tc, pio_get_dreq(audio_pio, audio_sm, true));
    dma_channel_configure(tx_dma, &tc, &audio_pio->txf[audio_sm], tx_blocks[0],
                          FRAMES_PER_BLOCK, false);
    dma_channel_set_irq0_enabled(tx_dma, true);
    irq_set_exclusive_handler(DMA_IRQ_0, audio_dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
}

static inline int16_t sat16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void make_output_block(uint32_t tx_slot, const uint32_t *rx) {
    static int32_t in[FRAMES_PER_BLOCK * 2];
    static int32_t out[FRAMES_PER_BLOCK * 2];
    static uint32_t nam_diag_phase;

    // Keep a cheap, race-safe snapshot for USB diagnostics.  This observes
    // the raw left I2S slot before the gate/NAM path, so LEVEL distinguishes
    // an ADC framing/input problem from an inference/output problem.
    uint32_t raw_peak = 0;
    uint32_t raw_right_peak = 0;
    uint32_t raw_nonzero = 0;
    uint32_t raw_active = 0;
    uint64_t raw_energy = 0;
    uint64_t raw_right_energy = 0;
    int64_t raw_sum = 0;
    for (uint i = 0; i < FRAMES_PER_BLOCK; ++i) {
        const int32_t v = (int16_t)(rx[i] >> 16);
        const int32_t r = (int16_t)rx[i];
        const uint32_t a = (uint32_t)(v < 0 ? -v : v);
        const uint32_t ra = (uint32_t)(r < 0 ? -r : r);
        raw_sum += v;
        if (a > raw_peak) raw_peak = a;
        if (ra > raw_right_peak) raw_right_peak = ra;
        if (a != 0) ++raw_nonzero;
        if (a >= NAM_GATE_PEAK) ++raw_active;
        // Cast to signed 64-bit before multiplying; converting a negative
        // sample directly to uint64_t would turn a quiet signal into a huge
        // diagnostic energy value.
        raw_energy += (uint64_t)((int64_t)v * (int64_t)v);
        raw_right_energy += (uint64_t)((int64_t)r * (int64_t)r);
    }
    adc_peak_last = raw_peak;
    adc_nonzero_last = raw_nonzero;
    adc_active_last = raw_active;
    adc_rms_last = isqrt_u64(raw_energy / FRAMES_PER_BLOCK);
    adc_right_peak_last = raw_right_peak;
    adc_right_rms_last = isqrt_u64(raw_right_energy / FRAMES_PER_BLOCK);
    // A sustained full-scale ADC block is not useful guitar content.  In the
    // IXO12 self-test it indicates an analogue direct-monitor feedback loop;
    // feeding that back into a high-gain NAM would immediately create a
    // buzzer and keep the loop latched.  Drop the block and re-prime instead.
    const bool input_overload = adc_rms_last >= NAM_INPUT_CLIP_RMS;

    if (tx_nam_diag) {
        // Runtime NAM transport diagnostic: synthesize a modest 440 Hz input
        // in the same Q31 format as the ADC path, then run the real model.
        // This isolates inference/DAC from the analogue input and PIO RX.
        for (uint i = 0; i < FRAMES_PER_BLOCK; ++i) {
            const float phase = (float)nam_diag_phase *
                                (6.28318530718f / 4294967296.0f);
            const int16_t s = (int16_t)(5000.0f * sinf(phase));
            nam_diag_phase += 39370534u;
            const int32_t q = (int32_t)((float)s * 65536.0f);
            in[2 * i] = q;
            in[2 * i + 1] = q;
        }
        nam_fx_process(out, in, FRAMES_PER_BLOCK);
        for (uint i = 0; i < FRAMES_PER_BLOCK; ++i) {
            const int16_t v = sat16(out[2 * i] >> 16);
            tx_blocks[tx_slot][i] = ((uint32_t)(uint16_t)v << 16) | (uint16_t)v;
        }
        return;
    }

    if (tx_diag) {
        static uint32_t phase;
        for (uint i = 0; i < FRAMES_PER_BLOCK; ++i) {
            const int16_t v = (phase++ & 32u) ? 8192 : -8192;
            tx_blocks[tx_slot][i] = ((uint32_t)(uint16_t)v << 16) | (uint16_t)v;
        }
        return;
    }

    if (!nam_enabled) {
        for (uint i = 0; i < FRAMES_PER_BLOCK; ++i) {
            const int32_t s = (int16_t)(rx[i] >> 16);
            int32_t dry = s + s / 2; // exact saved loopback gain
            const int16_t v = sat16(dry);
            tx_blocks[tx_slot][i] = ((uint32_t)(uint16_t)v << 16) | (uint16_t)v;
        }
        return;
    }

    const int32_t dc = (int32_t)(raw_sum / (int64_t)FRAMES_PER_BLOCK);
    if (input_overload) {
        nam_gate_open = false;
        nam_gate_attack = 0;
        nam_gate_hold = 0;
        reprime_blocks = NAM_REPRIME_BLOCKS;
    }
    const bool silent_reprime = reprime_blocks != 0 || input_overload;
    int peak = 0;
    uint active_samples = 0;
    if (!silent_reprime) {
        for (uint i = 0; i < FRAMES_PER_BLOCK; ++i) {
            // Remove the codec's block-level input bias before the gate and
            // NAM.  It prevents a floating LINE IN/DC offset from opening a
            // high-gain model, while leaving normal guitar AC content intact.
            const int v = (int16_t)(rx[i] >> 16) - dc;
            const int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            if (a >= NAM_GATE_PEAK) ++active_samples;
        }
        // A cable/static burst with the guitar volume closed is often several
        // milliseconds long. Require a real 24 ms musical envelope before
        // the high-gain model receives signal.
        // A single misframed/analogue ADC spike must not open the high-gain
        // model. Real guitar content occupies several consecutive samples;
        // sparse spikes are treated as silence and never become a buzzer.
        if (peak >= NAM_GATE_PEAK && active_samples >= NAM_GATE_ACTIVE_SAMPLES) {
            if (nam_gate_attack < NAM_GATE_ATTACK_BLOCKS) ++nam_gate_attack;
            nam_gate_hold = NAM_GATE_HOLD_BLOCKS;
            if (nam_gate_attack == NAM_GATE_ATTACK_BLOCKS) nam_gate_open = true;
        } else {
            if (!nam_gate_open) nam_gate_attack = 0;
            if (nam_gate_hold) --nam_gate_hold;
            if (nam_gate_open && !nam_gate_hold) {
                nam_gate_open = false;
                nam_gate_attack = 0;
                reprime_blocks = NAM_REPRIME_BLOCKS;
            }
        }
    }
    const bool gate_closed = !nam_gate_open;
    for (uint i = 0; i < FRAMES_PER_BLOCK; ++i) {
        const int16_t s = (silent_reprime || gate_closed) ? 0 :
                          sat16((int16_t)(rx[i] >> 16) - dc);
        const int32_t q = (int32_t)((float)s * NAM_INPUT_ATTENUATION * 65536.0f);
        in[2 * i] = q;
        in[2 * i + 1] = q;
    }
    nam_fx_process(out, in, FRAMES_PER_BLOCK);
    if (silent_reprime) --reprime_blocks;
    for (uint i = 0; i < FRAMES_PER_BLOCK; ++i) {
        // Write both I2S slots explicitly.  Keeping the two 16-bit values
        // separate makes the channel mapping unambiguous on RP2350 and avoids
        // relying on an implicit stereo packing expression in the hot path.
        const int16_t left = (silent_reprime || gate_closed) ? 0 : sat16(out[2 * i] >> 16);
        // The NAM engine is mono; duplicate its left sample deliberately.
        // Do not depend on the unused second element of the interleaved
        // engine buffer, which may contain a stale value during priming.
        const int16_t right = left;
        tx_blocks[tx_slot][i] = ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
    }
}

static void audio_task(void) {
    for (;;) {
        const uint32_t produced = rx_done_count;
        if (rx_read_count == produced) return;
        if (produced - rx_read_count > BLOCK_COUNT) {
            // RX slot ownership was lost.  Resynchronise at the current DMA
            // position; TX slots have already been cleared by their IRQ.
            queue_fault = true;
            rx_read_count = produced;
            return;
        }
        // The first six output blocks are a silence runway.  Thereafter every
        // NAM result is written far enough ahead that DMA never reads a block
        // while core0 is modifying it.
        if (tx_write_count < tx_started_count) {
            queue_fault = true;
            rx_read_count = produced;
            tx_write_count = tx_started_count + 6u;
            return;
        }
        if (tx_write_count - tx_done_count >= BLOCK_COUNT) return;
        const uint rx_slot = rx_read_count & (BLOCK_COUNT - 1u);
        const uint tx_slot = tx_write_count & (BLOCK_COUNT - 1u);
        make_output_block(tx_slot, rx_blocks[rx_slot]);
        __dmb();
        ++rx_read_count;
        ++tx_write_count;
    }
}

static void button_task(void) {
    static bool previous;
    static uint32_t last_ms;
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_ms < 50u) return;
    last_ms = now;
    multicore_lockout_start_blocking();
    const bool down = bb_get_bootsel_button();
    multicore_lockout_end_blocking();
    if (down && !previous) {
        set_nam_enabled(!nam_enabled);
    }
    previous = down;
}

static void usb_control_task(void) {
    if (!tud_cdc_available()) return;
    char command[24] = {0};
    uint n = 0;
    while (tud_cdc_available() && n < sizeof(command) - 1u) {
        const char c = (char)tud_cdc_read_char();
        if (c == '\n' || c == '\r') break;
        command[n++] = c;
    }
    if (n == 0) return;
    if (!strcmp(command, "NAM")) {
        set_nam_enabled(true);
        tud_cdc_write_str("OK NAM\n");
    } else if (!strcmp(command, "BYPASS")) {
        set_nam_enabled(false);
        tud_cdc_write_str("OK BYPASS\n");
    } else if (!strcmp(command, "STATUS")) {
        tud_cdc_write_str(nam_enabled ? "NAM\n" : "BYPASS\n");
    } else if (!strcmp(command, "LEVEL")) {
        // Keep the diagnostic line below one USB CDC packet.  A long
        // human-readable line was frequently observed only as a truncated
        // prefix by terminal clients, hiding the underrun counter.
        char reply[128];
        snprintf(reply, sizeof(reply), "L=%lu/%lu R=%lu/%lu A=%lu N=%lu O=%u F=%u U=%lu C=%lu/%lu/%lu/%lu\n",
                 (unsigned long)adc_peak_last,
                 (unsigned long)adc_rms_last,
                 (unsigned long)adc_right_peak_last,
                 (unsigned long)adc_right_rms_last,
                 (unsigned long)adc_active_last,
                 (unsigned long)adc_nonzero_last,
                 adc_rms_last >= NAM_INPUT_CLIP_RMS ? 1u : 0u,
                 queue_fault ? 1u : 0u,
                 (unsigned long)tx_underrun_count,
                 (unsigned long)rx_done_count,
                 (unsigned long)tx_done_count,
                 (unsigned long)rx_read_count,
                 (unsigned long)tx_write_count);
        tud_cdc_write_str(reply);
    } else if (!strcmp(command, "DIAG")) {
        tx_diag = true;
        tud_cdc_write_str("OK DIAG\n");
    } else if (!strcmp(command, "NAMDIG")) {
        tx_diag = false;
        tx_nam_diag = true;
        nam_enabled = true;
        reprime_blocks = 0;
        nam_gate_attack = NAM_GATE_ATTACK_BLOCKS;
        nam_gate_open = true;
        nam_fx_reset();
        board_led_write(true);
        tud_cdc_write_str("OK NAMDIG\n");
    } else if (!strcmp(command, "BOOTLOADER")) {
        tud_cdc_write_str("OK BOOTLOADER\n");
        tud_cdc_write_flush();
        sleep_ms(20);
        reset_usb_boot(0, 0);
    } else {
        tud_cdc_write_str("ERR COMMANDS: NAM BYPASS STATUS BOOTLOADER\n");
    }
    tud_cdc_write_flush();
}

int main(void) {
    tusb_init();
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(300000, true);

    audio_pio = pio0;
    mclk_init(audio_pio);
    i2c_init(I2C_PORT, 100000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    if (!wm8978_init_line_path()) while (true) tight_loop_contents();

    audio_sm = pio_claim_unused_sm(audio_pio, true);
    const uint offset = pio_add_program(audio_pio, &wm8960_duplex_program);
    i2s_duplex_init(audio_pio, audio_sm, offset);
    memset(rx_blocks, 0, sizeof(rx_blocks));
    memset(tx_blocks, 0, sizeof(tx_blocks));

    nam_fx_init();
    nam_fx_warmup(NAM_REPRIME_BLOCKS);
    board_led_write(true);

    for (uint i = 0; i < 4; ++i) pio_sm_put_blocking(audio_pio, audio_sm, 0);
    pio_sm_set_enabled(audio_pio, audio_sm, true);
    (void)pio_sm_get_blocking(audio_pio, audio_sm); // discard PIO alignment word
    audio_dma_init();
    // Six silent blocks provide a complete immutable startup runway: NAM
    // starts processing only after the ADC has delivered the first full block.
    rx_started_count = 1;
    tx_started_count = 1;
    tx_write_count = 6;
    dma_start_channel_mask((1u << rx_dma) | (1u << tx_dma));

    // Feed/consume complete blocks while muted, then open the DAC after the
    // queue is established.  I2C writes are protected by eight ms of slots.
    const absolute_time_t unmute_at = make_timeout_time_ms(25);
    bool dac_open = false;
    while (true) {
        tud_task();
        usb_control_task();
        audio_task();
        if (!dac_open && absolute_time_diff_us(get_absolute_time(), unmute_at) <= 0) {
            if (!wm8978_unmute_dac()) while (true) tight_loop_contents();
            dac_open = true;
        }
        button_task();
        tight_loop_contents();
    }
}
