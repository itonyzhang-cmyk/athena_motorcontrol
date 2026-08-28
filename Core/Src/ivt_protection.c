#include "ivt_protection.h"

#include <math.h>

#include "safety.h"

static uint8_t finite_value(float value)
{
    return (uint8_t)isfinite(value);
}

static float abs_max3(float a, float b, float c)
{
    float maximum = fabsf(a);
    if (fabsf(b) > maximum) maximum = fabsf(b);
    if (fabsf(c) > maximum) maximum = fabsf(c);
    return maximum;
}

uint32_t ivt_protection_evaluate(const IvtProtectionConfig *config,
                                 const IvtProtectionSample *sample)
{
    uint32_t faults = SAFETY_FAULT_NONE;

    if ((config->enabled & IVT_PROTECT_CURRENT) != 0U) {
        if (sample->current_valid == 0U ||
            finite_value(config->current_trip_a) == 0U ||
            config->current_trip_a <= 0.0f ||
            finite_value(sample->i_a) == 0U || finite_value(sample->i_b) == 0U ||
            finite_value(sample->i_c) == 0U) {
            faults |= SAFETY_FAULT_CURRENT_SENSOR;
        } else if (abs_max3(sample->i_a, sample->i_b, sample->i_c) >
                   config->current_trip_a) {
            faults |= SAFETY_FAULT_OVERCURRENT;
        }
    }

    if ((config->enabled & IVT_PROTECT_VBUS) != 0U) {
        if (sample->vbus_valid == 0U || finite_value(config->vbus_min_v) == 0U ||
            finite_value(config->vbus_max_v) == 0U || config->vbus_min_v <= 0.0f ||
            config->vbus_max_v <= config->vbus_min_v || finite_value(sample->vbus_v) == 0U) {
            faults |= SAFETY_FAULT_VBUS_SENSOR;
        } else if (sample->vbus_v < config->vbus_min_v) {
            faults |= SAFETY_FAULT_UNDERVOLTAGE;
        } else if (sample->vbus_v > config->vbus_max_v) {
            faults |= SAFETY_FAULT_OVERVOLTAGE;
        }
    }

    if ((config->enabled & IVT_PROTECT_TEMP) != 0U) {
        if (sample->temperature_valid == 0U ||
            finite_value(config->temperature_trip_c) == 0U ||
            config->temperature_trip_c <= 0.0f ||
            finite_value(sample->temperature_c) == 0U) {
            faults |= SAFETY_FAULT_TEMP_SENSOR;
        } else if (sample->temperature_c > config->temperature_trip_c) {
            faults |= SAFETY_FAULT_OVERTEMPERATURE;
        }
    }

    return faults;
}
