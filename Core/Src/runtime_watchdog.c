#include "runtime_watchdog.h"

#if defined(GD32F30X_HD)
#include "gd32f30x.h"
#endif

/* The normal main loop includes a one-second diagnostic interval. With the
 * nominal 40 kHz LSI, DIV256 and reload 0x1FF give about 3.3 s; LSI tolerance
 * still leaves room for that interval while making a stalled main loop reset
 * promptly. FWDGT cannot be disabled once enabled. */
#define RUNTIME_WATCHDOG_RELOAD 0x01FFU

static volatile uint32_t main_progress;
static uint32_t observed_progress;
static uint8_t watchdog_active;

uint8_t runtime_watchdog_should_reload(uint32_t observed, uint32_t current)
{
    return (uint8_t)(observed != current);
}

int runtime_watchdog_init(void)
{
    main_progress = 0U;
    observed_progress = 0U;
    watchdog_active = 0U;
#if defined(GD32F30X_HD)
    if (fwdgt_config(RUNTIME_WATCHDOG_RELOAD, FWDGT_PSC_DIV256) != SUCCESS) {
        return -1;
    }
    fwdgt_enable();
    watchdog_active = 1U;
    return 0;
#else
    return 0;
#endif
}

void runtime_watchdog_main_heartbeat(void)
{
    main_progress++;
}

void runtime_watchdog_timer_service(void)
{
#if defined(GD32F30X_HD)
    const uint32_t current = main_progress;
    if (watchdog_active != 0U &&
        runtime_watchdog_should_reload(observed_progress, current) != 0U) {
        observed_progress = current;
        fwdgt_counter_reload();
    }
#endif
}

uint8_t runtime_watchdog_active(void)
{
    return watchdog_active;
}
