use anyhow::{Result, anyhow};
use indicatif::{ProgressBar, ProgressStyle};
use log::{debug, error, info};
use std::collections::{HashSet, VecDeque};
use std::path::Path;

use crate::bms::{merge, validation};
use crate::client::RateLimitedClient;
use crate::parser::{self, ParseResult};
use crate::provider::{
    BmsProvider, BmsUrl, bms_search::BmsSearchProvider, lr2ir_archive::Lr2IrArchiveProvider,
    seed::SeedProvider,
};
use crate::table::BmsData;
use crate::downloader::Fetched;
use crate::{downloader, extract, site, store};

/// Pages followed for one chart, at most; files downloaded for one chart, at most; how deep links are followed.
const MAX_PAGES: usize = 30;
const MAX_FILES: usize = 16;
const MAX_DEPTH: u8 = 3;
/// Files given by the table or a provider can be any size; files found by following pages, at most this
const MAX_FOUND_BYTES: u64 = 1536 << 20;

pub async fn download_md5(
    client: &RateLimitedClient,
    md5: &str,
    output_dir: &Path,
    body_dir: Option<&Path>,
    seed: BmsUrl,
    hint: Option<&str>,
    boards: Option<crate::provider::threads::ThreadsProvider>,
) -> Result<()> {
    download_by_md5(client, md5, output_dir, seed, body_dir, hint, boards).await
}

pub async fn download_table_entry(
    client: &RateLimitedClient,
    bms: &BmsData,
    output_dir: &Path,
) -> Result<()> {
    let seed = BmsUrl {
        main_urls: bms.main_url.clone().map_or(vec![], |u| vec![u]),
        diff_urls: bms.diff_url.clone().map_or(vec![], |u| vec![u]),
    };

    download_by_md5(client, &bms.md5, output_dir, seed, None, bms.title.as_deref(), None).await
}

enum Step {
    /// Not this file; try the next URL
    Next,
    /// Done (saved, or the diff was placed into the body)
    Done,
    /// The chart is here but its sounds are not: look for the body
    NeedBody,
}

async fn download_by_md5(
    client: &RateLimitedClient,
    md5: &str,
    output_dir: &Path,
    seed: BmsUrl,
    body_dir: Option<&Path>,
    hint: Option<&str>,
    boards: Option<crate::provider::threads::ThreadsProvider>,
) -> Result<()> {
    std::fs::create_dir_all(output_dir)?;

    let package = output_dir.join("packages").join(md5);
    if package.exists() {
        return Ok(());
    }

    let temp = tempfile::tempdir_in(work_dir(output_dir)?)?;

    let mut attempted_urls = HashSet::new();
    let mut budget = Budget::default();
    let mut providers: Vec<Box<dyn BmsProvider>> = vec![
        Box::new(SeedProvider::new(seed)),
        Box::new(BmsSearchProvider),
        Box::new(Lr2IrArchiveProvider),
    ];
    // The request boards come last: their own rule is to look everywhere else first
    if let Some(b) = boards {
        providers.push(Box::new(b));
    }
    // Each provider is asked once; its answer is used in both passes
    let mut answers: Vec<Option<BmsUrl>> = Vec::new();
    for provider in &providers {
        info!("Searching on {}.... (md5: {})", provider.name(), md5);
        answers.push(match provider.find_urls(client, md5).await {
            Ok(urls) => Some(urls),
            Err(e) => {
                error!("Searching on {} failed: {}", provider.name(), e);
                None
            }
        });
    }

    for need in [NeedBmsType::Diff, NeedBmsType::Main] {
        'providers: for urls in answers.iter().flatten() {
            let urls = match need {
                NeedBmsType::Diff => &urls.diff_urls,
                NeedBmsType::Main => &urls.main_urls,
            };
            let mut walker = UrlWalker::new(urls, &mut attempted_urls);

            while let Some(next) = walker.next(client).await {
                let (url, depth) = match next {
                    Ok(next) => next,
                    Err(e) => {
                        error!("Parsing failed: {}", e);
                        continue;
                    }
                };
                if budget.files >= MAX_FILES {
                    error!("Gave up: too many files tried for md5={}", md5);
                    break 'providers;
                }
                let download_temp = tempfile::tempdir_in(work_dir(output_dir)?)?;

                let fetched = match download(client, &url, download_temp.path(), hint, depth).await {
                    Ok(f) => f,
                    Err(e) => {
                        error!("Failed: {} - {}", url, e);
                        // A dead link may still be in the Internet Archive
                        if !site::wayback::is_wayback(&url) && worth_archive(&e) {
                            match site::wayback::lookup(client, &url).await {
                                Ok(Some(archived)) => {
                                    info!("Trying the archived copy: {}", archived);
                                    walker.push_front(archived, depth);
                                }
                                Ok(None) => {}
                                Err(e) => debug!("Wayback lookup failed: {}", e),
                            }
                        }
                        continue;
                    }
                };
                let paths = match fetched {
                    Fetched::Page(links) => {
                        if depth < MAX_DEPTH && budget.pages < MAX_PAGES {
                            budget.pages += 1;
                            walker.push_all(links, depth + 1);
                        }
                        continue;
                    }
                    Fetched::Files(paths) => paths,
                    Fetched::Retry(_) => continue,
                };

                for path in paths {
                    budget.files += 1;
                    match take_file(md5, &url, &path, temp.path(), output_dir, body_dir)? {
                        Step::Done => return Ok(()),
                        Step::Next => {}
                        Step::NeedBody => {
                            if let NeedBmsType::Diff = need {
                                break 'providers;
                            }
                        }
                    }
                }
            }
        }
    }

    Err(anyhow!("Download incomplete: md5={}", md5))
}

