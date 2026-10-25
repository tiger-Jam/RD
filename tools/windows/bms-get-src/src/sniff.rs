/// What a downloaded file is, judged by its first bytes (the name or the server's header is often wrong or missing).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Zip,
    Rar,
    SevenZ,
    Lzh,
    Exe,
    Html,
    Json,
    Chart,
    Other,
}

impl Kind {
    pub fn ext(self) -> Option<&'static str> {
        match self {
            Kind::Zip => Some("zip"),
            Kind::Rar => Some("rar"),
            Kind::SevenZ => Some("7z"),
            Kind::Lzh => Some("lzh"),
            Kind::Exe => Some("exe"),
            Kind::Chart => Some("bms"),
            Kind::Html | Kind::Json | Kind::Other => None,
        }
    }
}

pub fn kind(head: &[u8]) -> Kind {
    if head.starts_with(b"PK\x03\x04") || head.starts_with(b"PK\x05\x06") {
        return Kind::Zip;
    }
    if head.starts_with(b"Rar!\x1a\x07") {
        return Kind::Rar;
    }
    if head.starts_with(b"7z\xbc\xaf\x27\x1c") {
        return Kind::SevenZ;
    }
    if is_lzh_at(head, 0) {
        return Kind::Lzh;
    }
    if head.starts_with(b"MZ") {
        return Kind::Exe;
    }
    let text = trim_start(head);
    let lower: Vec<u8> = text.iter().take(512).map(|b| b.to_ascii_lowercase()).collect();
    if lower.starts_with(b"<!doctype") || lower.starts_with(b"<html") || lower.starts_with(b"<?xml")
        || lower.starts_with(b"<head") || lower.starts_with(b"<script") || lower.starts_with(b"<meta")
        || lower.starts_with(b"<frameset") || lower.starts_with(b"<!--")
    {
        return Kind::Html;
    }
    if text.starts_with(b"{") || text.starts_with(b"[") {
        return Kind::Json;
    }
    if text.starts_with(b"#") || text.starts_with(b"*") {
        return Kind::Chart;
    }
    Kind::Other
}

/// An LHA header starts with its size and checksum, then the method id "-lh?-" / "-lz?-".
pub fn is_lzh_at(data: &[u8], at: usize) -> bool {
    let Some(m) = data.get(at + 2..at + 7) else { return false };
    m[0] == b'-' && m[1] == b'l' && (m[2] == b'h' || m[2] == b'z') && m[4] == b'-'
}

fn trim_start(b: &[u8]) -> &[u8] {
    let b = b.strip_prefix(b"\xef\xbb\xbf").unwrap_or(b);
    let n = b.iter().take_while(|c| c.is_ascii_whitespace()).count();
    &b[n..]
}
