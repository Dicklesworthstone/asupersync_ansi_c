//! Differential parsing against the C parsers (bead bd-9kll.16.2).
//!
//! The same bytes go to asupersync's HTTP/1.1 parsers here (the pinned rev's
//! `Http1Codec` for requests and `Http1ClientCodec` for responses, driven to
//! end of input) and to the C parser (the `ASX_DIFF_HTTP_REQUEST` and
//! `ASX_DIFF_HTTP_RESPONSE` modes of tests/fuzz/fuzz_parsers.c). Each side
//! prints one outcome object per input and `tools/ci/parser_differential.py`
//! compares them: accepted or rejected, then method (or status and reason),
//! target, version, headers, trailers, body length and digest, and the bytes
//! the first message used.
//!
//! A response is framed by the method of the request it answers (a HEAD
//! response has no body, a 2xx to CONNECT is a tunnel), so a response input
//! has one outcome per method, `GET`, `HEAD` and `CONNECT`, in one object.
//! The Rust side learns the method by encoding a request of that method into
//! the client codec first, as a client does.
//!
//! The WebSocket target (`ws-frame`) feeds the first frame of the input to
//! `FrameCodec` and to the C header functions (`asx_ws_header_decode`,
//! `asx_ws_header_check`, `asx_ws_mask` and the close-payload rules of the
//! engine, composed by the `ASX_DIFF_WS_FRAME` mode), once for each receiver
//! role, `client` and `server`, since the masking rule depends on it. A
//! rejected frame also carries the close code the receiver fails the
//! connection with, which is compared: it is what goes on the wire.
//!
//! `twin_run parse-fuzz http-request --seed <u64> --count <n> --out <dir>`
//! (or `http-response`, `ws-frame`) writes `<dir>/inputs/NNNNN.bin` (a fixed
//! list of well-formed messages, then seeded mutations of them) and
//! `<dir>/rust.jsonl`, the Rust outcome of each input in order.
//! `twin_run parse-diff http-request <file>...` prints the outcome of each
//! named file, which the minimizer re-runs on candidates.
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
use asupersync::codec::{Decoder, Encoder};
use asupersync::http::h1::types::{Method, Request};
use asupersync::http::h1::{Http1ClientCodec, Http1Codec};
use asupersync::net::websocket::{FrameCodec, Role};
use serde_json::{Map, Value, json};

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

/// The request methods a response is decoded against, with a target each
/// (CONNECT takes an authority).
const RESPONSE_METHODS: [(&str, Method, &str); 3] = [
    ("GET", Method::Get, "/"),
    ("HEAD", Method::Head, "/"),
    ("CONNECT", Method::Connect, "a:443"),
];

/// Decode the first response of `input` the way `FramedRead` does with a peer
/// that sends `input` and closes: `decode` on the buffered bytes, and
/// `decode_eof` only when that wants more. A close-delimited body is reported
/// by `decode_eof` only once `decode` has moved the codec into its EOF state,
/// so calling `decode_eof` alone reports `Io` for every response without a
/// length, which is a harness artifact and not a parser divergence.
fn decode_response(input: &[u8], method: Method, target: &str) -> Value {
    let mut codec = Http1ClientCodec::new();
    let mut scratch = BytesMut::new();
    let request = Request::builder(method, target).header("Host", "a").build();
    if let Err(err) = codec.encode(request, &mut scratch) {
        return json!({
            "kind": "error",
            "detail": format!("request encode: {}", variant_name(&format!("{err:?}"))),
        });
    }
    let mut buf = BytesMut::from(input);
    let decoded = match codec.decode(&mut buf) {
        Ok(None) => codec.decode_eof(&mut buf),
        other => other,
    };
    match decoded {
        Ok(Some(resp)) => json!({
            "kind": "complete",
            "status": resp.status,
            "reason": resp.reason,
            "version": resp.version.as_str(),
            "headers": pairs(&resp.headers),
            "trailers": pairs(&resp.trailers),
            "body_len": resp.body.len(),
            "body_fnv": format!("{:016x}", fnv1a(&resp.body)),
            "consumed": input.len() - buf.len(),
        }),
        Ok(None) => json!({"kind": "error", "detail": "no frame"}),
        Err(err) => json!({"kind": "error", "detail": variant_name(&format!("{err:?}"))}),
    }
}

