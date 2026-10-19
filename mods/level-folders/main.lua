-- 譜面の数字（ノーツ数・長さ・TOTAL・BPM）だけから難易度をざっくり推定し、段階ごとのフォルダにする。
-- #PLAYLEVEL はあてにしない。推定はおおまか（譜面の中身の偏りは見ていない）なので、見本として使う
local STEPS = 12  -- 段階の数

-- 推定の値：1 秒あたりのノーツ数を中心に、TOTAL が小さい（ゲージが重い）ほど、BPM の幅が大きいほど上げる
local function estimate(c)
  local sec = c.length_ms / 1000
  if sec < 20 or c.notes < 50 then return nil end
  local nps = c.notes / sec
  local total = c.total > 0 and c.total or 300
  local gauge = math.max(0.8, math.min(1.3, (c.notes / total) / 4))  -- 1 ノーツの重さ
  local soflan = (c.bpm_min > 0 and c.bpm_max / c.bpm_min > 1.5) and 1.1 or 1.0
  return nps * gauge * soflan
end

function build_folders(charts)
  local scored = {}
  for _, c in ipairs(charts) do
    if c.keys == 7 then
      local v = estimate(c)
      if v then scored[#scored + 1] = { sha = c.sha256, v = v } end
    end
  end
  if #scored == 0 then return {} end
  -- 持っている譜面を推定の値の順に並べ、同じ数ずつ段階に分ける（持っている譜面に対する相対の段階）
  table.sort(scored, function(a, b) return a.v < b.v end)
  local folders = {}
  for i = 1, STEPS do folders[i] = { title = string.format("推定 %02d", i), charts = {} } end
  for i, s in ipairs(scored) do
    local step = math.min(STEPS, math.floor((i - 1) * STEPS / #scored) + 1)
    local f = folders[step].charts
    f[#f + 1] = s.sha
  end
  return { { title = "推定難易度（7 鍵）", folders = folders } }
end
