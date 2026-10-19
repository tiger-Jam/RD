-- 判定を 1 つずつ覚えて、曲が終わったら CSV に書き出す。オートプレイでは書かない
local rows, info

local function csv(s)
  s = tostring(s or "")
  if s:find('[,"\n]') then s = '"' .. s:gsub('"', '""') .. '"' end
  return s
end

function on_play_start(i)
  info = i
  rows = {}
end

function on_judge(e)
  if not rows then return end
  rows[#rows + 1] = string.format("%.1f,%d,%s,%s,%s,%d,%d,%.1f",
    e.time_ms, e.lane, e.name, e.offset_ms or "", e.offset_ms and (e.fast and "FAST" or "SLOW") or "",
    e.combo, e.exscore, e.hp)
end

function on_play_end(r)
  if not rows or info.autoplay or info.replay then return end
  local lines = {
    "# " .. csv(info.title) .. " " .. csv(info.subtitle),
    "# sha256," .. info.sha256 .. ",gauge," .. csv(info.gauge) .. ",ex," .. r.exscore .. ",bp," .. r.bp,
    "time_ms,lane,judge,offset_ms,fast_slow,combo,exscore,hp",
  }
  for _, l in ipairs(rows) do lines[#lines + 1] = l end
  local name = bmj.date("%Y%m%d_%H%M%S") .. "_" .. info.sha256:sub(1, 12) .. ".csv"
  local ok, err = bmj.write_file(name, table.concat(lines, "\n") .. "\n")
  if ok then bmj.log("書き出した: " .. name) else bmj.log("書き出せない: " .. tostring(err)) end
  rows = nil
end
