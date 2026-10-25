use std::path::PathBuf;

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(version, about, long_about = None)]
#[command(propagate_version = true)]
pub struct Cli {
    #[command(subcommand)]
    pub command: Commands,

    #[arg(short, long, default_value = ".")]
    pub output_dir: PathBuf,

    /// Existing folder of the song (body). When the downloaded diff plays with the
    /// sounds in this folder, only the diff is copied there and the body is not downloaded.
    #[arg(long)]
    pub body_dir: Option<PathBuf>,
}

#[derive(Subcommand)]
pub enum Commands {
    /// Download BMS form an MD5 hash
    Md5 {
        md5: String,
        /// Where the body is distributed (from the difficulty table's `url`). Can be given more than once
        #[arg(long)]
        main_url: Vec<String>,
        /// Where the diff is distributed (from the difficulty table's `url_diff`). Can be given more than once
        #[arg(long)]
        diff_url: Vec<String>,
        /// The song's title: on a page with many songs (an event's list), links near it are tried first
        #[arg(long)]
        hint: Option<String>,
        /// The song's artist (with --hint, to look the song up on the request boards)
        #[arg(long)]
        artist: Option<String>,
        /// The request-board index made by `threads` (searched last, by title and artist)
        #[arg(long)]
        threads: Option<PathBuf>,
    },
    /// Collect the request boards' threads into an index file (for `md5 --threads`)
    Threads { out: PathBuf },
    /// Download BMS from a difficulty table
    Table { url: String },
    /// Download BMS from an event page
    Event { url: String },
}
