#ifndef PMU_PANEL_INPUT_H
#define PMU_PANEL_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((visibility("hidden"))) int pmu_arm_current_panel(const char *socket_path);
__attribute__((visibility("hidden"))) int pmu_panel_start(const char *socket_path, const char *port_name);
__attribute__((visibility("hidden"))) int pmu_panel_stop(void);

#ifdef __cplusplus
}
#endif

#endif
