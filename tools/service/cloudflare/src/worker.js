// RD の受け口。配った RD が新しい版を取り、ログを送ってくる。Mac mini は外からは受けず、ここへ取りに来る。
//   GET  /update/<ファイル>             新しい版の情報（latest.json）と zip（R2 の update/ の下）
//   POST /log?client=&name=&version=    ログ（本文がファイルの中身）。R2 の logs/<client>/<name> に置く
//   GET  /admin/logs?cursor=            置いてあるログの一覧（Mac mini から。X-RD-Admin が要る）
//   GET  /admin/object?key=             ログを 1 つ取る
//   DELETE /admin/object?key=           ログを 1 つ消す（取ったあとに）
// 頭に /rd が付いていてもよい。ログは中身が RD のログ（1 行目が「[時刻] RD 版」）か Windows のダンプ（MDMP）でなければ捨てる

const KEY = "rd-7f3c1a9e"; // RD の app/service.cpp と同じ（配る版に入っているので、知らない送り主を断る程度のもの）
const MAX_UPLOAD = 32 << 20;
const CLIENT = /^[0-9a-f]{16}$/;
const NAME = /^[0-9A-Za-z_.-]{1,96}$/;
const VERSION = /^[0-9A-Za-z.]{0,32}$/;
const LOG_HEAD = /^\[\d\d:\d\d:\d\d\.\d\d\d\] RD [0-9A-Za-z.]+/;

const text = (status, body) => new Response(body + "\n", { status, headers: { "content-type": "text/plain; charset=utf-8" } });

// 合言葉を比べる（違う文字の位置で時間が変わらないように、最後まで比べる）
function same(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || !b) return false;
  let diff = a.length ^ b.length;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i % b.length);
  return diff === 0;
}

function looksLikeLog(name, bytes) {
  if (name.endsWith(".txt")) return LOG_HEAD.test(new TextDecoder().decode(bytes.subarray(0, 64)));
  if (name.endsWith(".dmp")) return bytes[0] === 0x4d && bytes[1] === 0x44 && bytes[2] === 0x4d && bytes[3] === 0x50;
  return false;
}

async function update(env, name) {
  if (!NAME.test(name)) return text(404, "not found");
  const obj = await env.BUCKET.get("update/" + name);
  if (!obj) return text(404, "not found");
  const json = name.endsWith(".json");
  return new Response(obj.body, {
    headers: {
      "content-type": json ? "application/json" : "application/zip",
      "content-length": String(obj.size),
      "cache-control": json ? "no-cache" : "max-age=86400",
    },
  });
}

async function log(req, env, q) {
  if (!same(req.headers.get("x-rd-key") || "", KEY)) return text(403, "forbidden");
  const client = q.get("client") || "", name = q.get("name") || "", version = q.get("version") || "";
  if (!CLIENT.test(client) || !NAME.test(name) || !VERSION.test(version)) return text(400, "bad request");
  const n = Number(req.headers.get("content-length") || "-1");
  if (!(n >= 0 && n <= MAX_UPLOAD)) return text(413, "too large");
  // 送りすぎを断る（台ごと・全体）。上限の仕組みが無い環境（手元の試し）では数えない
  if (env.PER_CLIENT && !(await env.PER_CLIENT.limit({ key: client })).success) return text(429, "too many");
  if (env.ALL && !(await env.ALL.limit({ key: "all" })).success) return text(429, "too many");
  const bytes = new Uint8Array(await req.arrayBuffer());
  if (bytes.length !== n || !looksLikeLog(name, bytes)) return text(400, "not a log");
  let key = `logs/${client}/${name}`;
  for (let i = 1; await env.BUCKET.head(key); i++) {
    const dot = name.lastIndexOf(".");
    key = `logs/${client}/${name.slice(0, dot)}.${i}${name.slice(dot)}`;
  }
  await env.BUCKET.put(key, bytes, { customMetadata: { version } });
  return text(200, "ok");
}

async function admin(req, env, p, q) {
  if (!same(req.headers.get("x-rd-admin") || "", env.ADMIN_SECRET || "")) return text(403, "forbidden");
  if (p === "/admin/logs" && req.method === "GET") {
    const r = await env.BUCKET.list({ prefix: "logs/", cursor: q.get("cursor") || undefined, limit: 500, include: ["customMetadata"] });
    return Response.json({
      objects: r.objects.map((o) => ({ key: o.key, size: o.size, uploaded: o.uploaded, version: o.customMetadata?.version || "" })),
      cursor: r.truncated ? r.cursor : null,
    });
  }
  const key = q.get("key") || "";
  if (p !== "/admin/object" || !key.startsWith("logs/") || key.includes("..")) return text(404, "not found");
  if (req.method === "GET") {
    const obj = await env.BUCKET.get(key);
    return obj ? new Response(obj.body) : text(404, "not found");
  }
  if (req.method === "DELETE") {
    await env.BUCKET.delete(key);
    return text(200, "ok");
  }
  return text(405, "method not allowed");
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    let p = url.pathname;
    if (p.startsWith("/rd/")) p = p.slice(3);
    if (p.startsWith("/update/") && (req.method === "GET" || req.method === "HEAD")) return update(env, p.slice(8));
    if (p === "/log" && req.method === "POST") return log(req, env, url.searchParams);
    if (p.startsWith("/admin/")) return admin(req, env, p, url.searchParams);
    return text(404, "not found");
  },
};
