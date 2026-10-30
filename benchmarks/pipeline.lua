-- pipelined GETs: wrk -s pipeline.lua URL -- <depth>
local depth = 16
function init(args)
   if args[1] then depth = tonumber(args[1]) end
   local r = {}
   for i = 1, depth do r[i] = wrk.format("GET") end
   req = table.concat(r)
end
function request() return req end
