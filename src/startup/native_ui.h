#ifndef PMU_NATIVE_UI_H
#define PMU_NATIVE_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PmuUi PmuUi;
typedef struct PmuUiHandle PmuUiHandle;
typedef struct PmuUiLoadingHandle PmuUiLoadingHandle;
typedef void (*PmuUiTask)(void *);
typedef void (*PmuUiResult)(int, void *);

PmuUi *pmu_ui_init(void *lua_state, uintptr_t image_base);
void pmu_ui_stop(PmuUi *ui);
int pmu_ui_post(PmuUi *ui, PmuUiTask task, void *data, PmuUiTask destroy);
PmuUiHandle *pmu_ui_show(PmuUi *ui, const char *title, const char *message,
                       PmuUiResult result, void *data);
void pmu_ui_dismiss(PmuUi *ui, PmuUiHandle *handle);
PmuUiLoadingHandle *pmu_ui_loading_begin(PmuUi *ui, const char *title);
void pmu_ui_loading_end(PmuUi *ui, PmuUiLoadingHandle *handle);

#ifdef __cplusplus
}
#endif

#endif
