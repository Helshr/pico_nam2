#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "wm8960_duplex.pio.h"
#include "wm8960_tx.pio.h"

#define I2C_PORT i2c0
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#define I2S_BCLK_PIN 10
#define I2S_LRCLK_PIN 11
#define I2S_DAC_PIN 12  // Pico output -> Waveshare RXSDA -> WM8960 DACDAT
#define I2S_ADC_PIN 13  // Pico input  <- Waveshare TXSDA <- WM8960 ADCDAT
#define WM8960_ADDR 0x1a
// With the codec using its onboard 24 MHz oscillator and the official
// default clock tree (MCLK / 2 / 256), its exact native rate is 46,875 Hz.
// Match that rate during diagnostics so DAC samples are not consumed by an
// asynchronous clock.  The production NAM firmware will later use the PLL
// for an exact 48 kHz clock domain.
#define SAMPLE_RATE 46875u
#define SINE_TABLE_SIZE 256u
#define NOTE_ATTACK_FRAMES 480u
#define NOTE_RELEASE_FRAMES 480u

#ifndef WM8960_USE_EXTERNAL_GUITAR
#define WM8960_USE_EXTERNAL_GUITAR 0
#endif

#ifndef WM8960_GUITAR_LEVEL_DIAGNOSTIC
#define WM8960_GUITAR_LEVEL_DIAGNOSTIC 0
#endif

#ifndef WM8960_GUITAR_UNITY_MONITOR
#define WM8960_GUITAR_UNITY_MONITOR 0
#endif

#ifndef WM8960_DUPLEX_TONE_TEST
#define WM8960_DUPLEX_TONE_TEST 0
#endif

#ifndef WM8960_CAPTURE_DIAGNOSTIC
#define WM8960_CAPTURE_DIAGNOSTIC 0
#endif

#ifndef WM8960_ADC_MUTE_DIAGNOSTIC
#define WM8960_ADC_MUTE_DIAGNOSTIC 0
#endif

#if WM8960_CAPTURE_DIAGNOSTIC
#define CAPTURE_FRAMES 4096u
static int16_t capture_samples[CAPTURE_FRAMES];
#endif

static int16_t sine_table[SINE_TABLE_SIZE];

typedef struct {
    float frequency_hz;
    uint16_t duration_ms;
} melody_note_t;

static const melody_note_t melody[] = {
    {261.63f, 320}, // C4
    {329.63f, 320}, // E4
    {392.00f, 320}, // G4
    {523.25f, 420}, // C5
    {392.00f, 320}, // G4
    {329.63f, 320}, // E4
    {261.63f, 500}, // C4
    {0.0f,   500},  // short pause before repeating / diagnostics
};

static bool wm8960_write(uint8_t reg, uint16_t value) {
    uint8_t bytes[2] = {
        (uint8_t)((reg << 1) | ((value >> 8) & 1u)),
        (uint8_t)(value & 0xffu),
    };
    return i2c_write_blocking(I2C_PORT, WM8960_ADDR, bytes, 2, false) == 2;
}

static bool wm8960_init_playback(void) {
    // Exact playback sequence that was already verified on this board.
    static const struct {
        uint8_t reg;
        uint16_t value;
    } init[] = {
        {0x0f, 0x0000},             // reset
        {0x19, 0x01c0},             // fast VMID and VREF
        {0x1a, 0x01f8},             // DACs and output stages
        {0x2f, 0x000c},             // left/right output mixers only
        {0x04, 0x0004},             // 24 MHz / 2 / 256 = 46,875 Hz
        {0x05, 0x0000},             // DAC unmuted
        {0x07, 0x0002},             // I2S, 16-bit words, codec slave
        {0x02, 0x016f},             // headphone left, unmuted
        {0x03, 0x016f},             // headphone right, unmuted
        {0x28, 0x017f},             // speaker left
        {0x29, 0x017f},             // speaker right
        {0x31, 0x00f7},             // class-D outputs
        {0x0a, 0x01ff},             // DAC left volume
        {0x0b, 0x01ff},             // DAC right volume
        {0x22, 0x0180},             // left DAC mixer
        {0x25, 0x0180},             // right DAC mixer
        {0x18, 0x0040},             // jack-detect/output control
        {0x17, 0x01c3},             // known-good additional control
        {0x30, 0x0009},             // output routing / jack detect
    };

    for (uint32_t i = 0; i < sizeof(init) / sizeof(init[0]); ++i) {
        if (!wm8960_write(init[i].reg, init[i].value)) return false;
        sleep_ms(2);
    }

    return true;
}

