# WM8978 loopback firmware snapshots

- `WM8978_LOOPBACK_NO_GATE_GAIN1_5.UF2` — clean baseline saved on 2026-08-30.
  It uses the WM8978 L2 line input, removes the per-sample noise gate that
  caused fuzzy zero-crossing distortion, and applies 1.5× digital gain.
- `WM8978_NAM2_JOHN_MAYER_CLEAN.UF2` — NAM2 real-time firmware built on
  2026-08-31. It embeds `example.nam` (JOHN MAYER CLEAN-001), uses the WM8978
  L2/R2 line input, DMA full-duplex I2S, and the existing dual-core A2-Lite
  inference engine at 300 MHz. BOOTSEL toggles NAM/bypass after boot.
- `WM8978_NAM2_BE100_HBE_MAMMOTH.UF2` — same firmware with the higher-gain
  `[AMP] BE100DLX-HBE Mammoth - SM58` model for an obvious A/B test. The source
  model is now `example.nam`; the John Mayer source is backed up below.

Model backup: `JOHN_MAYER_CLEAN_001.nam`.

To flash the NAM2 image, put the Pico 2 W into BOOTSEL and copy this UF2 to the
`RP2350` drive. The original loopback images remain available for rollback.
