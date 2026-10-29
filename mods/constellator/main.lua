-- 譜面の傾向：oraja-constellator（MIT。NOTICE.txt）の常設フォルダーを移したもの。
-- 難易度表 → レベル → 「すべて」と、傾向・複合・密度・プレー状況・クリア状況・スコア状況のフォルダ。中身の無いフォルダは出さない。
-- 傾向は本体が分析したもの（c.attr。まだ分析していない譜面は傾向のフォルダには入らない）

local ATTR = { RANDA = 1, UDE = 2, YUBI = 3, DELAY = 4 }
local function bit(code) return 1 << (code - 1) end

-- 傾向のフォルダ：名前と、入る条件
local TREND = {
  { "16分乱打", function(a) return a.code == ATTR.RANDA end, {
      { "中速乱打", function(a) return a.sub == 1 end },
      { "高速乱打", function(a) return a.sub == 2 end },
      { "3-3", function(a) return a.randa_33 >= 0.15 end },
      { "4-2", function(a) return a.randa_42 >= 0.15 end },
      { "5-1", function(a) return a.randa_51 >= 0.15 end },
    } },
  { "腕ガチ", function(a) return a.code == ATTR.UDE end, {
      { "全押しなし", function(a) return a.sub == 11 end },
      { "全押し交互", function(a) return a.sub == 12 end },
      { "全押し多用", function(a) return a.sub == 13 end },
    } },
  { "指ガチ・微縦連", function(a) return a.code == ATTR.YUBI end, {
      { "長縦連混じり", function(a) return a.sub == 21 end },
      { "同時押し微縦連", function(a) return a.sub == 22 end },
    } },
  { "ディレイ", function(a) return a.code == ATTR.DELAY end, {
      { "超高速乱打", function(a) return a.sub == 31 end },
      { "超高速階段", function(a) return a.sub == 32 end },
    } },
  { "判定保留", function(a) return a.code == 0 end },
}

local function both(x, y) return function(a) return a.mask & (bit(x) | bit(y)) == (bit(x) | bit(y)) end end
local COMPOSITE = {
  { "腕ガチ+指ガチ", both(ATTR.UDE, ATTR.YUBI) },
  { "腕ガチ+乱打", both(ATTR.UDE, ATTR.RANDA) },
  { "腕ガチ+ディレイ", both(ATTR.UDE, ATTR.DELAY) },
  { "指ガチ+乱打", both(ATTR.YUBI, ATTR.RANDA) },
  { "指ガチ+ディレイ", both(ATTR.YUBI, ATTR.DELAY) },
  { "乱打+ディレイ", both(ATTR.RANDA, ATTR.DELAY) },
  { "総合力", function(a)
      local n = 0
      for code = 1, 4 do if a.mask & bit(code) ~= 0 then n = n + 1 end end
      return n >= 3
    end },
}

local DAY = 24 * 60 * 60
local function played(c) return (c.playcount or 0) > 0 end
local function rate(c) return c.notes > 0 and c.exscore / (c.notes * 2) or 0 end
local function since(days) return function(c) return played(c) and (c.last_played or 0) > 0 and bmj.time() - c.last_played >= days * DAY end end

local PLAY = {
  { "未プレイ", function(c) return not played(c) end },
  { "最終プレイから7日以上", since(7) },
  { "最終プレイから15日以上", since(15) },
  { "最終プレイから30日以上", since(30) },
}
-- ランプ：1 FAILED 2 EASY 3 NORMAL 4 HARD 5 FULL COMBO 6 PERFECT
local CLEAR = {
  { "FAILED", function(c) return c.clear == 1 end },
  { "EASY以上HARD未満", function(c) return c.clear == 2 or c.clear == 3 end },
  { "HARD以上", function(c) return c.clear >= 4 end },
  { "未FC", function(c) return c.clear < 5 end },
  { "BP30以上", function(c) return played(c) and c.bp >= 30 end },
  { "BP50以上", function(c) return played(c) and c.bp >= 50 end },
  { "BP100以上", function(c) return played(c) and c.bp >= 100 end },
}
local SCORE = {
  { "A未満", function(c) return played(c) and rate(c) < 6 / 9 end },
  { "AA未満", function(c) return played(c) and rate(c) < 7 / 9 end },
  { "AAA未満", function(c) return played(c) and rate(c) < 8 / 9 end },
  { "スコア率70%未満", function(c) return played(c) and rate(c) < 0.7 end },
  { "スコア率80%未満", function(c) return played(c) and rate(c) < 0.8 end },
  { "スコア率90%未満", function(c) return played(c) and rate(c) < 0.9 end },
}

