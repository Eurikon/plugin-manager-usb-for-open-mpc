local ENABLED = false
local info = debug and debug.getinfo and debug.getinfo(1, "S")
local directory = info and info.source and info.source:match("^@(.*/)")
local wide = string.packsize and string.packsize("T") == 8
local MODULE = directory and directory .. (wide and "manager-entry-aarch64.so" or "manager-entry.so")
local stop

local function start()
    if not ENABLED or not MODULE or not mpc or not mpc.ui or not mpc.ui.popups
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
