#ifndef INC_IVT_PROTECTION_H_
#define INC_IVT_PROTECTION_H_

#include <stdint.h>

/* Protection measurements are deliberately opt-in.  A non-zero threshold is
 * not enough: enable bits must be set only after the corresponding sensing
 * path and scale have been verified on the actual hardware. */
#define IVT_PROTECT_CURRENT (1U << 0)
#define IVT_PROTECT_VBUS    (1U << 1)
#define IVT_PROTECT_TEMP    (1U << 2)

typedef struct {
    uint32_t enabled;
    float current_trip_a;
    float vbus_min_v;
    float vbus_max_v;
    float temperature_trip_c;
} IvtProtectionConfig;

typedef struct {
    float i_a;
    float i_b;
    float i_c;
    float vbus_v;
    float temperature_c;
    uint8_t current_valid;
    uint8_t vbus_valid;
    uint8_t temperature_valid;
} IvtProtectionSample;

/* Returns the SafetyFault bit mask to latch.  It has no hardware side effects,
 * allowing every threshold and invalid-sensor path to be host-tested. */
uint32_t ivt_protection_evaluate(const IvtProtectionConfig *config,
                                 const IvtProtectionSample *sample);

#endif /* INC_IVT_PROTECTION_H_ */