local function pick(list, ok)
  local out = {}
  for _, c in ipairs(list) do if ok(c) then out[#out + 1] = c.sha256 end end
  return out
end

-- 条件の一覧からフォルダを作る（中身の無いものは出さない）
local function group(title, list, defs, use_attr)
  local folders = {}
  for _, d in ipairs(defs) do
    local name, ok, children = d[1], d[2], d[3]
    local test = use_attr and function(c) return c.attr ~= nil and ok(c.attr) end or ok
    local charts = pick(list, test)
    if #charts > 0 then
      local f = { title = name, charts = charts }
      if children then
        local inner = {}
        for _, s in ipairs(children) do
          local sub = pick(list, function(c) return c.attr ~= nil and ok(c.attr) and s[2](c.attr) end)
          if #sub > 0 then inner[#inner + 1] = { title = name .. " " .. s[1], charts = sub } end
        end
        if #inner > 0 then
          inner[#inner + 1] = { title = name .. " すべて", charts = charts }
          f = { title = name, folders = inner }
        end
      end
      folders[#folders + 1] = f
    end
  end
  if #folders == 0 then return nil end
  return { title = title, folders = folders }
end

-- 平均密度（ノーツ数 ÷ 長さ）の、レベルの中での順位
local function density(list)
  local sorted = {}
  for _, c in ipairs(list) do
    if c.length_ms > 0 then sorted[#sorted + 1] = { c = c, d = c.notes * 1000 / c.length_ms } end
  end
  local n = #sorted
  if n < 4 then return nil end
  table.sort(sorted, function(x, y) return x.d > y.d end)
  local function slice(from, to)
    local out = {}
    for i = from, to do out[#out + 1] = sorted[i].c.sha256 end
    return out
  end
  local q, t = math.ceil(n * 0.25), math.ceil(n * 0.10)
  local folders = {
    { title = "密度 上位10%", charts = slice(1, t) },
    { title = "密度 上位25%", charts = slice(1, q) },
    { title = "密度 中央50%", charts = slice(q + 1, n - q) },
    { title = "密度 下位25%", charts = slice(n - q + 1, n) },
    { title = "密度 下位10%", charts = slice(n - t + 1, n) },
  }
  local out = {}
  for _, f in ipairs(folders) do if #f.charts > 0 then out[#out + 1] = f end end
  return { title = "密度", folders = out }
end

function build_folders(charts)
  -- 難易度表 → レベル → 譜面（7 鍵の譜面だけ。同じ譜面は 1 回）
  local tables, order = {}, {}
  for _, c in ipairs(charts) do
    if c.keys == 7 and c.tables then
      for _, t in ipairs(c.tables) do
        local tb = tables[t.table]
        if not tb then
          tb = { name = t.table, order = t.table_order, levels = {}, list = {} }
          tables[t.table] = tb
          order[#order + 1] = tb
        end
        local lv = tb.levels[t.level]
        if not lv then
          lv = { name = t.symbol .. t.level, order = t.level_order, charts = {}, seen = {} }
          tb.levels[t.level] = lv
          tb.list[#tb.list + 1] = lv
        end
        if not lv.seen[c.sha256] then
          lv.seen[c.sha256] = true
          lv.charts[#lv.charts + 1] = c
        end
      end
    end
  end
  table.sort(order, function(x, y) return x.order < y.order end)
  local root = {}
  for _, tb in ipairs(order) do
    table.sort(tb.list, function(x, y) return x.order < y.order end)
    local levels = {}
    for _, lv in ipairs(tb.list) do
      local list = lv.charts
      local folders = { { title = "すべて", charts = pick(list, function() return true end) } }
      local function add(g) if g then folders[#folders + 1] = g end end
      add(group("傾向", list, TREND, true))
      add(group("複合", list, COMPOSITE, true))
      add(density(list))
      add(group("プレー状況", list, PLAY, false))
      add(group("クリア状況", list, CLEAR, false))
      add(group("スコア状況", list, SCORE, false))
      levels[#levels + 1] = { title = lv.name, folders = folders }
    end
    if #levels > 0 then root[#root + 1] = { title = tb.name, folders = levels } end
  end
  if #root == 0 then return {} end
  return { { title = "譜面の傾向", folders = root } }
end