/// The outcome of the first HTTP/1.1 response in `input` for each request
/// method in `RESPONSE_METHODS`, end of input signalled: an object keyed by
/// method name whose values are `empty`, `complete` with the parsed fields,
/// `error`, or `panic` as for a request.
pub fn http_response_outcome(input: &[u8]) -> Value {
    let mut by_method = Map::new();
    for (name, method, target) in RESPONSE_METHODS {
        let outcome = if input.is_empty() {
            json!({"kind": "empty"})
        } else {
            std::panic::catch_unwind(|| decode_response(input, method.clone(), target))
                .unwrap_or_else(|_| json!({"kind": "panic", "detail": "the parser panicked"}))
        };
        by_method.insert(name.to_owned(), outcome);
    }
    Value::Object(by_method)
}

/// The largest frame payload either side accepts in a frame comparison. RFC
/// 6455 leaves the limit to the endpoint; both drivers apply this one, so a
/// size policy cannot hide a framing difference. `tests/fuzz/fuzz_parsers.c`
/// holds the same number.
const WS_MAX_PAYLOAD: usize = 65_536;

/// The roles a frame is decoded as the receiver of: the masking rule differs
/// (a server requires a masked frame, a client rejects one).
const WS_ROLES: [(&str, Role); 2] = [("client", Role::Client), ("server", Role::Server)];

/// The mask key as 8 lowercase hex digits, or the empty string for an
/// unmasked frame.
fn hex_key(key: Option<[u8; 4]>) -> String {
    key.map_or_else(String::new, |k| {
        k.iter().map(|b| format!("{b:02x}")).collect::<String>()
    })
}

/// The first frame of `input` as the receiver `role` decodes it, end of input
/// signalled: `complete` with the header fields and the unmasked payload's
/// length and digest, or `error` with the reason and the close code the
/// receiver would fail the connection with (`incomplete` has no code: nothing
/// was violated yet).
fn decode_ws_frame(input: &[u8], role: Role) -> Value {
    let mut codec = FrameCodec::new(role).max_payload_size(WS_MAX_PAYLOAD);
    let mut buf = BytesMut::from(input);
    match codec.decode(&mut buf) {
        Ok(Some(frame)) => json!({
            "kind": "complete",
            "fin": frame.fin,
            "rsv": (u8::from(frame.rsv1) << 2) | (u8::from(frame.rsv2) << 1) | u8::from(frame.rsv3),
            "opcode": frame.opcode as u8,
            "masked": frame.masked,
            "mask_key": hex_key(frame.mask_key),
            "payload_len": frame.payload.len(),
            "payload_fnv": format!("{:016x}", fnv1a(frame.payload.as_ref())),
            "consumed": input.len() - buf.len(),
        }),
        Ok(None) => json!({"kind": "error", "detail": "incomplete"}),
        Err(err) => json!({
            "kind": "error",
            "detail": variant_name(&format!("{err:?}")),
            "close_code": u16::from(err.as_close_code()),
        }),
    }
}

/// The outcome of the first WebSocket frame in `input` for each receiver role
/// in `WS_ROLES`: an object keyed by role name whose values are `empty`,
/// `complete` with the parsed fields, `error`, or `panic`.
pub fn ws_frame_outcome(input: &[u8]) -> Value {
    let mut by_role = Map::new();
    for (name, role) in WS_ROLES {
        let outcome = if input.is_empty() {
            json!({"kind": "empty"})
        } else {
            std::panic::catch_unwind(|| decode_ws_frame(input, role))
                .unwrap_or_else(|_| json!({"kind": "panic", "detail": "the parser panicked"}))
        };
        by_role.insert(name.to_owned(), outcome);
    }
    Value::Object(by_role)
}

/// The kind of HTTP/1.1 message a corpus is made of.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Request,
    Response,
}

