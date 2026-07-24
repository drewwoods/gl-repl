#include "app/boot/glr_frame_pacer.h"

#include <math.h>

#define GLR_FRAME_PERIOD_MS_EXACT (1000.0 / 60.0)

int glr_frame_pacer_next_delay_ms(GlrFramePacer *pacer, double now_ms) {
    int delay;

    if (!pacer)
        return 1;

    if (pacer->next_deadline_ms == 0.0)
        pacer->next_deadline_ms = now_ms;
    pacer->next_deadline_ms += GLR_FRAME_PERIOD_MS_EXACT;

    if (pacer->next_deadline_ms < now_ms)
        pacer->next_deadline_ms = now_ms;

    delay = (int)lround(pacer->next_deadline_ms - now_ms);
    return delay < 1 ? 1 : delay;
}
