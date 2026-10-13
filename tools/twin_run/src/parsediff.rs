//! Differential parsing against the C parsers (bead bd-9kll.16.2).
//!
//! The same bytes go to asupersync's HTTP/1.1 request parser here (the pinned
//! rev's `Http1Codec`, driven to end of input) and to the C parser (the
//! `ASX_DIFF_HTTP_REQUEST` mode of tests/fuzz/fuzz_parsers.c). Each side
//! prints one outcome object per input and `tools/ci/parser_differential.py`
//! compares them: accepted or rejected, then method, target, version, headers,
//! trailers, body length and digest, and the bytes the first request used.
//!
//! `twin_run parse-fuzz http-request --seed <u64> --count <n> --out <dir>`
//! writes `<dir>/inputs/NNNNN.bin` (a fixed list of well-formed requests,
//! then seeded mutations of them) and `<dir>/rust.jsonl`, the Rust outcome of
//! each input in order. `twin_run parse-diff http-request <file>...` prints
//! the outcome of each named file, which the minimizer re-runs on candidates.
//!
//! Byte strings are printed as the JSON string of the same code points
//! (Latin-1), as the C side does, so equal bytes compare equal whatever they
//! hold and no UTF-8 question arises in the comparison. Header and trailer
//! values are the exception: asupersync's decoder keeps a valid UTF-8 value
//! as UTF-8 and decodes any other as Latin-1, so the wire bytes cannot be
//! recovered from the `String`. The Rust value is printed as the string it
//! holds, and `parser_differential.py` gives the C bytes the same decoding
//! before it compares.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use asupersync::bytes::BytesMut;
use asupersync::codec::Decoder;
use asupersync::http::h1::Http1Codec;
use serde_json::{Value, json};

use crate::scenario_gen::Rng;

