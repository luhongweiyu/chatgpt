#pragma once

// Keep the original uploaded header private, but rename its public symbols so
// it can coexist in the same x86 process with the recovered implementation.
#define dmsoft legacy_dmsoft
#define LoadDm LegacyLoadDm
#define LoadDmW LegacyLoadDmW
#define FreeDm LegacyFreeDm
#include "private/legacy_dm.h"
#undef FreeDm
#undef LoadDmW
#undef LoadDm
#undef dmsoft
