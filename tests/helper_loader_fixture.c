#define _GNU_SOURCE
#include "helper_loader_fixture.h"
#include <dlfcn.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#ifndef FIXTURE_VERSION
#define FIXTURE_VERSION 1
#endif
#ifndef FIXTURE_ABI
#define FIXTURE_ABI 1
#endif
static PmuLoaderFixture *active;
static int start(const PmuHelperHostV1 *host,void *binding){
    if(!host||host->abi_version!=1||host->struct_size<sizeof(*host)||!binding)return 0;
    Dl_info module={0};if(!dladdr((const void *)&start,&module)||!module.dli_fname)return 0;void *owned=dlopen(module.dli_fname,RTLD_NOW|RTLD_NODELETE);if(!owned)return 0;dlclose(owned);
    active=binding;active->version=FIXTURE_VERSION;active->host=host;active->context_valid=host->helper_directory&&host->payload_directory&&host->bootstrap_script&&host->generation_id&&strlen(host->generation_id)==64&&host->architecture;
    if(active->crash)kill(getpid(),SIGKILL);
    if(active->reject==FIXTURE_VERSION){if(active->ready)host->ready(host->userdata);return 0;}
    if(active->ready)host->ready(host->userdata);
    return 1;
}
static void stop(void){if(active){++active->stopped;active->host=0;active=0;}}
const PmuHelperPayloadV1 *pmu_helper_payload_v1(void){static const PmuHelperPayloadV1 payload={FIXTURE_ABI,sizeof(PmuHelperPayloadV1),start,stop};return &payload;}