#[derive(Default)]
struct Budget {
    pages: usize,
    files: usize,
}

/// Errors where the original is gone (HTTP errors, unreachable hosts), not "this is not an archive".
fn worth_archive(e: &anyhow::Error) -> bool {
    let s = format!("{:#}", e);
    !(s.contains("Not an archive") || s.contains("No extractor") || s.contains("password") || s.contains("MEGA") || s.contains("OneDrive"))
}

fn take_file(md5: &str, url: &str, path: &Path, temp: &Path, output_dir: &Path, body_dir: Option<&Path>) -> Result<Step> {
    let result = match extract::extract(path) {
        Ok(result) => result,
        Err(e) => {
            error!("Failed to extract: {} - {}", url, e);
            return Ok(Step::Next);
        }
    };
    debug!(
        "Extracted {} entries from {} to {}",
        result.extracted_paths.len(),
        result.archive_path.display(),
        result.target_dir.display()
    );

    merge::merge_bms_dir(temp, &result.target_dir)?;

    if !validation::validate_md5(md5, temp)? {
        return Ok(Step::Next);
    }

    // The body is already there: copy only the diff into the body folder.
    if let Some(body) = body_dir {
        if body.is_dir() && validation::validate_ref_with(md5, temp, body)? {
            if let Some(chart) = crate::bms::analyze::find_chart(temp, md5)? {
                let chart_dir = chart.parent().unwrap_or(temp).to_path_buf();
                copy_missing(&chart_dir, body)?;
                let placed = output_dir.join("placed");
                std::fs::create_dir_all(&placed)?;
                std::fs::write(placed.join(format!("{}.txt", md5)), body.to_string_lossy().as_bytes())?;
                info!("Placed the diff into the existing body: {}", body.display());
                return Ok(Step::Done);
            }
        }
    }

    if validation::validate_ref(md5, temp)? {
        save_song(md5, temp, output_dir)?;
        return Ok(Step::Done);
    }
    // A pack: the diff came apart from its song. Put the chart into the folder that has its sounds
    if let Some(chart) = crate::bms::analyze::find_chart(temp, md5)? {
        if let Some(dir) = best_folder(&chart, temp)? {
            let Some(name) = chart.file_name() else { return Ok(Step::NeedBody) };
            let target = dir.join(name);
            if !target.exists() {
                std::fs::rename(&chart, &target)?;
                info!("The chart's sounds are in {}: moved the chart there", dir.display());
                if validation::validate_ref(md5, temp)? {
                    save_song(md5, temp, output_dir)?;
                    return Ok(Step::Done);
                }
            }
        }
    }
    Ok(Step::NeedBody)
}

/// Saves the chart's own song folder (not a whole event pack that came with it).
fn save_song(md5: &str, temp: &Path, output_dir: &Path) -> Result<()> {
    let chart = crate::bms::analyze::find_chart(temp, md5)?;
    let dir = chart.as_deref().and_then(Path::parent).filter(|d| *d != temp).map(Path::to_path_buf);
    match dir {
        Some(d) => store::save(&d, output_dir, Path::new(md5)),
        None => store::save(temp, output_dir, Path::new(md5)),
    }
}

/// The folder under `root` where most of the chart's files are (at least the validation's share), if any.
fn best_folder(chart: &Path, root: &Path) -> Result<Option<std::path::PathBuf>> {
    let mut best: Option<(usize, std::path::PathBuf)> = None;
    let mut stack = vec![(root.to_path_buf(), 0)];
    while let Some((dir, depth)) = stack.pop() {
        let share = validation::ref_match_in(chart, &dir)?;
        if share >= validation::MIN_MATCH && best.as_ref().is_none_or(|(b, _)| share > *b) {
            best = Some((share, dir.clone()));
        }
        if depth < 3 {
            for e in std::fs::read_dir(&dir)?.flatten() {
                if e.file_type().is_ok_and(|t| t.is_dir()) {
                    stack.push((e.path(), depth + 1));
                }
            }
        }
    }
    Ok(best.map(|(_, d)| d))
}

