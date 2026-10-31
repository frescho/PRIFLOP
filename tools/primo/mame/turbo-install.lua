-- Driven by turbo-install.sh, which replaces @OUT@ with its output directory.
local f = 0
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local function load(path, addr)
  local h = io.open(path, "rb"); local d = h:read("*a"); h:close()
  for i = 1, #d do mem:write_u8(addr + i - 1, d:byte(i)) end
  return #d
end
local function w(a) return mem:read_u8(a) + 256 * mem:read_u8(a + 1) end
local function dump(tag)
  local base = w(0x4174)
  local tab = base + 0x0B42
  print(string.format("%s: 40B1=%04X 4174=%04X 404A=%02X tab0E=%04X tab10=%04X tab12=%04X 40A0=%04X SP=%04X PC=%04X",
    tag, w(0x40B1), base, mem:read_u8(0x404A), w(tab+0x0E), w(tab+0x10), w(tab+0x12), w(0x40A0),
    cpu.state["SP"].value, cpu.state["PC"].value))
end
emu.register_frame_done(function()
  f = f + 1
  if f == 300 then dump("basic"); load("@OUT@/boot80.bin", 0x4500); cpu.state["PC"].value = 0x4500 end
  if f == 600 then dump("cdos"); manager.machine.video:snapshot(); load("@OUT@/turbo_body.bin", 0x7000); cpu.state["PC"].value = 0x7000 end
  if f == 900 then dump("turbo"); manager.machine.video:snapshot(); manager.machine:exit() end
end)
