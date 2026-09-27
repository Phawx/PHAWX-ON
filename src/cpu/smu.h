#ifndef PH_SMU_H
#define PH_SMU_H
#include "phawx.h"

enum {
    AF_UNKNOWN = -1, AF_RAVEN = 0, AF_PICASSO, AF_DALI, AF_RENOIR, AF_LUCIENNE, AF_CEZANNE,
    AF_VANGOGH, AF_REMBRANDT, AF_MENDOCINO, AF_PHOENIX, AF_HAWKPOINT,
    AF_KRACKAN, AF_STRIX, AF_STRIXHALO, AF_DRAGON, AF_FIRE, AF_COUNT
};

enum {
    S_STAPM_LIMIT, S_FAST_LIMIT, S_SLOW_LIMIT, S_APU_SLOW_LIMIT, S_STAPM_TIME, S_SLOW_TIME,
    S_TCTL_TEMP, S_MAX_GFXCLK, S_MIN_GFXCLK, S_COUNT
};

enum {
    PM_STAPM_LIM, PM_STAPM_VAL, PM_FAST_LIM, PM_FAST_VAL, PM_SLOW_LIM, PM_SLOW_VAL,
    PM_APU_SLOW_LIM, PM_TCTL_LIM, PM_TCTL_VAL, PM_COUNT
};

int      smu_detect(void);                 /* family from CPUID table, AF_UNKNOWN if not listed */
int      smu_init(void);                   /* 0 when the MP1 mailbox answers */
int      smu_family(void);
uint32_t smu_version(void);
int      smu_has(int s);
int      smu_set(int s, uint32_t v);       /* 0 ok, 1 unsupported by firmware, -1 failed */
int      smu_pm_ok(void);
int      smu_pm_refresh(void);
int      smu_pm_get(int field, float *v);

#endif