impl Kind {
    /// The outcome of `input` for this kind.
    pub fn outcome(self, input: &[u8]) -> Value {
        match self {
            Self::Request => http_request_outcome(input),
            Self::Response => http_response_outcome(input),
        }
    }
}

/// Which parser a corpus and a comparison are for.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Target {
    Http(Kind),
    WsFrame,
}

impl Target {
    fn parse(name: &str) -> Option<Self> {
        match name {
            "http-request" => Some(Self::Http(Kind::Request)),
            "http-response" => Some(Self::Http(Kind::Response)),
            "ws-frame" => Some(Self::WsFrame),
            _ => None,
        }
    }

    /// The outcome of `input` for this target.
    pub fn outcome(self, input: &[u8]) -> Value {
        match self {
            Self::Http(kind) => kind.outcome(input),
            Self::WsFrame => ws_frame_outcome(input),
        }
    }

    /// `count` inputs for batch `seed`, deterministic.
    pub fn inputs(self, seed: u64, count: u64) -> Vec<Vec<u8>> {
        match self {
            Self::Http(kind) => http_inputs(kind, seed, count),
            Self::WsFrame => ws_inputs(seed, count),
        }
    }
}

/// Well-formed requests the mutations start from: the shapes that matter for
/// framing (none, Content-Length, chunked with and without trailers), targets
/// (origin, absolute, asterisk, authority) and versions.
const REQUEST_BASES: [&[u8]; 10] = [
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

/// Well-formed responses the mutations start from: the framings (none,
/// Content-Length, chunked with and without a trailer, delimited by close),
/// the statuses that carry no body or switch protocols, a reason-less status
/// line, HTTP/1.0, repeated and lowercase headers.
const RESPONSE_BASES: [&[u8]; 12] = [
    b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
    b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\nX-T: y\r\n\r\n",
    b"HTTP/1.1 204 No Content\r\n\r\n",
    b"HTTP/1.1 304 Not Modified\r\nETag: \"a\"\r\n\r\n",
    b"HTTP/1.0 200 OK\r\nConnection: close\r\n\r\nbody until close",
    b"HTTP/1.1 100 Continue\r\n\r\n",
    b"HTTP/1.1 301 Moved Permanently\r\nLocation: /x\r\nContent-Length: 0\r\n\r\n",
    b"HTTP/1.1 404 Not Found\r\ncontent-length: 9\r\n\r\nnot found",
    b"HTTP/1.1 200 OK\r\nX-A: 1\r\nX-A: 2\r\nContent-Length: 2\r\n\r\nok",
    b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n",
    b"HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n",
];

fn bases(kind: Kind) -> &'static [&'static [u8]] {
    match kind {
        Kind::Request => &REQUEST_BASES,
        Kind::Response => &RESPONSE_BASES,
    }
}

