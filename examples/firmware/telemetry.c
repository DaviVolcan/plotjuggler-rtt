/*
 * Minimal target-side example for plotjuggler-rtt.
 *
 * Streams one packed record per control-loop tick on RTT up-channel 1.
 * The matching data map is examples/telemetry_map.json; change one and
 * you must change the other.
 *
 * Needs the SEGGER RTT sources (SEGGER_RTT.c / SEGGER_RTT.h) in your
 * build, with SEGGER_RTT_MAX_NUM_UP_BUFFERS >= 2.
 */

#include <stdint.h>

#include "SEGGER_RTT.h"

#define TELEMETRY_CHANNEL 1u

/* Low byte is a layout version: bump it whenever a field changes, and the
 * plugin will refuse to sync with an outdated map instead of plotting
 * misaligned data. */
#define TELEMETRY_MAGIC 0xC3A55A01u

typedef struct __attribute__((packed))
{
    uint32_t magic;       /* offset 0, always TELEMETRY_MAGIC */
    uint32_t seq;         /* offset 4, +1 per record */
    float current_a;      /* offset 8 */
    float current_b;      /* offset 12 */
    float speed_rpm;      /* offset 16 */
    int16_t temp_cdeg;    /* offset 20, hundredths of a degree */
    uint8_t state;        /* offset 22 */
} telemetry_record_t;     /* 23 bytes */

_Static_assert(sizeof(telemetry_record_t) == 23, "update telemetry_map.json");

/* Large enough for a few milliseconds of records if the host stalls. */
static uint8_t rtt_buffer[4096];

void Telemetry_Init(void)
{
    /* NO_BLOCK_SKIP drops a whole record when the buffer is full, so a
     * slow host never stalls the control loop and never sees half a
     * record. */
    SEGGER_RTT_ConfigUpBuffer(TELEMETRY_CHANNEL, "Telemetry", rtt_buffer, sizeof(rtt_buffer),
                              SEGGER_RTT_MODE_NO_BLOCK_SKIP);
}

/* Call at a fixed rate, e.g. from a 1 kHz timer or task. */
void Telemetry_Send(float current_a, float current_b, float speed_rpm, float temp_c,
                    uint8_t state)
{
    static uint32_t seq = 0;

    const telemetry_record_t record = {
        .magic = TELEMETRY_MAGIC,
        .seq = seq++,
        .current_a = current_a,
        .current_b = current_b,
        .speed_rpm = speed_rpm,
        .temp_cdeg = (int16_t)(temp_c * 100.0f),
        .state = state,
    };
    SEGGER_RTT_Write(TELEMETRY_CHANNEL, &record, sizeof(record));
}
