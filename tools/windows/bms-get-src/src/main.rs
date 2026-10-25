mod app;
mod bms;
mod cli;
mod client;
mod download;
mod downloader;
mod event;
mod extract;
mod page;
mod parser;
mod provider;
mod site;
mod sniff;
mod store;
mod table;
mod threads;

use std::num::NonZeroU32;

use clap::Parser;
use cli::Cli;
use governor::Quota;

use crate::client::RateLimitedClient;

#[tokio::main]
async fn main() {
    env_logger::init_from_env(env_logger::Env::new().default_filter_or("bms_get=info"));
    let cli = Cli::parse();

    let quota = Quota::per_second(NonZeroU32::new(2).unwrap());
    // Cookies: some uploaders hand over the file only in the session that opened the page.
    // Timeouts: a dead host must not hold a chart forever (the read timeout is per chunk, so big files are fine)
    let client = reqwest::Client::builder()
        .user_agent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) bms-get")
        .cookie_store(true)
        .connect_timeout(std::time::Duration::from_secs(20))
        .read_timeout(std::time::Duration::from_secs(60))
        .build()
        .unwrap();
    let client = RateLimitedClient::new(client, quota);

    if let Err(e) = app::run(cli, client).await {
        log::error!("{:#}", e);
        std::process::exit(1);
    }
}
