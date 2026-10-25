//! Request boards: threads where people ask for BMS that cannot be found and others answer with links
//! (したらば: BMSクレクレ板 travel/2565, and the 発狂BMS board game/39018 whose proposals carry download links).
//! The threads are collected into one index file; a chart's title (and artist) is looked up in it.

use anyhow::{Context, Result};
use encoding_rs::EUC_JP;
use log::{info, warn};
use regex::Regex;
use std::collections::{HashSet, VecDeque};
use std::io::{BufRead, Write};
use std::path::Path;
use std::sync::OnceLock;
use unicode_normalization::UnicodeNormalization;

use crate::client::RateLimitedClient;

const BOARDS: &[&str] = &["travel/2565", "game/39018"];

#[derive(serde::Serialize, serde::Deserialize, Clone)]
pub struct Post {
    /// board/key
    pub thread: String,
    pub no: u32,
    pub text: String,
}

/// Collects every thread reachable on the boards (the thread lists, then the threads they link to) into `out` (JSON lines).
pub async fn build_index(client: &RateLimitedClient, out: &Path) -> Result<()> {
    let spam = Regex::new("出会|人妻|熟女|逆援|セフレ|コピー|サロン|不倫|セックス").unwrap();
    let mut queue: VecDeque<String> = VecDeque::new();
    for board in BOARDS {
        let text = get_euc(client, &format!("https://jbbs.shitaraba.net/{}/subject.txt", board)).await?;
        for line in text.lines() {
            let Some((key, title)) = line.split_once(".cgi,") else { continue };
            if !spam.is_match(title) {
                queue.push_back(format!("{}/{}", board, key));
            }
        }
    }
    let link = Regex::new(r"(travel/2565|game/39018)/(\d{9,10})").unwrap();
    let mut seen = HashSet::new();
    let tmp = out.with_extension("tmp");
    if let Some(dir) = out.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let mut w = std::io::BufWriter::new(std::fs::File::create(&tmp)?);
    let (mut threads, mut posts) = (0, 0);
    while let Some(thread) = queue.pop_front() {
        if !seen.insert(thread.clone()) {
            continue;
        }
        let raw = match get_euc(client, &format!("https://jbbs.shitaraba.net/bbs/rawmode.cgi/{}/", thread)).await {
            Ok(r) => r,
            Err(e) => {
                warn!("Thread {}: {}", thread, e);
                continue;
            }
        };
        if raw.trim_start().starts_with('<') {
            continue; // deleted (an error page)
        }
        threads += 1;
        for line in raw.lines() {
            let f: Vec<&str> = line.split("<>").collect();
            if f.len() < 5 {
                continue;
            }
            let Ok(no) = f[0].parse() else { continue };
            let text = clean(f[4]);
            for c in link.captures_iter(f[4]) {
                let t = format!("{}/{}", &c[1], &c[2]);
                if !seen.contains(&t) {
                    queue.push_back(t);
                }
            }
            serde_json::to_writer(&mut w, &Post { thread: thread.clone(), no, text })?;
            w.write_all(b"\n")?;
            posts += 1;
        }
    }
    // Threads already deleted from the board may be in the Internet Archive (Part.1-15 and others)
    for (thread, list) in archived_threads(client).await {
        if seen.contains(&thread) {
            continue;
        }
        seen.insert(thread.clone());
        let mut got: std::collections::BTreeMap<u32, String> = Default::default();
        for url in list {
            let Ok(html) = get_euc(client, &url).await else { continue };
            for (no, text) in parse_read_cgi(&html) {
                got.entry(no).or_insert(text);
            }
        }
        if got.is_empty() {
            continue;
        }
        threads += 1;
        for (no, text) in got {
            serde_json::to_writer(&mut w, &Post { thread: thread.clone(), no, text })?;
            w.write_all(b"\n")?;
            posts += 1;
        }
    }
    w.flush()?;
    drop(w);
    std::fs::rename(&tmp, out)?;
    info!("Request boards: {} threads, {} posts → {}", threads, posts, out.display());
    Ok(())
}

