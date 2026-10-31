-- Driven by boot-ports.sh, which replaces @OUT@ and @RUN@. Loads our own DOS's
-- boot file (BOOTD) at 0x4500 at frame 300, as the boot ROM does, and counts
-- the CPU's writes to the two IEC port ranges: frames 100-300 (BASIC idle, the
-- control: must be none) and 300-900 (the boot file: its probe of the drive).
local f = 0
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local io = cpu.spaces["io"]
local idle, boot = {}, {}
local count = nil
local function load(path, addr)
  local fh = assert(_G.io.open(path, "rb")); local d = fh:read("*a"); fh:close()
  for i = 1, #d do mem:write_u8(addr + i - 1, d:byte(i)) end
end
local function w(a) return mem:read_u8(a) + 256 * mem:read_u8(a + 1) end
local function ranges(t)
  local r40, r80 = 0, 0
  for p, n in pairs(t) do
    if p >= 0x40 and p < 0x80 then r40 = r40 + n elseif p >= 0x80 and p < 0xC0 then r80 = r80 + n end
  end
  return r40, r80
end
tap = io:install_write_tap(0x00, 0xFF, "ports", function(offset, data, mask)
  if count then local p = offset & 0xFF; count[p] = (count[p] or 0) + 1 end
end)
emu.register_frame_done(function()
  f = f + 1
  if f == 100 then count = idle end
  if f == 300 then
    print(string.format("@RUN@ basic: 40B1=%04X 4174=%04X rom029F=%02X rom0687=%02X",
      w(0x40B1), w(0x4174), mem:read_u8(0x029F), mem:read_u8(0x0687)))
    count = boot
    load("@OUT@/@RUN@.bin", 0x4500); cpu.state["PC"].value = 0x4500
  end
  if f == 900 then
    local i40, i80 = ranges(idle)
    local b40, b80 = ranges(boot)
    print(string.format("@RUN@ after: 40B1=%04X 4174=%04X outport=%02X idle40=%d idle80=%d boot40=%d boot80=%d PC=%04X",
      w(0x40B1), w(0x4174), mem:read_u8(@OUTPORT@), i40, i80, b40, b80, cpu.state["PC"].value))
    manager.machine.video:snapshot(); manager.machine:exit()
  end
end)
