#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "wm8960_duplex.pio.h"
#include "wm8960_tx.pio.h"
#include "wm8978_mclk.pio.h"

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
#define SYS_CLOCK_KHZ 153600u
#define SINE_TABLE_SIZE 256u

static uint16_t register_cache[58];
static int16_t sine_table[SINE_TABLE_SIZE];

typedef struct {
    float frequency_hz;
    uint16_t duration_ms;
} melody_note_t;

static const melody_note_t melody[] = {
    {261.63f, 260}, {329.63f, 260}, {392.00f, 260}, {523.25f, 380},
    {392.00f, 260}, {329.63f, 260}, {261.63f, 420}, {0.0f, 300},
};

static bool wm8978_write(uint8_t reg, uint16_t value) {
    if (reg >= 58 || value > 0x1ffu) return false;
    const uint8_t bytes[2] = {
        (uint8_t)((reg << 1) | ((value >> 8) & 1u)),
        (uint8_t)value,
    };
    if (i2c_write_blocking(I2C_PORT, WM8978_ADDR, bytes, 2, false) != 2) {
        return false;
    }
    register_cache[reg] = value;
    return true;
}

static bool write_checked(uint8_t reg, uint16_t value) {
    if (!wm8978_write(reg, value)) return false;
    sleep_ms(2);
    return true;
}

static void mclk_init(PIO pio) {
    // clk_sys = 153.6 MHz. Two PIO cycles at divider 6.25 produce an exact
    // 12.288 MHz clock (256 * 48 kHz) with deterministic 50-percent duty.
    const uint sm = pio_claim_unused_sm(pio, true);
    const uint offset = pio_add_program(pio, &wm8978_mclk_program);
    pio_sm_config config = wm8978_mclk_program_get_default_config(offset);
    sm_config_set_sideset_pins(&config, I2S_MCLK_PIN);
    sm_config_set_clkdiv(&config, 6.25f);
    pio_gpio_init(pio, I2S_MCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_MCLK_PIN, 1, true);
    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_enabled(pio, sm, true);
}

static bool wm8978_init_playback(void) {
    // Register sequence follows the vendor WM8978 recorder/player examples.
    if (!write_checked(0, 0x000)) return false;   // software reset
    sleep_ms(50);
    if (!write_checked(1, 0x01b)) return false;   // VMID/VREF and analogue bias
    if (!write_checked(2, 0x1b0)) return false;   // output + input boost stages
    if (!write_checked(3, 0x06c)) return false;   // vendor mixer/output power
    if (!write_checked(4, 0x010)) return false;   // standard I2S, 16-bit
    if (!write_checked(6, 0x000)) return false;   // external MCLK, codec slave
    if (!write_checked(10, 0x008)) return false;  // DAC unmuted, 128x oversample
    if (!write_checked(11, 0x0ff)) return false;  // left DAC digital volume
    if (!write_checked(12, 0x1ff)) return false;  // right DAC volume + update
    if (!write_checked(14, 0x008)) return false;  // ADC 128x oversample
    if (!write_checked(43, 0x010)) return false;  // output polarity
    if (!write_checked(47, 0x100)) return false;  // vendor baseline line gain
    if (!write_checked(48, 0x100)) return false;
    if (!write_checked(49, 0x002)) return false;  // thermal protection
    if (!write_checked(50, 0x001)) return false;  // left DAC -> left mixer
    if (!write_checked(51, 0x001)) return false;  // right DAC -> right mixer
    if (!write_checked(52, 52)) return false;     // headphone left, -5 dB
    if (!write_checked(53, 52 | 0x100)) return false;
    return true;
}

static bool wm8978_enable_line_bypass(void) {
    // This module has a LINE IN jack on L2/R2 and no populated microphone.
    // Route L2/R2 through the dedicated line-input boost.  Do not involve
    // the differential microphone PGAs: their negative pins are floating on
    // this board and would saturate or add high-frequency noise.
    if (!write_checked(2, 0x1b3)) return false;   // ADCs + line boosts only
    if (!write_checked(44, 0x000)) return false;  // microphone pins off
    if (!write_checked(45, 0x040)) return false;  // unused PGAs muted
    if (!write_checked(46, 0x140)) return false;
    if (!write_checked(47, 0x140)) return false;  // L2 direct line gain, -3 dB
    if (!write_checked(48, 0x140)) return false;  // R2 direct line gain, -3 dB
    if (!write_checked(50, 0x01e)) return false;  // left bypass, +6 dB
    if (!write_checked(51, 0x01e)) return false;  // right bypass, +6 dB
    if (!write_checked(52, 0x03f)) return false;  // headphone left, +6 dB
    if (!write_checked(53, 0x13f)) return false;  // headphone right, +6 dB
    if (!write_checked(3, 0x06c)) return false;   // DACs off, mixers remain on
    return true;
}

