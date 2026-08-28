#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "runtime_watchdog.h"

int main(void)
{
    assert(runtime_watchdog_should_reload(0U, 0U) == 0U);
    assert(runtime_watchdog_should_reload(0U, 1U) == 1U);
    assert(runtime_watchdog_should_reload(UINT32_MAX, 0U) == 1U);

    /* The host build has no GD32 FWDGT backend.  It must remain an explicit
     * no-op rather than pretending that a hardware watchdog is armed. */
    assert(runtime_watchdog_init() == 0);
    assert(runtime_watchdog_active() == 0U);
    runtime_watchdog_main_heartbeat();
    runtime_watchdog_timer_service();
    assert(runtime_watchdog_active() == 0U);
    puts("runtime watchdog progress tests: PASS");
    return 0;
}