/// The boards' thread pages saved in the Internet Archive: thread → capture URLs (`id_`, a few per thread).
async fn archived_threads(client: &RateLimitedClient) -> Vec<(String, Vec<String>)> {
    let key = Regex::new(r"read\.cgi/(travel/2565|game/39018)/(\d{9,10})").unwrap();
    let mut by: std::collections::BTreeMap<String, Vec<String>> = Default::default();
    for host in ["jbbs.livedoor.jp", "jbbs.shitaraba.net"] {
        for board in BOARDS {
            let api = format!(
                "https://web.archive.org/cdx/search/cdx?url={}/bbs/read.cgi/{}/&matchType=prefix&output=txt&fl=original,timestamp&filter=statuscode:200&collapse=urlkey&limit=50000",
                host, board
            );
            // The archive's index is often busy: a few tries
            let mut text = None;
            for wait in [0u64, 20, 60] {
                tokio::time::sleep(std::time::Duration::from_secs(wait)).await;
                match client.get(&api).await.send().await {
                    Ok(r) if r.status().is_success() => {
                        if let Ok(t) = r.text().await {
                            if !t.contains("<html") {
                                text = Some(t);
                                break;
                            }
                        }
                    }
                    _ => {}
                }
            }
            let Some(text) = text else {
                warn!("Internet Archive index not available: {} {}", host, board);
                continue;
            };
            for line in text.lines() {
                let mut f = line.split_whitespace();
                let (Some(orig), Some(ts)) = (f.next(), f.next()) else { continue };
                if let Some(c) = key.captures(orig) {
                    let list = by.entry(format!("{}/{}", &c[1], &c[2])).or_default();
                    if list.len() < 12 {
                        list.push(format!("https://web.archive.org/web/{}id_/{}", ts, orig));
                    }
                }
            }
        }
    }
    by.into_iter().collect()
}

/// Posts on an old read.cgi page: `<dt>番号 ：名前：日付</dt><dd>本文</dd>`.
fn parse_read_cgi(html: &str) -> Vec<(u32, String)> {
    static RE: OnceLock<Regex> = OnceLock::new();
    let re = RE.get_or_init(|| Regex::new(r"(?is)<dt[^>]*>\s*(?:<a[^>]*>)?\s*(\d+)\s*(?:</a>)?\s*[：:].*?<dd[^>]*>(.*?)(?:<br>\s*)*(?:</dd>|<dt|</dl>)").unwrap());
    re.captures_iter(html).filter_map(|c| Some((c[1].parse().ok()?, clean(&c[2])))).collect()
}

async fn get_euc(client: &RateLimitedClient, url: &str) -> Result<String> {
    let bytes = client.get(url).await.send().await?.error_for_status()?.bytes().await?;
    Ok(EUC_JP.decode(&bytes).0.into_owned())
}

/// The post's text: line breaks kept, tags dropped, entities decoded.
fn clean(body: &str) -> String {
    static TAG: OnceLock<Regex> = OnceLock::new();
    let tag = TAG.get_or_init(|| Regex::new(r"<[^>]*>").unwrap());
    let s = body.replace("<br>", "\n");
    let s = tag.replace_all(&s, "");
    s.replace("&gt;", ">").replace("&lt;", "<").replace("&quot;", "\"").replace("&#39;", "'").replace("&nbsp;", " ").replace("&amp;", "&")
}

/// For comparing names: full-width to half-width, lower case, no spaces or punctuation.
pub fn norm(s: &str) -> String {
    s.nfkc()
        .flat_map(char::to_lowercase)
        .filter(|c| c.is_alphanumeric() || matches!(c, '&' | '+' | '#' | '*' | '@' | '$' | '%'))
        .collect()
}

/// Download links in a line ("ttp://", full-width letters, links split by the board are all common).
fn urls_in(line: &str) -> Vec<String> {
    static RE: OnceLock<Regex> = OnceLock::new();
    let re = RE.get_or_init(|| Regex::new(r"(?i)(h?t?tps?)://[\x21-\x7e]+").unwrap());
    let line: String = line.nfkc().collect();
    re.captures_iter(&line)
        .filter_map(|c| {
            let m = c.get(0)?.as_str();
            let scheme = if c[1].to_lowercase().ends_with("tps") { "https" } else { "http" };
            let rest = &m[m.find("://")? + 3..];
            let rest = rest.trim_end_matches(['.', ',', ')', ']', '>', '\'', '"', '」', '）']);
            let url = format!("{}://{}", scheme, rest);
            useful(&url).then_some(url)
        })
        .collect()
}

