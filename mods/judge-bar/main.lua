-- 判定のずれのバー。押すたびにずれの位置へ印を出し、しばらくで消える。上の三角は最近のずれの平均
-- 置き場所と大きさは下の値で変える（画面の幅・高さに対する割合）
local X = 0.5        -- バーの真ん中（横）
local Y = 0.70       -- バーの真ん中（縦）
local WIDTH = 0.25   -- バーの幅
local HEIGHT = 0.012 -- バーの太さ
local LIFE = 3000    -- 印が消えるまで（ms）
local KEEP = 40      -- 覚えておく印の数

local win = { pgreat = {18, 18}, great = {40, 40}, good = {100, 100} }
local marks = {}
local avg = nil

function on_play_start(info)
  win = info.windows
  marks = {}
  avg = nil
end

function on_judge(e)
  if e.mine or not e.offset_ms then return end
  marks[#marks + 1] = { offset = e.offset_ms, judge = e.judge, at = e.time_ms }
  if #marks > KEEP then table.remove(marks, 1) end
  avg = avg and (avg * 0.9 + e.offset_ms * 0.1) or e.offset_ms
end

local COLORS = { [5] = 0x60e0ff, [4] = 0xffe040, [3] = 0x60ff60, [2] = 0xff8040 }

function on_draw(s)
  local w, h = s.view_w * WIDTH, math.max(4, s.view_h * HEIGHT)
  local cx, cy = s.view_w * X, s.view_h * Y
  local range = math.max(win.good[1], win.good[2])
  local function px(ms) return cx + ms / range * (w / 2) end
  -- 帯：GOOD・GREAT・PGREAT の幅
  rd.draw_rect(px(-win.good[1]), cy - h / 2, px(win.good[2]) - px(-win.good[1]), h, 0x40c040, 110)
  rd.draw_rect(px(-win.great[1]), cy - h / 2, px(win.great[2]) - px(-win.great[1]), h, 0xc0a020, 140)
  rd.draw_rect(px(-win.pgreat[1]), cy - h / 2, px(win.pgreat[2]) - px(-win.pgreat[1]), h, 0x30a0d0, 170)
  rd.draw_rect(cx - 1, cy - h, 2, h * 2, 0xffffff, 230)
  -- 印（新しいほど濃い）
  for _, m in ipairs(marks) do
    local age = s.song_ms - m.at
    if age < LIFE then
      local x = px(math.max(-range, math.min(range, m.offset)))
      local a = math.floor(255 * (1 - age / LIFE))
      rd.draw_rect(x - 1, cy - h * 1.6, 2, h * 3.2, COLORS[m.judge] or 0xffffff, a)
    end
  end
  -- 平均
  if avg then
    local x = px(math.max(-range, math.min(range, avg)))
    for i = 0, 4 do rd.draw_rect(x - (5 - i), cy - h * 2.4 - i, (5 - i) * 2, 1, 0xffffff, 230) end
  end
end