static bool wm8960_enable_input(void) {
    // Reproduce the official Waveshare WM8960_Record register sequence.
    // This deliberately replaces the earlier "minimal delta" experiment:
    // receiving all-zero I2S frames showed that the ADC/data-output path had
    // not actually been enabled completely.
    static const struct {
        uint8_t reg;
        uint16_t value;
    } mic[] = {
        {0x0f, 0x0000},             // reset codec before record setup
#if WM8960_USE_EXTERNAL_GUITAR
        // Buffered guitar is injected at INPUT_R1 on the codec side of the
        // CTIA coupling network.  MICBIAS must stay off: it is intended for
        // electret headset capsules and adds an unwanted DC/noise source to
        // a low-impedance pedal or instrument-buffer output.
        {0x19, 0x00d4},             // VMID/VREF + right input + right ADC
#else
        {0x19, 0x00e8},             // VMID/VREF + left input + left ADC
#endif
        {0x1a, 0x01f8},             // DAC/output stages (official demo)
        {0x2f, 0x003c},             // left/right mic + output mixers
        {0x04, 0x0004},             // 24 MHz / 2 / 256 = 46,875 Hz
        {0x07, 0x0002},             // I2S, 16-bit words
#if WM8960_USE_EXTERNAL_GUITAR
        {0x00, 0x0117},             // unused left PGA, 0 dB
        {0x01, 0x0127},             // buffered guitar/right PGA, about +12 dB
        {0x20, 0x0000},             // disconnect onboard left microphone
        {0x21, 0x0108},             // INPUT_R1 -> right input PGA
#else
        {0x00, 0x012f},             // left input PGA seed, about +18 dB
        {0x01, 0x012f},             // match both PGAs before enabling ALC
        {0x20, 0x0108},             // onboard microphone -> left PGA
        {0x21, 0x0000},             // right input routing
#endif
        {0x2b, 0x0000},             // left input boost mixer
        {0x2c, 0x0000},             // right input boost mixer
#if !WM8960_USE_EXTERNAL_GUITAR
        {0x11, 0x017b},             // left ALC, max +30 dB, speech target
        {0x12, 0x0032},             // min +0.75 dB, short gain-up hold
        {0x13, 0x0030},             // 192 ms decay, fastest attack/limiter
        {0x1b, 0x0000},             // ALC timing for 44.1/48 kHz family
#endif
        {0x05, 0x000c},             // official transient DAC mute setting
#if WM8960_ADC_MUTE_DIAGNOSTIC
        {0x15, 0x0100},             // minimum left ADC digital volume
        {0x16, 0x0100},             // minimum right ADC digital volume + update
#else
        {0x15, 0x01c3},             // left ADC digital volume
        {0x16, 0x01c3},             // right ADC digital volume
#endif
#if WM8960_USE_EXTERNAL_GUITAR
        {0x17, 0x01c8},             // copy right ADC to both I2S channels
        // Official Waveshare record example uses this hardware noise gate.
        // The CTIA mic-input path has a measurable idle floor, so suppress
        // it before samples ever reach the PIO/DAC path.
        {0x14, 0x00f9},
#else
        {0x17, 0x01c4},             // copy left ADC to both I2S channels
        {0x14, 0x0059},             // ALC noise gate around -60 dBFS
#endif
        {0x0a, 0x01ff},             // left DAC volume
        {0x0b, 0x01ff},             // right DAC volume
        {0x05, 0x0000},             // unmute DAC
        {0x06, 0x0000},             // de-emphasis / clocking control
        {0x10, 0x0000},             // ADC/DAC limiter control
        {0x31, 0x00f7},             // class-D/output power control
        {0x02, 0x0168},             // headphone left, clear monitor volume
        {0x03, 0x0168},             // headphone right, clear monitor volume
        {0x22, 0x0180},             // left DAC -> left output mixer
        {0x25, 0x0180},             // right DAC -> right output mixer
    };

    for (uint32_t i = 0; i < sizeof(mic) / sizeof(mic[0]); ++i) {
        if (!wm8960_write(mic[i].reg, mic[i].value)) return false;
        sleep_ms(2);
    }
    sleep_ms(100);
    return true;
}

