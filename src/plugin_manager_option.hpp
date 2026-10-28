#pragma once
// The optional on-track plugin manager the welcome screen offers instead of this USB manager. It is installed from the
// catalog entry MPC_PM_ID. Its release is found by TAG PREFIX, not by the repository's "latest" release, because the
// repository publishes other things too: the release for catalog version V is the tag MPC_PM_TAG_PREFIX V, and its
// package is MPC_PM_ASSET_PREFIX V -mpc-<armv7|aarch64>.zip. Upstream offered poloq's Plugin Manager here.
#ifndef MPC_PM_ID
#define MPC_PM_ID "plugin-manager-g2"
#endif
#ifndef MPC_PM_NAME
#define MPC_PM_NAME "Plugin Manager G2"
#endif
#ifndef MPC_PM_REPO
#define MPC_PM_REPO "sd88me/mpc-vst-plugins"
#endif
#ifndef MPC_PM_TAG_PREFIX
#define MPC_PM_TAG_PREFIX "plugin-manager-g2-v"
#endif
#ifndef MPC_PM_ASSET_PREFIX
#define MPC_PM_ASSET_PREFIX "Plugin-Manager-G2-"
#endif