/// Where downloads are unpacked: next to the output folder, not in it (the output is a song folder that a player
/// may be watching; a folder that keeps changing while it unpacks makes the player re-read it all the time).
/// Same drive, so the finished package is moved, not copied.
fn work_dir(output_dir: &Path) -> Result<std::path::PathBuf> {
    let dir = match output_dir.parent().filter(|p| !p.as_os_str().is_empty()) {
        Some(parent) => parent.join(".bms-get-work"),
        None => output_dir.join(".bms-get-work"),
    };
    std::fs::create_dir_all(&dir)?;
    Ok(dir)
}

/// Copies files from `src` into `dst` that `dst` does not have yet (never overwrites).
fn copy_missing(src: &Path, dst: &Path) -> Result<()> {
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let path = entry.path();
        let target = dst.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            std::fs::create_dir_all(&target)?;
            copy_missing(&path, &target)?;
        } else if !target.exists() {
            std::fs::copy(&path, &target)?;
        }
    }
    Ok(())
}

#[derive(Clone, Copy)]
enum NeedBmsType {
    Diff,
    Main,
}

pub async fn download_event_entry(
    client: &RateLimitedClient,
    entry: &crate::event::EventEntry,
    output_dir: &Path,
) -> Result<()> {
    std::fs::create_dir_all(output_dir)?;

    let mut attempted_urls = HashSet::new();
    let mut walker = UrlWalker::new(&entry.urls, &mut attempted_urls);

    while let Some(next) = walker.next(client).await {
        let (url, depth) = match next {
            Ok(next) => next,
            Err(e) => {
                error!("Parsing failed: {}", e);
                continue;
            }
        };
        let download_temp = tempfile::tempdir_in(work_dir(output_dir)?)?;

        let path = match download(client, &url, download_temp.path(), None, depth).await {
            Ok(Fetched::Files(paths)) if !paths.is_empty() => paths[0].clone(),
            Ok(Fetched::Files(_)) | Ok(Fetched::Retry(_)) => continue,
            Ok(Fetched::Page(links)) => {
                if depth < MAX_DEPTH {
                    walker.push_all(links, depth + 1);
                }
                continue;
            }
            Err(e) => {
                error!("Failed: {} - {}", url, e);
                continue;
            }
        };

        let result = match extract::extract(&path) {
            Ok(result) => result,
            Err(e) => {
                error!("Failed to extract: {} - {}", url, e);
                continue;
            }
        };

        let Some(name) = result.target_dir.file_name() else {
            error!("Invalid extraction target: {}", result.target_dir.display());
            continue;
        };
        store::save(&result.target_dir, output_dir, Path::new(name))?;

        return Ok(());
    }

    Err(anyhow!("No downloadable URLs found"))
}

async fn download(client: &RateLimitedClient, url: &str, output_dir: &Path, hint: Option<&str>, depth: u8) -> Result<Fetched> {
    let pb = ProgressBar::new(0);
    pb.set_style(
        ProgressStyle::default_bar()
            .template("{msg}\n[{bar:40.green/white}] {bytes}/{total_bytes} ({percent}%) {elapsed_precise}")?
            .progress_chars("=> "),
    );
    pb.set_message(format!("Starting: {}", url));

    let pb_clone = pb.clone();
    let fetched = downloader::download(
        client,
        url,
        output_dir,
        hint,
        if depth == 0 { u64::MAX } else { MAX_FOUND_BYTES },
        Box::new(move |inc, total| {
            if (pb.length().is_none() || pb.length() == Some(0)) && total > 0 {
                pb.set_length(total);
            }
            pb.inc(inc);
        }),
    )
    .await?;

    pb_clone.finish_with_message(format!("Finished: {}", url));
    Ok(fetched)
}

struct UrlWalker<'a> {
    queue: VecDeque<(String, u8)>,
    attempted_urls: &'a mut HashSet<String>,
}

impl<'a> UrlWalker<'a> {
    fn new(urls: &[String], attempted_urls: &'a mut HashSet<String>) -> Self {
        Self {
            queue: urls.iter().map(|u| (u.trim().to_string(), 0)).filter(|(u, _)| !u.is_empty()).collect(),
            attempted_urls,
        }
    }

    fn push_all(&mut self, urls: Vec<String>, depth: u8) {
        self.queue.extend(urls.into_iter().map(|u| (u, depth)));
    }

    fn push_front(&mut self, url: String, depth: u8) {
        self.queue.push_front((url, depth));
    }

    async fn next(&mut self, client: &RateLimitedClient) -> Option<Result<(String, u8)>> {
        while let Some((url, depth)) = self.queue.pop_front() {
            if !self.attempted_urls.insert(url.clone()) {
                continue;
            }

            match parser::parse_url(client, &url).await {
                Ok(ParseResult::Links(new_urls)) => {
                    for u in new_urls.into_iter().rev() {
                        self.queue.push_front((u, depth));
                    }
                }
                Ok(ParseResult::File(dl_url)) => return Some(Ok((dl_url, depth))),
                Err(e) => return Some(Err(e.context(format!("Parsing failed: {}", url)))),
            }
        }

        None
    }
}