/// Status codes and status-line tails the structured response mutations
/// substitute: boundaries, one digit short or long, signs, non-digits,
/// informational and no-body statuses, and reason phrases that are empty,
/// long, or hold a control byte.
const STATUS_LINES: [&[u8]; 12] = [
    b"99 Low",
    b"100 Continue",
    b"1000 High",
    b"600 Odd",
    b"000 Zero",
    b"+200 OK",
    b"2OO OK",
    b"204 No Content",
    b"200 ",
    b"200",
    b"200 O\x01K",
    b"200 OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK OK",
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

/// One seeded mutation that keeps the message's shape and changes a part of
/// it a parser may treat differently: line endings, whitespace around the
/// colon, header-name case, repeated and folded fields, Content-Length and
/// chunk-size forms, the request target (or a response's status line). Most
/// results are still accepted by at least one parser, so these are what makes
/// the field comparison bite.
fn mutate_structured(rng: &mut Rng, data: &mut Vec<u8>, kind: Kind) {
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
            // The request target replaced, or a response's status and
            // reason (everything after the version).
            let first_sp = data.iter().position(|&b| b == b' ');
            if let (Some(sp), Some(&line_end)) = (first_sp, crlfs.first())
                && sp < line_end
            {
                match kind {
                    Kind::Request => {
                        if let Some(p) = data[sp + 1..line_end].iter().position(|&b| b == b' ') {
                            let target = choose(rng, &TARGETS);
                            replace_range(data, sp + 1, sp + 1 + p, target);
                        }
                    }
                    Kind::Response => {
                        let line = choose(rng, &STATUS_LINES);
                        replace_range(data, sp + 1, line_end, line);
                    }
                }
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
fn mutate(rng: &mut Rng, data: &mut Vec<u8>, kind: Kind) {
    if rng.chance(50) {
        mutate_structured(rng, data, kind);
    } else {
        mutate_bytes(rng, data, kind);
    }
}

/// One seeded byte-level mutation of `data`.
fn mutate_bytes(rng: &mut Rng, data: &mut Vec<u8>, kind: Kind) {
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
            // Replace the first token of the start line (the method, or a
            // response's version) or the version at the end of a request line.
            let first_sp = data.iter().position(|&b| b == b' ');
            let line_end = data.windows(2).position(|w| w == b"\r\n");
            if let (Some(sp), Some(end)) = (first_sp, line_end)
                && sp < end
            {
                if rng.chance(50) {
                    let firsts: &[&[u8]] = match kind {
                        Kind::Request => &METHODS,
                        Kind::Response => &VERSIONS,
                    };
                    let first = choose(rng, firsts);
                    replace_range(data, 0, sp, first);
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
            let other = choose(rng, bases(kind));
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
pub fn http_inputs(kind: Kind, seed: u64, count: u64) -> Vec<Vec<u8>> {
    let mut rng = Rng::new(seed);
    let mut inputs = Vec::new();
    for n in 0..count {
        if let Some(base) = usize::try_from(n).ok().and_then(|i| bases(kind).get(i)) {
            inputs.push(base.to_vec());
            continue;
        }
        let mut data = choose(&mut rng, bases(kind)).to_vec();
        for _ in 0..1 + rng.below(3) {
            mutate(&mut rng, &mut data, kind);
        }
        inputs.push(data);
    }
    inputs
}

/// One WebSocket frame as RFC 6455 section 5.2 lays it out, before the
/// mutations turn it into bytes. `wide` forces a longer length form than the
/// length needs (0 minimal, 1 the 16-bit form, 2 the 64-bit form), and
/// `declared` is a length field that differs from the payload's own, so a
/// frame can be cut short or oversized without carrying the bytes.
#[derive(Clone)]
struct WsFrame {
    fin: bool,
    rsv: u8,
    opcode: u8,
    masked: bool,
    key: [u8; 4],
    wide: u8,
    declared: Option<u64>,
    payload: Vec<u8>,
}

impl WsFrame {
    fn new(opcode: u8, payload: &[u8]) -> Self {
        Self {
            fin: true,
            rsv: 0,
            opcode,
            masked: false,
            key: [0x37, 0xfa, 0x21, 0x3d],
            wide: 0,
            declared: None,
            payload: payload.to_vec(),
        }
    }

    fn masked(mut self) -> Self {
        self.masked = true;
        self
    }

    fn encode(&self) -> Vec<u8> {
        let len = self.declared.unwrap_or(self.payload.len() as u64);
        let mask_bit = u8::from(self.masked) << 7;
        let mut out = Vec::with_capacity(self.payload.len() + 14);
        out.push((u8::from(self.fin) << 7) | ((self.rsv & 7) << 4) | (self.opcode & 0x0f));
        if self.wide == 2 || len > 0xffff {
            out.push(mask_bit | 127);
            out.extend_from_slice(&len.to_be_bytes());
        } else if self.wide == 1 || len > 125 {
            out.push(mask_bit | 126);
            out.extend_from_slice(&(len as u16).to_be_bytes());
        } else {
            out.push(mask_bit | len as u8);
        }
        if self.masked {
            out.extend_from_slice(&self.key);
        }
        for (at, byte) in self.payload.iter().enumerate() {
            out.push(if self.masked {
                byte ^ self.key[at & 3]
            } else {
                *byte
            });
        }
        out
    }
}

/// Well-formed frames the mutations start from: each data opcode, masked and
/// not, the control opcodes with and without a close status, a fragment and a
/// continuation, and the payload lengths at the edges of the 7-bit and 16-bit
/// length forms.
fn ws_bases() -> Vec<WsFrame> {
    let close_bye = [&[0x03u8, 0xe8][..], b"bye"].concat();
    vec![
        WsFrame::new(1, b"hello"),
        WsFrame::new(1, b"hello").masked(),
        WsFrame::new(2, &[0, 1, 2, 3]),
        WsFrame::new(2, &[0, 1, 2, 3]).masked(),
        WsFrame::new(1, b""),
        WsFrame::new(9, b""),
        WsFrame::new(9, b"ping data").masked(),
        WsFrame::new(10, b"pong"),
        WsFrame::new(8, b""),
        WsFrame::new(8, &close_bye).masked(),
        WsFrame::new(8, &[0x03, 0xea]),
        WsFrame {
            fin: false,
            ..WsFrame::new(1, b"frag")
        },
        WsFrame::new(0, b"cont"),
        WsFrame::new(2, &[b'a'; 125]),
        WsFrame::new(2, &[b'a'; 126]),
        WsFrame::new(2, &[b'a'; 300]).masked(),
    ]
}

/// Length-field values the mutations declare: the edges of each length form,
/// a value past the payload limit, and the 64-bit forms with the top bit set.
const WS_DECLARED: [u64; 12] = [
    0,
    1,
    125,
    126,
    127,
    0xffff,
    0x1_0000,
    0x1_0001,
    1 << 32,
    (1 << 63) - 1,
    1 << 63,
    u64::MAX,
];

/// Payloads the mutations substitute: close status codes on both sides of
/// every range edge, valid and invalid UTF-8 reasons, and the control-frame
/// size edges.
fn ws_payloads() -> Vec<Vec<u8>> {
    vec![
        vec![],
        vec![0x03],
        vec![0x03, 0xe8],
        b"\x03\xe8bye".to_vec(),
        vec![0x03, 0xe7],
        vec![0x03, 0xec],
        vec![0x03, 0xed],
        vec![0x03, 0xee],
        vec![0x03, 0xf7],
        vec![0x03, 0xf8],
        vec![0x0b, 0xb7],
        vec![0x0b, 0xb8],
        vec![0x13, 0x87],
        vec![0x13, 0x88],
        b"\x03\xe8\xc3\xa9".to_vec(),
        b"\x03\xe8\xff".to_vec(),
        b"\x03\xe8\xc0\x80".to_vec(),
        b"\x03\xe8\xed\xa0\x80".to_vec(),
        b"\x03\xe8\xf4\x90\x80\x80".to_vec(),
        vec![b'a'; 125],
        vec![b'a'; 126],
        b"\xff\xfe".to_vec(),
    ]
}

/// One seeded mutation of a frame's fields.
fn mutate_ws_frame(rng: &mut Rng, frame: &mut WsFrame) {
    match rng.below(9) {
        0 => frame.fin = !frame.fin,
        1 => frame.rsv = 1 + u8::try_from(rng.below(7)).unwrap_or(0),
        2 => frame.opcode = u8::try_from(rng.below(16)).unwrap_or(0),
        3 => frame.masked = !frame.masked,
        4 => frame.wide = 1 + u8::try_from(rng.below(2)).unwrap_or(0),
        5 => frame.declared = Some(WS_DECLARED[index(rng, WS_DECLARED.len())]),
        6 => {
            let payloads = ws_payloads();
            frame.payload = payloads[index(rng, payloads.len())].clone();
            frame.declared = None;
        }
        7 => {
            for byte in &mut frame.key {
                *byte = u8::try_from(rng.below(256)).unwrap_or(0);
            }
        }
        _ => frame.opcode = [8, 9, 10][index(rng, 3)],
    }
}

/// One seeded byte-level mutation of encoded frame bytes: a flipped bit, a
/// replaced, inserted or duplicated byte, a cut, or a second frame appended.
fn mutate_ws_bytes(rng: &mut Rng, data: &mut Vec<u8>, bases: &[WsFrame]) {
    match rng.below(6) {
        0 => {
            if !data.is_empty() {
                let at = index(rng, data.len());
                data[at] ^= 1 << rng.below(8);
            }
        }
        1 => {
            if !data.is_empty() {
                let at = index(rng, data.len());
                data[at] = u8::try_from(rng.below(256)).unwrap_or(0);
            }
        }
        2 => {
            if !data.is_empty() {
                let at = index(rng, data.len());
                data.truncate(at);
            }
        }
        3 => {
            let at = index(rng, data.len() + 1);
            data.insert(at, u8::try_from(rng.below(256)).unwrap_or(0));
        }
        4 => {
            if !data.is_empty() {
                let at = index(rng, data.len());
                let end = (at + 1 + index(rng, 8)).min(data.len());
                let copy: Vec<u8> = data[at..end].to_vec();
                replace_range(data, end, end, &copy);
            }
        }
        _ => data.extend_from_slice(&bases[index(rng, bases.len())].encode()),
    }
}

/// Every proper prefix of each base frame up to `WS_PREFIX_MAX` bytes: the
/// frames cut inside the header, where a decoder that counts a field's bytes
/// wrongly (a mask key one byte short) accepts a header that is not there.
const WS_PREFIX_MAX: usize = 16;

fn ws_prefixes(bases: &[WsFrame]) -> Vec<Vec<u8>> {
    let mut prefixes = Vec::new();
    for base in bases {
        let bytes = base.encode();
        for cut in 1..bytes.len().min(WS_PREFIX_MAX + 1) {
            prefixes.push(bytes[..cut].to_vec());
        }
    }
    prefixes
}

/// `count` WebSocket inputs for batch `seed`: the bases first, then every
/// header prefix of each, then seeded mutations (one to three on the fields,
/// and a byte-level one three times in ten) of a base chosen at random.
/// Deterministic.
pub fn ws_inputs(seed: u64, count: u64) -> Vec<Vec<u8>> {
    let mut rng = Rng::new(seed);
    let bases = ws_bases();
    let fixed: Vec<Vec<u8>> = bases
        .iter()
        .map(WsFrame::encode)
        .chain(ws_prefixes(&bases))
        .collect();
    let mut inputs = Vec::new();
    for n in 0..count {
        if let Some(input) = usize::try_from(n).ok().and_then(|i| fixed.get(i)) {
            inputs.push(input.clone());
            continue;
        }
        let mut frame = bases[index(&mut rng, bases.len())].clone();
        for _ in 0..1 + rng.below(3) {
            mutate_ws_frame(&mut rng, &mut frame);
        }
        let mut data = frame.encode();
        if rng.chance(30) {
            mutate_ws_bytes(&mut rng, &mut data, &bases);
        }
        inputs.push(data);
    }
    inputs
}

fn usage() -> ExitCode {
    eprintln!(
        "usage: twin_run parse-fuzz (http-request|http-response|ws-frame) --seed <u64> \
         --count <n> --out <dir>\n       \
         twin_run parse-diff (http-request|http-response|ws-frame) <file>..."
    );
    ExitCode::from(2)
}

fn parse_fuzz(target: Target, seed: u64, count: u64, out: &Path) -> ExitCode {
    let inputs_dir = out.join("inputs");
    if let Err(err) = std::fs::create_dir_all(&inputs_dir) {
        eprintln!("twin_run: cannot create {}: {err}", inputs_dir.display());
        return ExitCode::from(1);
    }
    let mut lines = String::new();
    for (n, input) in target.inputs(seed, count).iter().enumerate() {
        let name = format!("{n:05}.bin");
        if let Err(err) = std::fs::write(inputs_dir.join(&name), input) {
            eprintln!("twin_run: cannot write {name}: {err}");
            return ExitCode::from(1);
        }
        lines.push_str(&json!({"input": name, "outcome": target.outcome(input)}).to_string());
        lines.push('\n');
    }
    let file = out.join("rust.jsonl");
    if let Err(err) = std::fs::write(&file, lines) {
        eprintln!("twin_run: cannot write {}: {err}", file.display());
        return ExitCode::from(1);
    }
    println!("twin_run parse-fuzz {target:?}: {count} input(s) for seed {seed}");
    ExitCode::SUCCESS
}

fn parse_diff(target: Target, files: &[PathBuf]) -> ExitCode {
    for path in files {
        match std::fs::read(path) {
            Ok(bytes) => println!(
                "{}",
                json!({"input": path.display().to_string(), "outcome": target.outcome(&bytes)})
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
    let Some(target) = Target::parse(target) else {
        eprintln!(
            "twin_run: unknown parse target {target:?} (http-request, http-response, ws-frame)"
        );
        return ExitCode::from(2);
    };
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
        ("parse-fuzz", Some(seed), Some(count), Some(out)) => parse_fuzz(target, seed, count, &out),
        ("parse-diff", ..) if !files.is_empty() => parse_diff(target, &files),
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
        for kind in [Kind::Request, Kind::Response] {
            let inputs = http_inputs(kind, 1, 600);
            let accepted = inputs
                .iter()
                .filter(|input| match kind.outcome(input) {
                    Value::Object(map) if kind == Kind::Response => {
                        map.values().any(|o| o["kind"] == "complete")
                    }
                    other => other["kind"] == "complete",
                })
                .count();
            assert!(
                accepted * 100 >= inputs.len() * 15,
                "only {accepted} of {} {kind:?} inputs parse",
                inputs.len()
            );
        }
    }

    #[test]
    fn the_inputs_are_deterministic_and_start_with_the_bases() {
        for kind in [Kind::Request, Kind::Response] {
            let first = http_inputs(kind, 7, 60);
            assert_eq!(first, http_inputs(kind, 7, 60));
            assert_ne!(first, http_inputs(kind, 8, 60));
            assert_eq!(first.len(), 60);
            for (input, base) in first.iter().zip(bases(kind).iter()) {
                assert_eq!(input.as_slice(), *base);
            }
        }
    }

    #[test]
    fn a_response_is_framed_by_the_method_it_answers() {
        let input = b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        let outcome = http_response_outcome(input);
        assert_eq!(outcome["GET"]["kind"], "complete");
        assert_eq!(outcome["GET"]["status"], 200);
        assert_eq!(outcome["GET"]["reason"], "OK");
        assert_eq!(outcome["GET"]["body_len"], 5);
        assert_eq!(outcome["GET"]["consumed"], input.len());
        // To a HEAD request the same bytes are a response without a body.
        assert_eq!(outcome["HEAD"]["kind"], "complete");
        assert_eq!(outcome["HEAD"]["body_len"], 0);
        assert!(outcome["CONNECT"].is_object());
    }

    #[test]
    fn a_close_delimited_response_is_complete_at_end_of_input() {
        // No Content-Length and no Transfer-Encoding: the body is whatever
        // arrives before the peer closes, and the head alone is a response
        // with an empty body.
        let with_body = http_response_outcome(b"HTTP/1.1 200 OK\r\n\r\nhello");
        assert_eq!(with_body["GET"]["kind"], "complete");
        assert_eq!(with_body["GET"]["body_len"], 5);
        let head_only = http_response_outcome(b"HTTP/1.1 301\r\n\r\n");
        assert_eq!(head_only["GET"]["kind"], "complete");
        assert_eq!(head_only["GET"]["status"], 301);
        assert_eq!(head_only["GET"]["body_len"], 0);
    }

    #[test]
    fn an_empty_or_cut_short_response_is_not_complete() {
        let empty = http_response_outcome(b"");
        assert_eq!(empty["GET"], json!({"kind": "empty"}));
        let cut = http_response_outcome(b"HTTP/1.1 200 OK\r\nContent-Le");
        assert_eq!(cut["GET"]["kind"], "error");
    }

    #[test]
    fn targets_are_named_on_the_command_line() {
        assert_eq!(
            Target::parse("http-request"),
            Some(Target::Http(Kind::Request))
        );
        assert_eq!(
            Target::parse("http-response"),
            Some(Target::Http(Kind::Response))
        );
        assert_eq!(Target::parse("ws-frame"), Some(Target::WsFrame));
        assert_eq!(Target::parse("websocket"), None);
    }

    #[test]
    fn a_masked_frame_is_the_servers_to_accept_and_the_clients_to_reject() {
        let input = WsFrame::new(1, b"hello").masked().encode();
        let outcome = ws_frame_outcome(&input);
        let server = &outcome["server"];
        assert_eq!(server["kind"], "complete");
        assert_eq!(server["fin"], true);
        assert_eq!(server["opcode"], 1);
        assert_eq!(server["masked"], true);
        assert_eq!(server["mask_key"], "37fa213d");
        assert_eq!(server["payload_len"], 5);
        assert_eq!(server["consumed"], input.len());
        assert_eq!(
            server["payload_fnv"],
            format!("{:016x}", fnv1a(b"hello")).as_str()
        );
        let client = &outcome["client"];
        assert_eq!(client["kind"], "error");
        assert_eq!(client["detail"], "MaskedServerFrame");
        assert_eq!(client["close_code"], 1002);
    }

    #[test]
    fn an_error_carries_the_close_code_the_receiver_fails_with() {
        // A control frame with a payload over 125 bytes: asupersync maps it
        // to 1009 (bd-9kll.16.2 records the C side's 1002 against it).
        let big_ping = WsFrame::new(9, &[b'a'; 126]).masked().encode();
        let outcome = ws_frame_outcome(&big_ping);
        assert_eq!(outcome["server"]["detail"], "ControlFrameTooLarge");
        assert_eq!(outcome["server"]["close_code"], 1009);
        // A cut-short frame is not a violation: no code.
        let cut = ws_frame_outcome(&[0x81, 0x85, 0x01]);
        assert_eq!(cut["server"]["detail"], "incomplete");
        assert!(cut["server"].get("close_code").is_none());
        assert_eq!(ws_frame_outcome(b"")["server"], json!({"kind": "empty"}));
    }

    #[test]
    fn ws_bases_decode_for_the_role_that_matches_their_masking() {
        for base in ws_bases() {
            let outcome = ws_frame_outcome(&base.encode());
            let role = if base.masked { "server" } else { "client" };
            assert_eq!(outcome[role]["kind"], "complete", "{:?}", base.encode());
        }
    }

    #[test]
    fn ws_inputs_hold_every_header_prefix_of_each_base() {
        let bases = ws_bases();
        let prefixes = ws_prefixes(&bases);
        let inputs = ws_inputs(1, (bases.len() + prefixes.len()) as u64);
        assert_eq!(&inputs[bases.len()..], prefixes.as_slice());
        // A masked text frame of 5 bytes has a 6-byte header with its key: the
        // 5-byte prefix is one byte short of complete.
        let masked = WsFrame::new(1, b"hello").masked().encode();
        assert!(prefixes.contains(&masked[..5].to_vec()));
        assert!(prefixes.iter().all(|p| p.len() <= WS_PREFIX_MAX));
    }

    #[test]
    fn ws_inputs_are_deterministic_start_with_the_bases_and_mostly_parse() {
        // The bases and their prefixes are the same for every seed; the seed
        // is what the inputs after them differ in.
        let first = ws_inputs(7, 600);
        assert_eq!(first, ws_inputs(7, 600));
        assert_ne!(first, ws_inputs(8, 600));
        for (input, base) in first.iter().zip(ws_bases().iter()) {
            assert_eq!(input, &base.encode());
        }
        let inputs = ws_inputs(1, 600);
        let accepted = inputs
            .iter()
            .filter(|input| {
                ws_frame_outcome(input)
                    .as_object()
                    .is_some_and(|m| m.values().any(|o| o["kind"] == "complete"))
            })
            .count();
        assert!(
            accepted * 100 >= inputs.len() * 15,
            "only {accepted} of {} frame inputs parse",
            inputs.len()
        );
    }
}
