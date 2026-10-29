-- リコメンド：walkure-offline（MIT。NOTICE.txt）の推定を移したもの。
-- 譜面ごとのランプから地力 θ を段階反応モデルの最尤推定で求め、まだ取っていないランプの取れる確率が 20% 以上の譜面を、
-- 目標のランプごとのフォルダに確率の高い順に並べる。地力は★（発狂難易度表の換算）で親のフォルダの名前に出す
local LAMPS = { "EASY", "NORMAL", "HARD", "FULL COMBO" }  -- ランプの番号 2〜5 の順
local THETA_MIN, THETA_MAX, EPSILON = -20.0, 20.0, 1.0e-6
local MIN_PROBABILITY = 0.2

local function sigmoid(v) return 1.0 / (1.0 + math.exp(-v)) end

-- model.tsv：1 行目は★の換算表、2 行目からは md5・地力譜面度・難度 4 つ・★の難度 4 つ（- は達成者なし）
local function load_model()
  local text = bmj.read_mod_file("model.tsv")
  if not text then return nil end
  local mapping, model = nil, {}
  for line in text:gmatch("[^\n]+") do
    local cols = {}
    for v in line:gmatch("[^\t]+") do cols[#cols + 1] = v end
    if not mapping then
      mapping = {}
      for i, v in ipairs(cols) do mapping[i] = tonumber(v) end
    else
      local e = { a = tonumber(cols[2]), b = {}, star = {} }
      for i = 1, 4 do
        e.b[i] = tonumber(cols[2 + i])
        e.star[i] = tonumber(cols[6 + i])  -- "-" は nil
      end
      model[cols[1]] = e
    end
  end
  return mapping, model
end

-- 対数尤度の θ での微分。ランプの番号 k（1 FAILED … 5 FULL COMBO）の確率は sigmoid(a(θ−b[k−1])) − sigmoid(a(θ−b[k]))
local function derivative(theta, obs)
  local total = 0.0
  for _, o in ipairs(obs) do
    local e, k = o.e, o.k
    local lower = k >= 2 and sigmoid(e.a * (theta - e.b[k - 1])) or 1.0
    local upper = k <= 4 and sigmoid(e.a * (theta - e.b[k])) or 0.0
    local slope_l = e.a * lower * (1.0 - lower)
    local slope_u = e.a * upper * (1.0 - upper)
    total = total + (slope_l - slope_u) / (lower - upper)
  end
  return total
end

local function estimate(obs)
  if derivative(THETA_MAX, obs) > 0.0 or derivative(THETA_MIN, obs) < 0.0 then return nil end
  local lo, hi = THETA_MIN, THETA_MAX
  while hi - lo > EPSILON do
    local mid = (lo + hi) / 2
    if derivative(mid, obs) > 0.0 then lo = mid else hi = mid end
  end
  return hi
end

-- θ を★にする（換算表を折れ線でつなぐ。両端の外は端の線を延ばす）
local function star_of(theta, mapping)
  local n = #mapping
  local i = 1
  if theta >= mapping[n] then
    i = n - 1
  elseif theta > mapping[1] then
    for j = 1, n - 1 do
      if theta <= mapping[j + 1] then i = j break end
    end
  end
  return i + (theta - mapping[i]) / (mapping[i + 1] - mapping[i])
end

function build_folders(charts)
  local mapping, model = load_model()
  if not mapping then return {} end
  -- 同じ md5 の譜面は 1 つにまとめる（ランプは一番よいもの）
  local owned, order = {}, {}
  for _, c in ipairs(charts) do
    local md5 = c.md5:lower()
    local e = model[md5]
    if e then
      local o = owned[md5]
      if not o then
        o = { e = e, sha = c.sha256, title = c.title .. (c.subtitle ~= "" and " " .. c.subtitle or ""), clear = 0 }
        owned[md5] = o
        order[#order + 1] = o
      end
      o.clear = math.max(o.clear, math.min(c.clear, 5))  -- PERFECT は FULL COMBO
    end
  end
  local obs = {}
  for _, o in ipairs(order) do
    if o.clear >= 1 then obs[#obs + 1] = { e = o.e, k = o.clear } end
  end
  if #obs == 0 then return {} end
  local theta = estimate(obs)
  if not theta then return {} end
  local star = string.format("★%.2f", star_of(theta, mapping))

  local lists = { {}, {}, {}, {} }
  for _, o in ipairs(order) do
    for i = math.max(1, o.clear), 4 do  -- 今のランプより上のもの（FAILED・記録なしなら EASY から）
      local p = o.e.star[i] and sigmoid(o.e.a * (theta - o.e.b[i])) or 0.0
      if p >= MIN_PROBABILITY then lists[i][#lists[i] + 1] = { o = o, p = p } end
    end
  end
  local folders = {}
  for i = 1, 4 do
    local l = lists[i]
    table.sort(l, function(x, y)
      if x.p ~= y.p then return x.p > y.p end
      return x.o.title < y.o.title
    end)
    local list = {}
    for _, r in ipairs(l) do list[#list + 1] = { sha256 = r.o.sha, label = string.format("%.1f%%", r.p * 100) } end
    folders[#folders + 1] = { title = LAMPS[i], charts = list, keep_order = true }
  end
  return { { title = "リコメンド  " .. star, folders = folders } }
end
