use std::path::Path;

use anyhow::{Context, Result, anyhow};
use log::{error, info};

use crate::cli::{Cli, Commands};
use crate::client::RateLimitedClient;
use crate::download;
use crate::event;
use crate::table;

pub async fn run(cli: Cli, client: RateLimitedClient) -> Result<()> {
    match &cli.command {
        Commands::Md5 { md5, main_url, diff_url, hint, artist, threads } => {
            let seed = crate::provider::BmsUrl { main_urls: main_url.clone(), diff_urls: diff_url.clone() };
            let boards = match (threads, hint) {
                (Some(index), Some(title)) if index.exists() => Some(crate::provider::threads::ThreadsProvider {
                    index: index.clone(),
                    title: title.clone(),
                    artist: artist.clone(),
                }),
                _ => None,
            };
            info!("md5: {}", md5);
            download::download_md5(&client, md5, &cli.output_dir, cli.body_dir.as_deref(), seed, hint.as_deref(), boards).await
        }
        Commands::Threads { out } => crate::threads::build_index(&client, out).await,
        Commands::Table { url } => handle_table(&client, url, &cli.output_dir).await,
        Commands::Event { url } => handle_event(&client, url, &cli.output_dir).await,
    }
}


async fn handle_table(client: &RateLimitedClient, url: &str, output_dir: &Path) -> Result<()> {
    let table = table::parse_table(client, url)
        .await
        .context("Failed to fetch table")?;

    info!("Table fetched: {}", table.name);

    let mut succeeded = 0;
    let mut failed = 0;

    for bms in &table.bms_data {
        match download::download_table_entry(client, bms, output_dir).await {
            Ok(()) => succeeded += 1,
            Err(e) => {
                failed += 1;
                error!(
                    "Failed to download table entry: md5={}, level={}, title={}, artist={}: {:#}",
                    bms.md5,
                    bms.level,
                    bms.title.as_deref().unwrap_or("(no title)"),
                    bms.artist.as_deref().unwrap_or("(no artist)"),
                    e
                );
            }
        }
    }

    info!(
        "Table download finished: {} succeeded, {} failed",
        succeeded, failed
    );

    if failed > 0 {
        Err(anyhow!("{} table entries failed", failed))
    } else {
        Ok(())
    }
}

async fn handle_event(client: &RateLimitedClient, url: &str, output_dir: &Path) -> Result<()> {
    let scraper = match event::get_scraper(url) {
        Some(s) => s,
        None => {
            return Err(anyhow!("This event site URL is currently not supported"));
        }
    };

    let entries = scraper
        .scrape(client, url)
        .await
        .context("Failed to parse event page")?;

    info!("Event fetched ({} songs in total)", entries.len());

    let mut succeeded = 0;
    let mut failed = 0;

    for (index, entry) in entries.iter().enumerate() {
        match download::download_event_entry(client, entry, output_dir).await {
            Ok(()) => succeeded += 1,
            Err(e) => {
                failed += 1;
                error!(
                    "Failed to download event entry #{}: urls={:?}: {:#}",
                    index + 1,
                    entry.urls,
                    e
                );
            }
        }
    }

    info!(
        "Event download finished: {} succeeded, {} failed",
        succeeded, failed
    );

    if failed > 0 {
        Err(anyhow!("{} event entries failed", failed))
    } else {
        Ok(())
    }
}
