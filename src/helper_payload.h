#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t abi_version,struct_size;
    const char *helper_directory,*payload_directory,*bootstrap_script,*generation_id,*architecture;
    void *userdata;
    void (*ready)(void *userdata);
} PmuHelperHostV1;
typedef struct {
    uint32_t abi_version,struct_size;
    int (*start)(const PmuHelperHostV1 *host,void *binding);
    void (*stop)(void);
} PmuHelperPayloadV1;
typedef const PmuHelperPayloadV1 *(*PmuHelperPayloadQueryV1)(void);
#ifdef __cplusplus
}
#endif