/// Links that are not the file (the board itself, rankings, video sites...).
fn useful(url: &str) -> bool {
    let u = url.to_lowercase();
    ![
        "jbbs.", "shitaraba", "dream-pro.info", "lr2ir", "google.co", "google.com/search", "twitter.com", "x.com/", "nicovideo",
        "youtube", "youtu.be", "wikipedia", "/web/*", "pixiv.net", "soundcloud.com", "2ch.net", "5ch.net", "bmssearch.net/bms",
    ]
    .iter()
    .any(|bad| u.contains(bad))
}

pub struct Index {
    posts: Vec<Post>,
}

impl Index {
    pub fn load(path: &Path) -> Result<Self> {
        let f = std::fs::File::open(path).with_context(|| format!("No request-board index: {}", path.display()))?;
        let posts = std::io::BufReader::new(f).lines().map_while(|l| l.ok()).filter_map(|l| serde_json::from_str(&l).ok()).collect();
        Ok(Self { posts })
    }

    /// Links posted for the song, best first: lines naming the title (and the artist), and answers to requests for it.
    pub fn find(&self, title: &str, artist: Option<&str>) -> Vec<String> {
        // "Song [ANOTHER]" / "Song -Remix- (obj: x)": the board names the song, not the chart
        let title = title.split(['[', '(', '<', '【', '（', '＜']).next().unwrap_or(title).trim();
        let t = norm(title);
        let a = artist
            .map(|a| a.split(['/', '／']).next().unwrap_or(a))
            .map(|a| a.split("obj").next().unwrap_or(a))
            .map(norm)
            .filter(|a| a.chars().count() >= 2);
        // Short titles are only trusted with the artist's name next to them
        let short = t.chars().count() < 6;
        if t.chars().count() < 3 || (short && a.is_none()) {
            return vec![];
        }
        let mut found: Vec<(i32, String)> = Vec::new();
        let mut add = |score: i32, url: String| {
            if let Some(e) = found.iter_mut().find(|(_, u)| *u == url) {
                e.0 = e.0.max(score);
            } else {
                found.push((score, url));
            }
        };
        static REPLY: OnceLock<Regex> = OnceLock::new();
        let reply = REPLY.get_or_init(|| Regex::new(r"(?:>>|＞＞|》)\s*(\d{1,4})").unwrap());

        for (i, p) in self.posts.iter().enumerate() {
            let lines: Vec<&str> = p.text.lines().collect();
            let normed: Vec<String> = lines.iter().map(|l| norm(l)).collect();
            let post_has_artist = a.as_ref().is_some_and(|a| norm(&p.text).contains(a.as_str()));
            let mut named = false;
            for (j, nl) in normed.iter().enumerate() {
                if !nl.contains(&t) {
                    continue;
                }
                let near_artist = a.as_ref().is_some_and(|a| {
                    nl.contains(a.as_str()) || normed.get(j + 1).is_some_and(|n| n.contains(a.as_str())) || (j > 0 && normed[j - 1].contains(a.as_str()))
                });
                if short && !near_artist {
                    continue;
                }
                named = true;
                let score = if near_artist { 100 } else if post_has_artist { 70 } else { 40 };
                // The link is on the line, or on the line after the name ("曲名 / 作者" then the URL)
                for u in urls_in(lines[j]) {
                    add(score, u);
                }
                if let Some(next) = lines.get(j + 1) {
                    if !normed[j + 1].is_empty() && urls_in(lines[j]).is_empty() {
                        for u in urls_in(next) {
                            add(score - 5, u);
                        }
                    }
                }
            }
            if !named {
                continue;
            }
            // Answers: later posts in the thread that point at this one (>>no)
            for q in self.posts[i + 1..].iter().take_while(|q| q.thread == p.thread).take(300) {
                if reply.captures_iter(&q.text).any(|c| c[1].parse::<u32>().ok() == Some(p.no)) {
                    for u in urls_in(&q.text) {
                        add(60, u);
                    }
                }
            }
        }
        found.sort_by(|x, y| y.0.cmp(&x.0));
        found.into_iter().take(8).map(|(_, u)| u).collect()
    }
}