/// 64-bit FNV-1a, the digest both sides print for a body.
fn fnv1a(bytes: &[u8]) -> u64 {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for &byte in bytes {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

/// The JSON string whose code points are `bytes` (Latin-1).
fn latin1(bytes: &[u8]) -> String {
    bytes.iter().map(|&b| char::from(b)).collect()
}

/// Header or trailer fields: the name as Latin-1 of its UTF-8 bytes (a name
/// is validated UTF-8), the value as the decoded string (see the module doc).
fn pairs(items: &[(String, String)]) -> Value {
    Value::Array(
        items
            .iter()
            .map(|(name, value)| json!([latin1(name.as_bytes()), value]))
            .collect(),
    )
}

/// The name of an error's variant, without its payload.
fn variant_name(debug: &str) -> &str {
    debug.split(['(', ' ', '{']).next().unwrap_or(debug)
}

fn decode_request(input: &[u8]) -> Value {
    let mut codec = Http1Codec::new();
    let mut buf = BytesMut::from(input);
    match codec.decode_eof(&mut buf) {
        Ok(Some(req)) => json!({
            "kind": "complete",
            "method": latin1(req.method.as_str().as_bytes()),
            "target": latin1(req.uri.as_bytes()),
            "version": req.version.as_str(),
            "headers": pairs(&req.headers),
            "trailers": pairs(&req.trailers),
            "body_len": req.body.len(),
            "body_fnv": format!("{:016x}", fnv1a(&req.body)),
            "consumed": input.len() - buf.len(),
        }),
        // decode_eof returns this only for an empty buffer, handled above.
        Ok(None) => json!({"kind": "error", "detail": "no frame"}),
        Err(err) => json!({"kind": "error", "detail": variant_name(&format!("{err:?}"))}),
    }
}

/// The outcome of the first HTTP/1.1 request in `input`, with end of input
/// signalled: `empty` for no bytes, `complete` with the parsed fields, or
/// `error` (a malformed request, or one cut short). A panic in the parser is
/// reported as `panic`, never swallowed: it is the loudest divergence there is.
pub fn http_request_outcome(input: &[u8]) -> Value {
    if input.is_empty() {
        return json!({"kind": "empty"});
    }
    std::panic::catch_unwind(|| decode_request(input))
        .unwrap_or_else(|_| json!({"kind": "panic", "detail": "the parser panicked"}))
}

/// Well-formed requests the mutations start from: the shapes that matter for
/// framing (none, Content-Length, chunked with and without trailers), targets
/// (origin, absolute, asterisk, authority) and versions.
const BASES: [&[u8]; 10] = [
    b"GET / HTTP/1.1\r\nHost: a\r\n\r\n",
    b"POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello",
    b"POST /c HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    b"POST /t HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\nX-T: y\r\n\r\n",
    b"GET http://h/p?q=1 HTTP/1.0\r\nUser-Agent: t\r\n\r\n",
    b"PUT /a HTTP/1.1\r\nhost: a\r\ncontent-length: 0\r\n\r\n",
    b"OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n",
    b"CONNECT a:443 HTTP/1.1\r\nHost: a:443\r\n\r\n",
    b"GET /a%20b?x=1&y=2 HTTP/1.1\r\nHost: a\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n",
    b"DELETE /r HTTP/1.1\r\nHost: a\r\nX-A: 1\r\nX-A: 2\r\nContent-Length: 2\r\n\r\nok",
];

/// Bytes and short strings inserted by the mutations: line-ending and
/// whitespace trouble, NUL, high bytes, token and URI delimiters.
const SPECIALS: [&[u8]; 18] = [
    b"\r", b"\n", b"\r\n", b" ", b"\t", b":", b"\0", b"\x80", b"\xff", b"\x7f", b";", b",", b"\"",
    b"%", b"#", b"/", b"?", b"=",
];

const METHODS: [&[u8]; 9] = [
    b"GET", b"POST", b"get", b"PATCH", b"FOO", b"G E T", b"", b"TRACE", b"CONNECT",
];

const VERSIONS: [&[u8]; 7] = [
    b"HTTP/1.1",
    b"HTTP/1.0",
    b"HTTP/2.0",
    b"HTTP/1.2",
    b"HTTP/1",
    b"http/1.1",
    b"HTTP/1.1 ",
];

/// Header lines the mutations add after the request line: framing conflicts,
/// obsolete folding, malformed names, whitespace around the colon.
const HEADER_LINES: [&[u8]; 14] = [
    b"Content-Length: 5",
    b"Content-Length: 5, 5",
    b"Content-Length: +5",
    b"Content-Length: 0x5",
    b"Transfer-Encoding: chunked",
    b"Transfer-Encoding: gzip, chunked",
    b"Transfer-Encoding: identity",
    b"Host: b",
    b"X: y\r\n z",
    b"Bad Header: x",
    b": novalue",
    b"X:\ty",
    b"X : y",
    b"Expect: 100-continue",
];

fn choose<'a>(rng: &mut Rng, items: &'a [&'a [u8]]) -> &'a [u8] {
    items[usize::try_from(rng.below(items.len() as u64)).unwrap_or(0)]
}

fn index(rng: &mut Rng, len: usize) -> usize {
    if len == 0 {
        0
    } else {
        usize::try_from(rng.below(len as u64)).unwrap_or(0)
    }
}

/// Replace `data[from..to]` with `with` (`from <= to <= data.len()`).
fn replace_range(data: &mut Vec<u8>, from: usize, to: usize, with: &[u8]) {
    let tail = data.split_off(to);
    data.truncate(from);
    data.extend_from_slice(with);
    data.extend_from_slice(&tail);
}

/// Request-target forms the structured mutations substitute: origin, asterisk,
/// dot segments, percent-encoding, query and fragment, absolute with and
/// without an authority, authority form, and non-ASCII and control bytes.
const TARGETS: [&[u8]; 14] = [
    b"/",
    b"*",
    b"/a/../b",
    b"/a%2fb",
    b"/p?q=a+b&r",
    b"/p#frag",
    b"http://a/",
    b"http://a:80/p?q",
    b"http:///p",
    b"a:80",
    b"//",
    b"/a;b=c",
    b"/\xc3\xa9",
    b"/\x7f",
];

/// Content-Length value forms: zero, leading zero, surrounding whitespace, a
/// list, a sign.
const CL_VALUES: [&[u8]; 8] = [b"0", b"05", b"5 ", b" 5", b"5,5", b"+5", b"-0", b"5\t"];

/// First chunk-size line forms: extensions, a leading zero, trailing space,
/// a bare semicolon, lowercase hex, a quoted extension value.
const CHUNK_SIZES: [&[u8]; 6] = [b"5;ext=1", b"05", b"5 ", b"5;", b"a", b"5;x=\"y\""];

/// Well-formed header lines the structured mutations add.
const EXTRA_HEADERS: [&[u8]; 8] = [
    b"X-Custom: v",
    b"Connection: close",
    b"Connection: keep-alive",
    b"Accept: */*",
    b"User-Agent:",
    b"X-Empty:",
    b"x-lower: 1",
    b"X-Long: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
];

const WHITESPACE: [&[u8]; 3] = [b" ", b"\t", b"  "];

/// Start offsets of every occurrence of `needle` in `data`.
fn find_all(data: &[u8], needle: &[u8]) -> Vec<usize> {
    data.windows(needle.len())
        .enumerate()
        .filter_map(|(at, window)| (window == needle).then_some(at))
        .collect()
}

/// One of `places`, chosen at random (`None` when there are none).
fn pick_place(rng: &mut Rng, places: &[usize]) -> Option<usize> {
    places.get(index(rng, places.len())).copied()
}

/// One seeded mutation that keeps the request's shape and changes a part of
/// it a parser may treat differently: line endings, whitespace around the
/// colon, header-name case, repeated and folded fields, Content-Length and
/// chunk-size forms, the request target. Most results are still accepted by
/// at least one parser, so these are what makes the field comparison bite.
fn mutate_structured(rng: &mut Rng, data: &mut Vec<u8>) {
    let crlfs = find_all(data, b"\r\n");
    match rng.below(10) {
        0 => {
            // A bare LF where a CRLF was.
            if let Some(at) = pick_place(rng, &crlfs) {
                replace_range(data, at, at + 2, b"\n");
            }
        }
        1 => {
            // Whitespace before or after the colon of a header field.
            let first = crlfs.first().copied().unwrap_or(0);
            let colons: Vec<usize> = find_all(data, b":")
                .into_iter()
                .filter(|&at| at > first)
                .collect();
            if let Some(at) = pick_place(rng, &colons) {
                let white = choose(rng, &WHITESPACE);
                let at = if rng.chance(50) { at } else { at + 1 };
                replace_range(data, at, at, white);
            }
        }
        2 => {
            // The case of one header name flipped.
            let starts: Vec<usize> = crlfs.iter().map(|&at| at + 2).collect();
            if let Some(start) = pick_place(rng, &starts) {
                let end = data[start..]
                    .iter()
                    .position(|&b| b == b':' || b == b'\r')
                    .map_or(data.len(), |p| start + p);
                let upper = rng.chance(50);
                for byte in &mut data[start..end] {
                    *byte = if upper {
                        byte.to_ascii_uppercase()
                    } else {
                        byte.to_ascii_lowercase()
                    };
                }
            }
        }
        3 => {
            // A line repeated (a duplicate header, or the blank line).
            if crlfs.len() >= 2 {
                let i = 1 + index(rng, crlfs.len() - 1);
                let start = crlfs[i - 1] + 2;
                let end = crlfs[i] + 2;
                let copy: Vec<u8> = data[start..end].to_vec();
                replace_range(data, end, end, &copy);
            }
        }
        4 => {
            // The value of a Content-Length header replaced.
            let hits = find_all(data, b"ength: ");
            if let Some(at) = pick_place(rng, &hits) {
                let start = at + 7;
                let end = data[start..]
                    .iter()
                    .position(|b| !b.is_ascii_digit())
                    .map_or(data.len(), |p| start + p);
                let value = choose(rng, &CL_VALUES);
                replace_range(data, start, end, value);
            }
        }
        5 => {
            // The first chunk-size line replaced (chunked bodies).
            if let Some(&at) = find_all(data, b"\r\n\r\n").first() {
                let start = at + 4;
                let end = data[start..]
                    .windows(2)
                    .position(|w| w == b"\r\n")
                    .map_or(data.len(), |p| start + p);
                if start < end && data[start].is_ascii_hexdigit() {
                    let size = choose(rng, &CHUNK_SIZES);
                    replace_range(data, start, end, size);
                }
            }
        }
        6 => {
            // A well-formed extra header line after the request line.
            if let Some(&end) = crlfs.first() {
                let mut line: Vec<u8> = choose(rng, &EXTRA_HEADERS).to_vec();
                line.extend_from_slice(b"\r\n");
                replace_range(data, end + 2, end + 2, &line);
            }
        }
        7 => {
            // The request target replaced.
            let first_sp = data.iter().position(|&b| b == b' ');
            if let (Some(sp), Some(&line_end)) = (first_sp, crlfs.first())
                && sp < line_end
                && let Some(p) = data[sp + 1..line_end].iter().position(|&b| b == b' ')
            {
                let target = choose(rng, &TARGETS);
                replace_range(data, sp + 1, sp + 1 + p, target);
            }
        }
        8 => {
            // An obsolete line fold: the next line continues this one.
            if let Some(at) = pick_place(rng, &crlfs) {
                replace_range(data, at + 2, at + 2, b" ");
            }
        }
        _ => {
            // Blank lines before the request line (RFC 9112 section 2.2).
            replace_range(data, 0, 0, b"\r\n");
        }
    }
}

/// One seeded mutation of `data`: structured half the time, byte-level
/// (corrupting, mostly rejected by both parsers) otherwise.
fn mutate(rng: &mut Rng, data: &mut Vec<u8>) {
    if rng.chance(50) {
        mutate_structured(rng, data);
    } else {
        mutate_bytes(rng, data);
    }
}

/// One seeded byte-level mutation of `data`.
fn mutate_bytes(rng: &mut Rng, data: &mut Vec<u8>) {
    match rng.below(8) {
        0 => {
            // Replace one byte.
            if !data.is_empty() {
                let at = index(rng, data.len());
                data[at] = u8::try_from(rng.below(256)).unwrap_or(0);
            }
        }
        1 => {
            // Insert a special byte or short string.
            let at = index(rng, data.len() + 1);
            let special = choose(rng, &SPECIALS);
            replace_range(data, at, at, special);
        }
        2 => {
            // Delete a short range.
            if !data.is_empty() {
                let at = index(rng, data.len());
                let end = (at + 1 + index(rng, 8)).min(data.len());
                replace_range(data, at, end, b"");
            }
        }
        3 => {
            // Duplicate a short range in place.
            if !data.is_empty() {
                let at = index(rng, data.len());
                let end = (at + 1 + index(rng, 16)).min(data.len());
                let copy: Vec<u8> = data[at..end].to_vec();
                replace_range(data, end, end, &copy);
            }
        }
        4 => {
            // Cut the input short.
            if !data.is_empty() {
                let at = index(rng, data.len());
                data.truncate(at);
            }
        }
        5 => {
            // Replace the method or the version of the request line.
            let first_sp = data.iter().position(|&b| b == b' ');
            let line_end = data.windows(2).position(|w| w == b"\r\n");
            if let (Some(sp), Some(end)) = (first_sp, line_end)
                && sp < end
            {
                if rng.chance(50) {
                    let method = choose(rng, &METHODS);
                    replace_range(data, 0, sp, method);
                } else {
                    let last_sp = data[..end].iter().rposition(|&b| b == b' ').unwrap_or(sp);
                    if last_sp > sp {
                        let version = choose(rng, &VERSIONS);
                        replace_range(data, last_sp + 1, end, version);
                    }
                }
            }
        }
        6 => {
            // Add a header line after the request line.
            if let Some(end) = data.windows(2).position(|w| w == b"\r\n") {
                let mut line: Vec<u8> = choose(rng, &HEADER_LINES).to_vec();
                line.extend_from_slice(b"\r\n");
                replace_range(data, end + 2, end + 2, &line);
            }
        }
        _ => {
            // Splice the tail of another base onto a prefix of this input.
            let other = choose(rng, &BASES);
            let cut = index(rng, data.len() + 1);
            let from = index(rng, other.len() + 1);
            data.truncate(cut);
            data.extend_from_slice(&other[from..]);
        }
    }
}

/// `count` inputs for batch `seed`: the bases first, then seeded mutations
/// (one to three each) of a base chosen at random. Deterministic: the same
/// `(seed, count)` is the same list on every machine.
pub fn http_request_inputs(seed: u64, count: u64) -> Vec<Vec<u8>> {
    let mut rng = Rng::new(seed);
    let mut inputs = Vec::new();
    for n in 0..count {
        if let Some(base) = usize::try_from(n).ok().and_then(|i| BASES.get(i)) {
            inputs.push(base.to_vec());
            continue;
        }
        let mut data = choose(&mut rng, &BASES).to_vec();
        for _ in 0..1 + rng.below(3) {
            mutate(&mut rng, &mut data);
        }
        inputs.push(data);
    }
    inputs
}

fn usage() -> ExitCode {
    eprintln!(
        "usage: twin_run parse-fuzz http-request --seed <u64> --count <n> --out <dir>\n       \
         twin_run parse-diff http-request <file>..."
    );
    ExitCode::from(2)
}

fn parse_fuzz(seed: u64, count: u64, out: &Path) -> ExitCode {
    let inputs_dir = out.join("inputs");
    if let Err(err) = std::fs::create_dir_all(&inputs_dir) {
        eprintln!("twin_run: cannot create {}: {err}", inputs_dir.display());
        return ExitCode::from(1);
    }
    let mut lines = String::new();
    for (n, input) in http_request_inputs(seed, count).iter().enumerate() {
        let name = format!("{n:05}.bin");
        if let Err(err) = std::fs::write(inputs_dir.join(&name), input) {
            eprintln!("twin_run: cannot write {name}: {err}");
            return ExitCode::from(1);
        }
        lines.push_str(&json!({"input": name, "outcome": http_request_outcome(input)}).to_string());
        lines.push('\n');
    }
    let file = out.join("rust.jsonl");
    if let Err(err) = std::fs::write(&file, lines) {
        eprintln!("twin_run: cannot write {}: {err}", file.display());
        return ExitCode::from(1);
    }
    println!("twin_run parse-fuzz http-request: {count} input(s) for seed {seed}");
    ExitCode::SUCCESS
}

fn parse_diff(files: &[PathBuf]) -> ExitCode {
    for path in files {
        match std::fs::read(path) {
            Ok(bytes) => println!(
                "{}",
                json!({"input": path.display().to_string(), "outcome": http_request_outcome(&bytes)})
            ),
            Err(err) => {
                eprintln!("twin_run: cannot read {}: {err}", path.display());
                return ExitCode::from(1);
            }
        }
    }
    ExitCode::SUCCESS
}

/// `twin_run parse-fuzz ...` and `twin_run parse-diff ...` (args[0] is the
/// subcommand).
pub fn cli(args: &[String]) -> ExitCode {
    let (Some(sub), Some(target)) = (args.first(), args.get(1)) else {
        return usage();
    };
    if target != "http-request" {
        eprintln!("twin_run: unknown parse target {target:?} (only http-request)");
        return ExitCode::from(2);
    }
    let mut seed = None;
    let mut count = None;
    let mut out = None;
    let mut files = Vec::new();
    let mut rest = args[2..].iter();
    while let Some(arg) = rest.next() {
        match arg.as_str() {
            "--seed" => seed = rest.next().and_then(|v| v.parse::<u64>().ok()),
            "--count" => count = rest.next().and_then(|v| v.parse::<u64>().ok()),
            "--out" => out = rest.next().map(PathBuf::from),
            other => files.push(PathBuf::from(other)),
        }
    }
    match (sub.as_str(), seed, count, out) {
        ("parse-fuzz", Some(seed), Some(count), Some(out)) => parse_fuzz(seed, count, &out),
        ("parse-diff", ..) if !files.is_empty() => parse_diff(&files),
        _ => usage(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_get_parses_with_its_fields_and_consumed_length() {
        let input = b"GET /p?q=1 HTTP/1.1\r\nHost: a\r\nX-A: 1\r\n\r\n";
        assert_eq!(
            http_request_outcome(input),
            json!({
                "kind": "complete", "method": "GET", "target": "/p?q=1", "version": "HTTP/1.1",
                "headers": [["Host", "a"], ["X-A", "1"]], "trailers": [],
                "body_len": 0, "body_fnv": "cbf29ce484222325", "consumed": input.len(),
            })
        );
    }

    #[test]
    fn a_chunked_post_reports_the_decoded_body() {
        let input = b"POST /c HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
        let outcome = http_request_outcome(input);
        assert_eq!(outcome["kind"], "complete");
        assert_eq!(outcome["body_len"], 5);
        assert_eq!(outcome["body_fnv"], format!("{:016x}", fnv1a(b"hello")));
        assert_eq!(outcome["consumed"], input.len());
    }

    #[test]
    fn empty_cut_short_and_unsupported_inputs_are_not_complete() {
        assert_eq!(http_request_outcome(b""), json!({"kind": "empty"}));
        assert_eq!(
            http_request_outcome(b"GET / HTTP/1.1\r\nHost: a\r\n")["kind"],
            "error"
        );
        assert_eq!(
            http_request_outcome(b"GET / HTTP/9.9\r\n\r\n")["kind"],
            "error"
        );
    }

    #[test]
    fn bytes_print_as_the_json_string_of_the_same_code_points() {
        assert_eq!(latin1(b"a\x80\xff"), "a\u{80}\u{ff}");
        assert_eq!(fnv1a(b""), 0xcbf2_9ce4_8422_2325);
    }

    #[test]
    fn a_header_value_is_the_decoded_string_whatever_the_wire_bytes() {
        // Valid UTF-8 stays UTF-8 and any other value is decoded as Latin-1,
        // so these two different wire values are the same string.
        let utf8 = http_request_outcome(b"GET / HTTP/1.1\r\nHost: a\r\nX: \xc3\xa9\r\n\r\n");
        let latin = http_request_outcome(b"GET / HTTP/1.1\r\nHost: a\r\nX: \xe9\r\n\r\n");
        assert_eq!(utf8["headers"][1], json!(["X", "\u{e9}"]));
        assert_eq!(latin["headers"][1], json!(["X", "\u{e9}"]));
    }

    #[test]
    fn structured_mutations_keep_a_useful_share_of_requests_acceptable() {
        // Byte-level corruption alone left under a tenth of the corpus
        // accepted by both parsers, too few for the field comparison.
        let inputs = http_request_inputs(1, 600);
        let accepted = inputs
            .iter()
            .filter(|input| http_request_outcome(input)["kind"] == "complete")
            .count();
        assert!(
            accepted * 100 >= inputs.len() * 15,
            "only {accepted} of {} requests parse",
            inputs.len()
        );
    }

    #[test]
    fn the_inputs_are_deterministic_and_start_with_the_bases() {
        let first = http_request_inputs(7, 60);
        assert_eq!(first, http_request_inputs(7, 60));
        assert_ne!(first, http_request_inputs(8, 60));
        assert_eq!(first.len(), 60);
        for (input, base) in first.iter().zip(BASES.iter()) {
            assert_eq!(input.as_slice(), *base);
        }
    }
}
