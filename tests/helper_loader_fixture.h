#pragma once
#include "helper_payload.h"
typedef struct {
    int ready,reject,crash,version,stopped,context_valid;
    const PmuHelperHostV1 *host;
} PmuLoaderFixture;
