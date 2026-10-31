-- An original .ptp from cassette in MAME's primob64 (the reference for a
-- converted file): LOAD typed, RETURN, play pressed; a screen every 30 s
-- until @SECS@ s. Driven by hand, which replaces @OUT@ and @SECS@.
local f = 0
local function key(name)                    -- a key field by its name, in any port
  for _, port in pairs(manager.machine.ioport.ports) do
    for fname, fl in pairs(port.fields) do if fname == name then return fl end end
  end
end
-- LOAD typed key by key: MAME's text typing dropped and mixed letters here ("LoA")
local typed = { key("l  L"), key("o  O"), key("a  A"), key("d  D") }
local RET = manager.machine.ioport.ports[":IN3"].fields["RETURN"]
local cas = manager.machine.cassettes[":cassette"]
local log = io.open("@OUT@/log.txt", "w")
emu.register_frame_done(function()
  f = f + 1
  for i, fl in ipairs(typed) do
    local at = 250 + (i - 1) * 12
    if f == at then fl:set_value(1) elseif f == at + 5 then fl:set_value(0) end
  end
  if f == 320 then RET:set_value(1) end
  if f == 326 then RET:set_value(0); cas:play(); log:write("play\n") end
  if f > 326 and f % 1500 == 0 then
    manager.machine.video:snapshot()
    log:write(string.format("%ds pos %.1f/%.1f motor %s\n", f // 50, cas.position, cas.length, tostring(cas.motor_state)))
    log:flush()
  end
  if f == @SECS@ * 50 then log:close(); manager.machine:exit() end
end)