static bool wm8978_enable_line_adc(void) {
    // Enable both ADCs and the dedicated L2/R2 line-input boost path. Use the
    // maximum line-input mixer setting (+6 dB) only for this level diagnostic;
    // the production guitar path will use calibrated gain after a buffer.
    if (!write_checked(2, 0x1b3)) return false;   // boost stages + both ADCs
    if (!write_checked(3, 0x06c)) return false;   // DACs off, mixers powered
    if (!write_checked(44, 0x000)) return false;  // microphone PGAs disconnected
    if (!write_checked(47, 0x140)) return false;  // L2 LINE IN, -3 dB
    if (!write_checked(48, 0x140)) return false;  // R2 LINE IN, -3 dB
    if (!write_checked(14, 0x108)) return false;  // ADC HPF + 128x oversampling
    if (!write_checked(15, 0x0ff)) return false;  // left ADC digital volume
    if (!write_checked(16, 0x1ff)) return false;  // right ADC + update
    return true;
}

static bool wm8978_enable_line_loopback(void) {
    // Keep DAC/output power from wm8978_init_playback(), and additionally
    // enable the line-input ADC path for a hardware loopback test.
    if (!write_checked(2, 0x1b3)) return false;   // ADCs + line boosts only
    // The vendor module's 3.5 mm LINE IN is wired directly to L2/R2.
    if (!write_checked(44, 0x000)) return false;  // microphone pins off
    if (!write_checked(45, 0x040)) return false;
    if (!write_checked(46, 0x140)) return false;
    if (!write_checked(47, 0x140)) return false;  // L2 direct line gain, -3 dB
    if (!write_checked(48, 0x140)) return false;  // R2 direct line gain, -3 dB
    if (!write_checked(14, 0x108)) return false;  // ADC HPF + oversampling
    if (!write_checked(15, 0x0ff)) return false;
    if (!write_checked(16, 0x1ff)) return false;
    // R3 bits 0/1 power up the left/right DACs.  The initial codec setup
    // leaves them off while the ADC is being diagnosed; loopback must turn
    // them on so the captured guitar samples can reach the headphone mixer.
    if (!write_checked(3, 0x06f)) return false;
    if (!write_checked(50, 0x001)) return false; // DAC -> left mixer
    if (!write_checked(51, 0x001)) return false; // DAC -> right mixer
    // The physical headphone path is slightly louder on the left in the
    // current setup; apply a conservative 3 dB trim rather than boosting the
    // right channel into clipping.
    if (!write_checked(52, 0x03c)) return false; // left +3 dB
    if (!write_checked(53, 0x13f)) return false; // right +6 dB + update
    return true;
}

static void i2s_tx_init(PIO pio, uint sm, uint offset) {
    pio_sm_config config = wm8960_tx_program_get_default_config(offset);
    sm_config_set_out_pins(&config, I2S_DAC_PIN, 1);
    sm_config_set_sideset_pins(&config, I2S_BCLK_PIN);
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);

    const float pio_hz = (float)SAMPLE_RATE * 32.0f * 2.0f;
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / pio_hz);
    pio_gpio_init(pio, I2S_DAC_PIN);
    pio_gpio_init(pio, I2S_BCLK_PIN);
    pio_gpio_init(pio, I2S_LRCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_DAC_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_BCLK_PIN, 2, true);
    pio_sm_init(pio, sm, offset, &config);
    pio_sm_exec(pio, sm,
                pio_encode_jmp(offset + wm8960_tx_offset_entry_point));
}

static void i2s_duplex_init(PIO pio, uint sm, uint offset) {
    pio_sm_config config = wm8960_duplex_program_get_default_config(offset);
    sm_config_set_out_pins(&config, I2S_DAC_PIN, 1);
    sm_config_set_in_pins(&config, I2S_ADC_PIN);
    sm_config_set_sideset_pins(&config, I2S_BCLK_PIN);
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_in_shift(&config, false, true, 32);
    const float pio_hz = (float)SAMPLE_RATE * 32.0f * 4.0f;
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / pio_hz);
    pio_gpio_init(pio, I2S_DAC_PIN);
    pio_gpio_init(pio, I2S_ADC_PIN);
    pio_gpio_init(pio, I2S_BCLK_PIN);
    pio_gpio_init(pio, I2S_LRCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_DAC_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_ADC_PIN, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_BCLK_PIN, 2, true);
    pio_sm_init(pio, sm, offset, &config);
    pio_sm_exec(pio, sm, pio_encode_set(pio_y, 14));
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 16));
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 15));
}

