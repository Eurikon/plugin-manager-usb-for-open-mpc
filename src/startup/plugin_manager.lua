local ENABLED = false
local MODULE = "/sdcard/MPC Documents/Devices/Plugin Manager helper/manager-entry.so"
local stop

local function start()
    if not ENABLED or not mpc or not mpc.ui or not mpc.ui.popups
        or not debug or type(debug.getupvalue) ~= "function" then
        return "unavailable"
    end
    local popup = mpc.ui.popups.ShowInfoPopup
    if type(popup) ~= "function" then return "unavailable" end
    local _, binding = debug.getupvalue(popup, 2)
    if type(binding) ~= "userdata" then return "unavailable" end
    local loaded, entry = pcall(package.loadlib, MODULE, "pmu_start_manager")
    if not loaded or type(entry) ~= "function" then return "unavailable" end
    local stopped, callback = pcall(package.loadlib, MODULE, "pmu_stop_manager")
    if not stopped or type(callback) ~= "function" then return "unavailable" end
    stop = callback
    return pcall(entry, binding) and "helper-called" or "helper-error"
end

if plugin_manager ~= nil then
    function plugin_manager.OnLoad() return start() end
    function plugin_manager.OnUnload()
        if stop then pcall(stop) end
    end
end
return "helper-registered"
