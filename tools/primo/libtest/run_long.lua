-- One library program in MAME's primob64, loaded the way our DOS's CMD LOAD
-- leaves it (the drive's record loop, src/pico/primo_turbo.c): D1 at TXTTAB
-- plus its offset, D9 at its own address, D5 on the screen; C3 jumps; after a
-- C9 a program is linked and RUN is typed. Screens at fixed times, a key in
-- between. Driven by run.py, which replaces @PRG@, @OUT@ and @CDOS@.
-- With @CDOS@ a file (run2: CDOS's boot80.prg, never shipped) it is first
-- installed the way the boot ROM does it: loaded at 4500h and started; then
-- (run_long.lua: no keys, 120 s, a screen every 20 s.)
local f, cpu = 0, manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local function w16(a) return mem:read_u8(a) + 256 * mem:read_u8(a + 1) end
local function put16(a, v) mem:write_u8(a, v & 255); mem:write_u8(a + 1, v >> 8) end
local log = io.open("@OUT@/log.txt", "w")
local function say(s) log:write(s .. "\n"); log:flush() end
local shot = 0
local function snap(tag)
  shot = shot + 1
  manager.machine.video:snapshot()
  say(string.format("shot %d %s PC=%04X SP=%04X", shot, tag, cpu.state["PC"].value, cpu.state["SP"].value))
end
local mode
local function load()
  local h = io.open("@PRG@", "rb"); local d = h:read("*a"); h:close()
  local i, txttab, scr = 1, w16(0x40A4), mem:read_u8(0x404A)
  local fin, last = txttab, 0
  while i <= #d do
    local t = d:byte(i)
    if t == 0xC9 then mode = "basic"; break end
    if t == 0xC3 or t == 0xCD then
      local a = d:byte(i + 1) + 256 * d:byte(i + 2)
      if t == 0xC3 then mode = string.format("jump %04X", a); cpu.state["PC"].value = a; break end
      i = i + 3
    else
      local off = d:byte(i + 1) + 256 * d:byte(i + 2)
      local ln = d:byte(i + 3) + 256 * d:byte(i + 4)
      local dest = (t == 0xD1) and (txttab + off) or (t == 0xD5) and (scr * 256 + off) or off
      for k = 0, ln - 1 do mem:write_u8((dest + k) & 0xFFFF, d:byte(i + 5 + k)) end
      say(string.format("record %02X at %04X, %d bytes", t, dest, ln))
      last = t; fin = dest + ln
      i = i + 5 + ln
    end
  end
  if mode == "basic" and last ~= 0xD5 then
    put16(0x40F9, fin); put16(0x40FB, fin); put16(0x40FD, fin)
  end
  if mode == "basic" then                     -- as CDOS: RUN only if a record set the current line
    local cl = w16(0x40A2)
    say(string.format("CURLIN %04X", cl))
    if cl == 0xFFFF then mode = "basic, not started" end
  end
  say("mode " .. tostring(mode))
end
local kb = manager.machine.natkeyboard
local RET = manager.machine.ioport.ports[":IN3"].fields["RETURN"]
local SPC = manager.machine.ioport.ports[":IN1"].fields["Space"]
local function press(fl, at, now)
  if now == at then fl:set_value(1) elseif now == at + 6 then fl:set_value(0) end
end
local CDOS = "@CDOS@"
local T = (CDOS ~= "") and 500 or 0          -- everything later by this much
local function install()
  local h = io.open(CDOS, "rb"); local d = h:read("*a"); h:close()
  for i = 1, #d do mem:write_u8(0x4500 + i - 1, d:byte(i)) end
  cpu.state["PC"].value = 0x4500
  say("cdos installed from 4500")
end
-- run_long: no keys; a screen every 20 s for 120 s, so a demo that runs by itself shows it
local shots_at = {500, 1500, 2500, 3500, 4500, 5500, 6250}
emu.register_frame_done(function()
  f = f + 1
  if T > 0 and f == 200 then install() end
  if f == 250 + T then load() end
  if f == 260 + T and mode == "basic" then kb:post("RUN") end
  if mode == "basic" then press(RET, 320 + T, f) end
  for _, at in ipairs(shots_at) do
    if f == at + 250 + T then snap(string.format("%ds", math.floor(at / 50))) end
  end
  if f == 6250 + 260 + T then log:close(); manager.machine:exit() end
end)