static void report_line_input(PIO pio, uint sm) {
    for (uint32_t i = 0; i < 4; ++i) pio_sm_put_blocking(pio, sm, 0);
    pio_sm_set_enabled(pio, sm, true);
    (void)pio_sm_get_blocking(pio, sm); // discard alignment word

    uint32_t frames = 0;
    uint32_t left_peak = 0;
    uint32_t right_peak = 0;
    uint64_t left_energy = 0;
    uint64_t right_energy = 0;
    while (true) {
        const uint32_t word = pio_sm_get_blocking(pio, sm);
        pio_sm_put_blocking(pio, sm, 0); // headphones stay digitally silent
        const int32_t left = (int16_t)(word >> 16);
        const int32_t right = (int16_t)word;
        const uint32_t la = (uint32_t)(left < 0 ? -left : left);
        const uint32_t ra = (uint32_t)(right < 0 ? -right : right);
        if (la > left_peak) left_peak = la;
        if (ra > right_peak) right_peak = ra;
        left_energy += (uint64_t)(left * left);
        right_energy += (uint64_t)(right * right);
        if (++frames == 4800u) {
            const uint32_t left_rms =
                (uint32_t)sqrt((double)left_energy / frames);
            const uint32_t right_rms =
                (uint32_t)sqrt((double)right_energy / frames);
            printf("LINE_INPUT L peak=%lu rms=%lu | R peak=%lu rms=%lu\n",
                   (unsigned long)left_peak, (unsigned long)left_rms,
                   (unsigned long)right_peak, (unsigned long)right_rms);
            frames = left_peak = right_peak = 0;
            left_energy = right_energy = 0;
        }
    }
}

static void loopback_line_input(PIO pio, uint sm) {
    for (uint32_t i = 0; i < 4; ++i) pio_sm_put_blocking(pio, sm, 0);
    pio_sm_set_enabled(pio, sm, true);
    (void)pio_sm_get_blocking(pio, sm); // discard alignment word
    while (true) {
        const uint32_t word = pio_sm_get_blocking(pio, sm);
        int16_t left = (int16_t)(word >> 16);
        // Modest 1.5x linear gain with headroom.
        int32_t amplified = (int32_t)left + ((int32_t)left / 2);
        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;
        left = (int16_t)amplified;
        // The board's guitar jack is wired to the left input only. Copy it
        // to both headphone channels; ignore the unused right input noise.
        pio_sm_put_blocking(pio, sm,
                            ((uint32_t)(uint16_t)left << 16) |
                            (uint16_t)left);
    }
}

static void build_sine_table(void) {
    for (uint32_t i = 0; i < SINE_TABLE_SIZE; ++i) {
        const float phase = 2.0f * (float)M_PI * (float)i /
                            (float)SINE_TABLE_SIZE;
        sine_table[i] = (int16_t)(sinf(phase) * 6500.0f);
    }
}

static void play_melody(PIO pio, uint sm) {
    for (uint32_t note_index = 0;
         note_index < sizeof(melody) / sizeof(melody[0]); ++note_index) {
        const melody_note_t *note = &melody[note_index];
        const uint32_t frames = note->duration_ms * SAMPLE_RATE / 1000u;
        const uint32_t phase_step = note->frequency_hz > 0.0f
            ? (uint32_t)(note->frequency_hz * 4294967296.0 / SAMPLE_RATE)
            : 0u;
        uint32_t phase = 0;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            uint32_t gain = 32767u;
            if (frame < 480u) gain = frame * 32767u / 480u;
            const uint32_t remaining = frames - frame - 1u;
            if (remaining < 480u) {
                const uint32_t release = remaining * 32767u / 480u;
                if (release < gain) gain = release;
            }
            int16_t sample = 0;
            if (phase_step) {
                sample = (int16_t)(((int32_t)sine_table[phase >> 24] *
                                    (int32_t)gain) / 32767);
                phase += phase_step;
            }
            const uint16_t u = (uint16_t)sample;
            pio_sm_put_blocking(pio, sm, ((uint32_t)u << 16) | u);
        }
    }
}

int main(void) {
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
    // Bring up CDC before touching I2C so a wiring/configuration failure is
    // observable instead of trapping in the pre-stdio error loop.
    stdio_init_all();
    sleep_ms(500);
    printf("WM8978 diagnostic starting\n");
    PIO pio = pio0;
    mclk_init(pio);
    sleep_ms(100);

    i2c_init(I2C_PORT, 100000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    const bool codec_ok = wm8978_init_playback();
    if (!codec_ok) {
        while (true) {
            printf("ERROR: WM8978 did not acknowledge I2C at 0x1A\n");
            sleep_ms(500);
        }
    }

    // Measure the codec ADC input before attempting any digital loopback.
    if (!wm8978_enable_line_loopback()) {
        while (true) {
            printf("ERROR: WM8978 ADC setup failed\n");
            sleep_ms(500);
        }
    }
    const uint duplex_sm = pio_claim_unused_sm(pio, true);
    const uint duplex_offset = pio_add_program(pio, &wm8960_duplex_program);
    i2s_duplex_init(pio, duplex_sm, duplex_offset);
    printf("WM8978 OK: L2 guitar -> Pico -> headphones\n");
    loopback_line_input(pio, duplex_sm);
}
