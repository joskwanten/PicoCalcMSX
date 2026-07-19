#ifndef AUDIO_HDMI_H
#define AUDIO_HDMI_H

// HDMI audio backend: streams the emulator's 48 kHz stereo output over HDMI
// Data Islands via pico_hdmi. The emulator (PSG + SCC) is configured to produce
// 48 kHz directly, so no resampling is needed.
//
// Split across cores: core 0 (the emulator) tops up a lock-free ring with
// audio_hdmi_generate(); core 1 (the HSTX loop) drains that ring into the Data
// Island queue via audio_hdmi_pump(), registered as pico_hdmi's background task
// so audio keeps flowing even while core 0 emulates a frame.

void audio_hdmi_init(void);      // reset resampler/ring state
void audio_hdmi_generate(void);  // core 0: top up the ring from the emulator
void audio_hdmi_pump(void);      // core 1: drain the ring into the DI queue

#endif // AUDIO_HDMI_H
