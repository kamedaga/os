local clock = os.clock

local function metric(name, start_time)
  local elapsed = clock() - start_time
  io.write(string.format("[lua-cli-bench] metric op=%s sec=%.9f\n", name, elapsed))
end

io.write("[lua-cli-bench] start\n")

local t0 = clock()
local sum = 0
local items = {}
for i = 1, 12000 do
  local s = string.format("item-%05d:%08x", i, (i * 1103515245 + 12345) % 0x100000000)
  items[i] = s
  sum = sum + #s
end
table.sort(items)
metric("table_string_sort", t0)

t0 = clock()
local acc = 0.0
for i = 1, 30000 do
  acc = acc + math.sin(i / 17.0) * math.cos(i / 29.0) + math.log(i + 1.0)
end
metric("math_loop", t0)

t0 = clock()
local path = "/tmp/lua_cli_bench.txt"
local f = assert(io.open(path, "w+"))
for i = 1, 2048 do
  f:write(items[(i % #items) + 1], "\n")
end
assert(f:flush())
assert(f:seek("set", 0))
local lines = 0
for line in f:lines() do
  lines = lines + 1
  sum = sum + #line
end
assert(f:close())
metric("file_write_read_lines", t0)

io.write(string.format("[lua-cli-bench] checksum=%d acc=%.6f lines=%d\n", sum, acc, lines))
io.write("[lua-cli-bench] ok\n")
