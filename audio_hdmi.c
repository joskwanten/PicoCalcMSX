#include "audio_hdmi.h"
#include "machine.h" // machine_get_audio

#include "pico/stdlib.h"
#include "hardware/sync.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h" // DI_HSYNC_ACTIVE

// The emulator produces 48 kHz directly (AUDIO_SAMPLE_RATE), matching the HDMI
// audio rate, so there is no resampling — samples are just moved into a ring.
// Split across cores: core 0 (the emulator) tops the ring up via
// audio_hdmi_generate(); core 1 (the HSTX loop) drains it into the Data Island
// queue via audio_hdmi_pump(), registered as pico_hdmi's background task.

// --- Lock-free SPSC ring of 48 kHz stereo frames ---
// core 0 writes at a_head, core 1 reads at a_tail.
#define ARING_BITS 12
#define ARING (1u << ARING_BITS)
#define ARING_MASK (ARING - 1)
static int16_t aring_l[ARING];
static int16_t aring_r[ARING];
static volatile uint32_t a_head = 0;
static volatile uint32_t a_tail = 0;

#define IN_CHUNK 256
static int16_t in_buf[IN_CHUNK * 2];

static inline uint32_t aring_fill(void) { return (a_head - a_tail) & ARING_MASK; }

void audio_hdmi_init(void)
{
    a_head = a_tail = 0;
}

void audio_hdmi_generate_burst(uint32_t max_samples)
{
    // Top the ring up toward half full (~40 ms of slack at 48 kHz). The batch is
    // bounded so a concurrent drain by core 1 can never make this spin forever —
    // and the caller bounds it verder: synthese draait op core 0, en een lange
    // burst midden in het zichtbare veld laat de beam-pacing achterlopen
    // (core 1 rendert dan lijnen uit nog-niet-geëmuleerde VDP-state).
    uint32_t fill = aring_fill();
    const uint32_t target = ARING / 2;
    if (fill >= target) return;
    uint32_t need = target - fill;
    if (need > max_samples) need = max_samples;

    while (need) {
        uint32_t chunk = need < IN_CHUNK ? need : IN_CHUNK;
        machine_get_audio(in_buf, chunk * 2); // 48 kHz stereo, interleaved int16
        for (uint32_t i = 0; i < chunk; i++) {
            if (((a_head + 1) & ARING_MASK) == a_tail)
                return; // ring full
            aring_l[a_head] = in_buf[i * 2];
            aring_r[a_head] = in_buf[i * 2 + 1];
            __dmb(); // sample data visible before the head advances
            a_head = (a_head + 1) & ARING_MASK;
        }
        need -= chunk;
    }
}

void audio_hdmi_generate(void)
{
    audio_hdmi_generate_burst(1200); // ~1.5 frame bij 48 kHz (vblank/opstart)
}

void __not_in_flash_func(audio_hdmi_pump)(void)
{
    static int frame_counter = 0;

    // Begrensd per aanroep: de queue in één keer vol encoderen (TERC4) kost
    // ~100+ µs en verhongert dan de lijnproducer op core 1 -> ring-misses op
    // een vást punt in het frame. Een paar islands per keer druppelt net zo
    // hard bij (de taak draait vele malen per scanlijn), zonder burst.
    int budget = 4;
    while (hstx_di_queue_get_level() < 200 && budget-- > 0) {
        if (aring_fill() < 4)
            break; // underrun: the library inserts a silence island for us

        audio_sample_t s[4];
        for (int i = 0; i < 4; i++) {
            s[i].left = aring_l[a_tail];
            s[i].right = aring_r[a_tail];
            a_tail = (a_tail + 1) & ARING_MASK;
        }

        hstx_packet_t packet;
        frame_counter = hstx_packet_set_audio_samples(&packet, s, 4, frame_counter);

        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, DI_HSYNC_ACTIVE);
        hstx_di_queue_push(&island);
    }
}
