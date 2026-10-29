#pragma once
#define MPC_MANAGER_VERSION "1.3.0"
#ifndef MPC_MANAGER_DEVELOPMENT
#define MPC_MANAGER_DEVELOPMENT 1
#endif
static_assert(MPC_MANAGER_DEVELOPMENT == 0 || MPC_MANAGER_DEVELOPMENT == 1, "Invalid manager build identity");