static void i2s_tx_init(PIO pio, uint sm, uint offset) {
    pio_sm_config c = wm8960_tx_program_get_default_config(offset);
    sm_config_set_out_pins(&c, I2S_DAC_PIN, 1);
    sm_config_set_sideset_pins(&c, I2S_BCLK_PIN);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    // The official PIO program takes exactly two instructions per I2S bit.
    const float pio_hz = (float)SAMPLE_RATE * 32.0f * 2.0f;
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / pio_hz);
    pio_gpio_init(pio, I2S_DAC_PIN);
    pio_gpio_init(pio, I2S_BCLK_PIN);
    pio_gpio_init(pio, I2S_LRCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_DAC_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_BCLK_PIN, 2, true);
    pio_sm_init(pio, sm, offset, &c);
    // Start at the public setup instruction so X is valid before bitloop1.
    pio_sm_exec(pio, sm,
                pio_encode_jmp(offset + wm8960_tx_offset_entry_point));
}

static void i2s_duplex_init(PIO pio, uint sm, uint offset) {
    pio_sm_config c = wm8960_duplex_program_get_default_config(offset);
    sm_config_set_out_pins(&c, I2S_DAC_PIN, 1);
    sm_config_set_in_pins(&c, I2S_ADC_PIN);
    sm_config_set_sideset_pins(&c, I2S_BCLK_PIN);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    // The verified Arduino-Pico full-duplex implementation uses exactly
    // four PIO cycles per I2S bit.  Keeping this sequence byte-for-byte
    // equivalent is important: adding a delayed sample changed BCLK duty
    // and produced corrupted ADC words on this WM8960 board.
    const float pio_hz = (float)SAMPLE_RATE * 32.0f * 4.0f;
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / pio_hz);
    pio_gpio_init(pio, I2S_DAC_PIN);
    pio_gpio_init(pio, I2S_ADC_PIN);
    pio_gpio_init(pio, I2S_BCLK_PIN);
    pio_gpio_init(pio, I2S_LRCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_DAC_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_ADC_PIN, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_BCLK_PIN, 2, true);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_exec(pio, sm, pio_encode_set(pio_y, 14));

    // Prime the ISR shift counter to 31 bits exactly as the upstream
    // pio_i2s_inout initializer does.  The program's leading one-bit IN then
    // completes and discards a startup word, leaving every subsequent
    // autopush aligned to one complete 16-bit-left + 16-bit-right frame.
    // Without this priming, RX words remain one bit out of phase and clean
    // ADC audio appears as full-scale random noise.
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 16));
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 15));
}

static void build_sine_table(void) {
    for (uint32_t i = 0; i < SINE_TABLE_SIZE; ++i) {
        const float phase = 2.0f * (float)M_PI * (float)i /
                            (float)SINE_TABLE_SIZE;
        sine_table[i] = (int16_t)(sinf(phase) * 9000.0f);
    }
}

static void play_melody_once(PIO pio, uint sm) {
    uint32_t phase = 0;

    for (uint32_t note_index = 0;
         note_index < sizeof(melody) / sizeof(melody[0]);
         ++note_index) {
            const melody_note_t *note = &melody[note_index];
            const uint32_t frames =
                ((uint32_t)note->duration_ms * SAMPLE_RATE) / 1000u;
            const uint32_t phase_increment = note->frequency_hz > 0.0f
                ? (uint32_t)(note->frequency_hz * 4294967296.0 / SAMPLE_RATE)
                : 0u;

            phase = 0;
            for (uint32_t frame = 0; frame < frames; ++frame) {
                uint32_t gain = 32767u;
                if (frame < NOTE_ATTACK_FRAMES) {
                    gain = (frame * 32767u) / NOTE_ATTACK_FRAMES;
                }
                const uint32_t remaining = frames - frame - 1u;
                if (remaining < NOTE_RELEASE_FRAMES) {
                    const uint32_t release_gain =
                        (remaining * 32767u) / NOTE_RELEASE_FRAMES;
                    if (release_gain < gain) gain = release_gain;
                }

                int16_t sample = 0;
                if (phase_increment != 0u) {
                    const int32_t raw = sine_table[phase >> 24];
                    sample = (int16_t)((raw * (int32_t)gain) / 32767);
                    phase += phase_increment;
                }

                const uint16_t u = (uint16_t)sample;
                pio_sm_put_blocking(pio, sm, ((uint32_t)u << 16) | u);
            }
    }
}

