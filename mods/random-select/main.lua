-- フォルダの名前の下に ????? のバーを出す。決定すると、今の一覧に見えている譜面（鍵盤数・難易度の絞り込みのあと）から
-- 1 つを選ぶ。選ばれた譜面にカーソルが移って、そのまま決定画面へ進む

function folder_bars(folder)
  if folder.charts == 0 then return {} end  -- 譜面の無いフォルダ（フォルダだけが並ぶ階層・コース）には出さない
  -- 選んでいる間は、曲名の枠に一覧の曲名がルーレットのように次々に出る
  return { { key = "random", title = "?????", roulette = true } }
end

function pick_chart(req)
  -- 決定したときにルーレットに出ていた曲を遊ぶ
  if req.shown then return req.shown end
  local charts = req.charts
  if #charts == 0 then return nil end
  return charts[math.random(#charts)].sha256
end
