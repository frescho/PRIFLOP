-- Driven by boot-turbo.sh, which replaces @OUT@ and @IMG@ (the image: boot or boot_forced).
local f = 0
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local function load(path, addr)
  local h = io.open(path, "rb"); local d = h:read("*a"); h:close()
  for i = 1, #d do mem:write_u8(addr + i - 1, d:byte(i)) end
end
local function w(a) return mem:read_u8(a) + 256 * mem:read_u8(a + 1) end
local function dump(tag)
  local base = w(0x4174)
  local tab = base + 0x0B42
  print(string.format("%s: 40B1=%04X 4174=%04X tab0E=%04X tab10=%04X tab12=%04X SP=%04X PC=%04X IX=%04X",
    tag, w(0x40B1), base, w(tab+0x0E), w(tab+0x10), w(tab+0x12),
    cpu.state["SP"].value, cpu.state["PC"].value, cpu.state["IX"].value))
end
emu.register_frame_done(function()
  f = f + 1
  if f == 300 then dump("@IMG@ basic"); load("@OUT@/@IMG@.bin", 0x4500); cpu.state["PC"].value = 0x4500 end
  if f == 900 then dump("@IMG@ boot1"); manager.machine.video:snapshot(); load("@OUT@/@IMG@.bin", 0x4500); cpu.state["PC"].value = 0x4500 end
  if f == 1500 then dump("@IMG@ boot2"); manager.machine.video:snapshot(); manager.machine:exit() end
end)
