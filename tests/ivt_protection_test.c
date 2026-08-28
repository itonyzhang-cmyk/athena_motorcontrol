#include <assert.h>
#include <math.h>

#include "ivt_protection.h"
#include "safety.h"

void ivt_protection_tests(void)
{
    const IvtProtectionConfig disabled = {0};
    const IvtProtectionSample nominal = {
        .i_a = 1.0f, .i_b = -2.0f, .i_c = 1.0f, .vbus_v = 24.0f,
        .temperature_c = 30.0f, .current_valid = 1U, .vbus_valid = 1U,
        .temperature_valid = 1U
    };
    IvtProtectionConfig config = {0};
    IvtProtectionSample sample = nominal;

    assert(ivt_protection_evaluate(&disabled, &nominal) == SAFETY_FAULT_NONE);

    config.enabled = IVT_PROTECT_CURRENT;
    config.current_trip_a = 5.0f;
    assert(ivt_protection_evaluate(&config, &nominal) == SAFETY_FAULT_NONE);
    sample.i_b = -5.01f;
    assert(ivt_protection_evaluate(&config, &sample) == SAFETY_FAULT_OVERCURRENT);
    sample = nominal;
    sample.current_valid = 0U;
    assert(ivt_protection_evaluate(&config, &sample) == SAFETY_FAULT_CURRENT_SENSOR);

    config.enabled = IVT_PROTECT_VBUS;
    config.vbus_min_v = 18.0f;
    config.vbus_max_v = 30.0f;
    sample = nominal;
    sample.vbus_v = 17.9f;
    assert(ivt_protection_evaluate(&config, &sample) == SAFETY_FAULT_UNDERVOLTAGE);
    sample.vbus_v = 30.1f;
    assert(ivt_protection_evaluate(&config, &sample) == SAFETY_FAULT_OVERVOLTAGE);
    sample.vbus_v = NAN;
    assert(ivt_protection_evaluate(&config, &sample) == SAFETY_FAULT_VBUS_SENSOR);

    config.enabled = IVT_PROTECT_TEMP;
    config.temperature_trip_c = 80.0f;
    sample = nominal;
    sample.temperature_c = 80.1f;
    assert(ivt_protection_evaluate(&config, &sample) == SAFETY_FAULT_OVERTEMPERATURE);
    sample.temperature_valid = 0U;
    assert(ivt_protection_evaluate(&config, &sample) == SAFETY_FAULT_TEMP_SENSOR);

    config.enabled = IVT_PROTECT_CURRENT | IVT_PROTECT_VBUS;
    config.current_trip_a = 0.0f;
    config.vbus_min_v = 18.0f;
    config.vbus_max_v = 30.0f;
    sample = nominal;
    sample.vbus_v = 31.0f;
    assert(ivt_protection_evaluate(&config, &sample) ==
           (SAFETY_FAULT_CURRENT_SENSOR | SAFETY_FAULT_OVERVOLTAGE));
}