static void report_input_levels(PIO pio, uint duplex_sm) {
    // The receive framing has been validated. Monitor the clean left ADC
    // channel (the onboard microphone) at unity gain in both headphones.
    for (uint32_t i = 0; i < 4; ++i) {
        pio_sm_put_blocking(pio, duplex_sm, 0);
    }
    pio_sm_set_enabled(pio, duplex_sm, true);

    // Discard the one startup word containing the deliberate alignment bit.
    (void)pio_sm_get_blocking(pio, duplex_sm);

#if WM8960_CAPTURE_DIAGNOSTIC
    // Let USB CDC enumerate, then capture a short contiguous input segment
    // while keeping the DAC digitally silent.  Printing occurs only after
    // PIO has stopped, so serial I/O cannot alter the recorded samples.
    pio_sm_set_enabled(pio, duplex_sm, false);
    pio_sm_clear_fifos(pio, duplex_sm);
    // Do not print the one-shot capture until the Mac has opened CDC; the
    // result is otherwise discarded before the diagnostic reader attaches.
    while (!stdio_usb_connected()) sleep_ms(10);
    sleep_ms(250);
    for (uint32_t i = 0; i < 4; ++i) {
        pio_sm_put_blocking(pio, duplex_sm, 0);
    }
    pio_sm_set_enabled(pio, duplex_sm, true);
    (void)pio_sm_get_blocking(pio, duplex_sm);
    for (uint32_t i = 0; i < CAPTURE_FRAMES; ++i) {
        const uint32_t frame = pio_sm_get_blocking(pio, duplex_sm);
        capture_samples[i] = (int16_t)(frame >> 16);
        pio_sm_put_blocking(pio, duplex_sm, 0);
    }
    pio_sm_set_enabled(pio, duplex_sm, false);
    printf("CAPTURE_BEGIN %u\n", CAPTURE_FRAMES);
    for (uint32_t i = 0; i < CAPTURE_FRAMES; ++i) {
        printf("%d\n", capture_samples[i]);
    }
    printf("CAPTURE_END\n");
    while (true) sleep_ms(1000);
#endif

#if WM8960_USE_EXTERNAL_GUITAR && !WM8960_GUITAR_LEVEL_DIAGNOSTIC
    // The guitar dry-through path must never print or perform floating-point
    // diagnostics while audio is running.  USB stdio can block for longer
    // than the four-frame PIO FIFO, producing periodic discontinuities that
    // sound like continuous crackle when the input itself is quiet.
#if WM8960_DUPLEX_TONE_TEST
    // Keep the receive side active, but throw its data away and emit a known
    // clean 440 Hz tone through the same duplex PIO state machine.  This
    // isolates PIO timing from the analogue input.
    uint32_t phase = 0;
    const uint32_t phase_increment =
        (uint32_t)(440.0f * 4294967296.0 / SAMPLE_RATE);
    while (true) {
        (void)pio_sm_get_blocking(pio, duplex_sm);
        const uint16_t tone = (uint16_t)sine_table[phase >> 24];
        phase += phase_increment;
        pio_sm_put_blocking(pio, duplex_sm, ((uint32_t)tone << 16) | tone);
    }
#elif WM8960_GUITAR_UNITY_MONITOR
    // Deliberately bypass every gain and gate stage.  This is a controlled
    // listening test for the analogue input itself.
    while (true) {
        const uint32_t adc_frame = pio_sm_get_blocking(pio, duplex_sm);
        const uint16_t raw = (uint16_t)(adc_frame >> 16);
        pio_sm_put_blocking(pio, duplex_sm, ((uint32_t)raw << 16) | raw);
    }
#else
    static bool gate_open = false;
    static uint32_t gate_hold = 0;
    static int32_t envelope = 0;
    static int32_t last_good_input = 0;
    static int32_t highpass_prev_input = 0;
    static int32_t highpass = 0;
    static int32_t lowpass_1 = 0;
    static int32_t lowpass_2 = 0;
    static uint32_t impulse_reject = 0;
    while (true) {
        const uint32_t adc_frame = pio_sm_get_blocking(pio, duplex_sm);
        int32_t input = (int16_t)(adc_frame >> 16);

        // Idle captures contain rare 3-8 sample impulses around 11000 while
        // ordinary guitar samples remain far below that level.  Suppress an
        // impulse only when the preceding envelope was quiet, preserving a
        // genuine guitar transient once the input is active.
        int32_t input_abs = input < 0 ? -input : input;
        if (!impulse_reject && envelope < 500 && input_abs > 1800) {
            impulse_reject = 12;
        }
        if (impulse_reject) {
            input = last_good_input;
            --impulse_reject;
        } else {
            last_good_input = input;
        }

        // Remove DC/bias movement, then use two inexpensive low-pass stages
        // to reduce the high-frequency hiss outside useful guitar bandwidth.
        highpass = input - highpass_prev_input +
                   ((highpass * 32700) >> 15);
        highpass_prev_input = input;
        lowpass_1 += (highpass - lowpass_1) >> 1;
        lowpass_2 += (lowpass_1 - lowpass_2) >> 1;
        int32_t amplified = lowpass_2;

        const int32_t magnitude = amplified < 0 ? -amplified : amplified;
        if (magnitude > envelope) {
            envelope += (magnitude - envelope) >> 3;
        } else {
            envelope += (magnitude - envelope) >> 10;
        }
        if (envelope >= 180) {
            gate_open = true;
            gate_hold = SAMPLE_RATE / 5u;
        } else if (gate_hold) {
            --gate_hold;
        } else if (envelope < 90) {
            gate_open = false;
        }

        if (!gate_open) amplified = 0;
        amplified *= 8;
        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;
        const uint16_t guitar = (uint16_t)(int16_t)amplified;
        pio_sm_put_blocking(
            pio, duplex_sm, ((uint32_t)guitar << 16) | guitar);
    }
#endif
#else
    uint32_t frames = 0;
    uint32_t left_peak = 0;
    uint32_t right_peak = 0;
    uint64_t left_energy = 0;
    uint64_t right_energy = 0;

    while (true) {
        const uint32_t adc_frame = pio_sm_get_blocking(pio, duplex_sm);

        const int32_t left = (int16_t)(adc_frame >> 16);
        const int32_t right = (int16_t)adc_frame;
        const uint16_t monitor = WM8960_GUITAR_LEVEL_DIAGNOSTIC
            ? 0u
            : (uint16_t)(int16_t)left;
        pio_sm_put_blocking(
            pio, duplex_sm, ((uint32_t)monitor << 16) | monitor);
        const uint32_t left_abs = (uint32_t)(left < 0 ? -left : left);
        if (left_abs > left_peak) left_peak = left_abs;
        left_energy += (uint64_t)(left * left);
        const uint32_t right_abs = (uint32_t)(right < 0 ? -right : right);
        if (right_abs > right_peak) right_peak = right_abs;
        right_energy += (uint64_t)(right * right);

        if (++frames == 4800u) {
            const uint32_t left_rms =
                (uint32_t)sqrt((double)left_energy / frames);
            const uint32_t right_rms =
                (uint32_t)sqrt((double)right_energy / frames);
#if WM8960_USE_EXTERNAL_GUITAR
            printf("GUITAR INPUT L peak=%lu rms=%lu | R peak=%lu rms=%lu\n",
#else
            printf("BOARD MIC L peak=%lu rms=%lu | R peak=%lu rms=%lu\n",
#endif
                   (unsigned long)left_peak, (unsigned long)left_rms,
                   (unsigned long)right_peak, (unsigned long)right_rms);
            frames = 0;
            left_peak = 0;
            right_peak = 0;
            left_energy = 0;
            right_energy = 0;
        }
    }
#endif
}

int main(void) {
    // Reproduce the proven playback path before starting USB or touching any
    // ADC register.  This keeps the baseline independent of USB interrupts.
    sleep_ms(1500);

    i2c_init(I2C_PORT, 400000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    const bool codec_ok = wm8960_init_playback();
    if (!codec_ok) {
        stdio_init_all();
        printf("ERROR: WM8960 did not acknowledge I2C\n");
        while (true) sleep_ms(1000);
    }

    build_sine_table();
    PIO pio = pio0;
    const uint tx_sm = pio_claim_unused_sm(pio, true);
    const uint duplex_sm = pio_claim_unused_sm(pio, true);
    const uint tx_offset = pio_add_program(pio, &wm8960_tx_program);
    const uint duplex_offset = pio_add_program(pio, &wm8960_duplex_program);
    i2s_tx_init(pio, tx_sm, tx_offset);
    i2s_duplex_init(pio, duplex_sm, duplex_offset);
    pio_sm_set_enabled(pio, tx_sm, true);
    play_melody_once(pio, tx_sm);
#if !WM8960_USE_EXTERNAL_GUITAR
    play_melody_once(pio, tx_sm);
#endif

    stdio_init_all();
    // Audio operation must never depend on a host opening the CDC port.
    // If no terminal is connected these diagnostics are simply discarded.
    printf("STAGE 1: isolated melody completed\n");
    printf("STAGE 2: I2C OK; known-good playback phase completed\n");
    printf("STAGE 3: configuring selected analogue input\n");

    if (!wm8960_enable_input()) {
        printf("ERROR: input register write failed\n");
        while (true) sleep_ms(1000);
    }

#if WM8960_USE_EXTERNAL_GUITAR
    printf("STAGE 4: CTIA guitar input enabled; starting mono monitor\n");
#else
    printf("STAGE 4: board mic enabled; starting mono monitor\n");
#endif
    pio_sm_set_enabled(pio, tx_sm, false);
    pio_sm_clear_fifos(pio, tx_sm);
    report_input_levels(pio, duplex_sm);
}
