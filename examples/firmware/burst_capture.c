/*
 * Burst capture example for plotjuggler-rtt.
 *
 * The control loop runs at 8 kHz but the RTT link only has room for one
 * record per millisecond. Live telemetry therefore shows every 8th sample.
 * Around an event of interest (a setpoint step, a fault), this code keeps
 * every sample in RAM and sends the burst afterwards, one record per
 * millisecond, tagged with the dump magic. Each dump record carries the
 * tick at which it was sampled, so the plugin places it at the right
 * instant and the plot shows the full 8 kHz detail on top of the live data.
 *
 * The matching data map is examples/burst_map.json.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "SEGGER_RTT.h"

#define TELEMETRY_CHANNEL 1u
#define TELEMETRY_MAGIC 0xC3A55A02u
#define TELEMETRY_DUMP_MAGIC 0xC3A55A82u /* same layout, marks a replayed sample */

#define CONTROL_HZ 8000u
#define BURST_PRE 8u    /* samples up to and including the trigger tick (1 ms) */
#define BURST_POST 120u /* samples recorded after it (15 ms) */
#define BURST_LEN (BURST_PRE + BURST_POST)

typedef struct __attribute__((packed))
{
    uint32_t magic;  /* offset 0 */
    uint32_t seq;    /* offset 4, control-loop tick (8 kHz) */
    float current_a; /* offset 8 */
    float current_b; /* offset 12 */
    float speed_rpm; /* offset 16, live only */
} telemetry_record_t; /* 20 bytes */

_Static_assert(sizeof(telemetry_record_t) == 20, "update burst_map.json");

typedef struct
{
    float current_a;
    float current_b;
} burst_sample_t;

static uint8_t rtt_buffer[4096];

/* Ring buffer written by the control ISR until the burst is complete. */
static burst_sample_t ring[BURST_LEN];
static uint32_t written;          /* samples written since re-arming */
static uint32_t stop_at;          /* value of `written` that completes the burst, 0 = armed */
static uint32_t last_tick;        /* tick of the newest sample */
static volatile bool burst_ready; /* set by the ISR, cleared by the task after replay */
static uint32_t replayed;

void Telemetry_Init(void)
{
    SEGGER_RTT_ConfigUpBuffer(TELEMETRY_CHANNEL, "Telemetry", rtt_buffer, sizeof(rtt_buffer),
                              SEGGER_RTT_MODE_NO_BLOCK_SKIP);
}

/* Control ISR, every tick. Cheap: one struct copy per tick. */
void Burst_OnControlTick(uint32_t tick, float current_a, float current_b)
{
    if (burst_ready)
    {
        return; /* frozen until the task has sent it */
    }
    ring[written % BURST_LEN] = (burst_sample_t){current_a, current_b};
    last_tick = tick;
    written++;
    if (stop_at != 0u && written == stop_at)
    {
        burst_ready = true;
    }
}

/* Call from the control ISR when the event happens, e.g. on a setpoint
 * change. Ignored while a burst is being recorded or sent. */
void Burst_Trigger(void)
{
    if (stop_at == 0u && !burst_ready && written >= BURST_PRE)
    {
        stop_at = written + BURST_POST;
    }
}

/* Next replay record, or false when there is no complete burst to send. */
static bool Burst_NextRecord(telemetry_record_t* record)
{
    if (!burst_ready)
    {
        return false;
    }
    if (replayed == BURST_LEN)
    {
        /* Re-arm. stop_at first: the ISR does not touch the buffer until
         * burst_ready goes false. */
        replayed = 0u;
        stop_at = 0u;
        written = 0u;
        burst_ready = false;
        return false;
    }
    const uint32_t oldest = written - BURST_LEN;
    const burst_sample_t* s = &ring[(oldest + replayed) % BURST_LEN];
    memset(record, 0, sizeof(*record));
    record->magic = TELEMETRY_DUMP_MAGIC;
    record->seq = last_tick - (BURST_LEN - 1u) + replayed;
    record->current_a = s->current_a;
    record->current_b = s->current_b;
    replayed++;
    return true;
}

/* Telemetry task, every millisecond (every 8th control tick). While a
 * burst is pending its records take the place of the live one, so the
 * link never carries more than one record per millisecond. */
void Telemetry_Task1ms(uint32_t tick, float current_a, float current_b, float speed_rpm)
{
    telemetry_record_t record;
    if (!Burst_NextRecord(&record))
    {
        record = (telemetry_record_t){
            .magic = TELEMETRY_MAGIC,
            .seq = tick,
            .current_a = current_a,
            .current_b = current_b,
            .speed_rpm = speed_rpm,
        };
    }
    SEGGER_RTT_Write(TELEMETRY_CHANNEL, &record, sizeof(record));
}
