//! The RaptorQ dual oracle (bead bd-9kll.16.2, plan item 2).
//!
//! The C encoder and decoder (`asx_raptorq_*`, RFC 6330 section 5.3) have their
//! golden vectors from the crates.io `raptorq` 2.0.1 crate. This module is the
//! other half of a dual oracle, and the vector generator the repository
//! lacked. For one case it prints what BOTH independent Rust implementations
//! make of it, asupersync's `SystematicEncoder` and `InactivationDecoder` at
//! the pinned rev and the `raptorq` crate, as FNV-1a 64 digests.
//! `tests/fuzz/fuzz_parsers.c` (the `ASX_DIFF_RAPTORQ` mode) prints the C
//! library's for the same case, and `tools/ci/parser_differential.py --target
//! raptorq` compares the three and names the odd one out.
//!
//! A case is the input file: ASCII `K T SEED R N MASK` (decimal, single
//! spaces, an optional final newline):
//!
//! * the source block is K symbols of T bytes of the generator of
//!   tests/unit/raptorq/test_raptorq.c (`s = seed; s = s * 1664525 +
//!   1013904223; byte = s >> 24`);
//! * encoding: the digest of each of the R repair symbols from ESI K;
//! * decoding: the K+N encoding symbols a decoder is given are the first K+N
//!   ESIs of 0..K+R-1 after a shuffle driven by MASK (`s = mask; for i from
//!   K+R-1 down to 1: s = s * 1664525 + 1013904223; j = (s >> 8) % (i + 1);
//!   swap(i, j)`). Whether a set of encoding symbols decodes is a property of
//!   the set, so every correct decoder must agree on it, and on the block.
//!
//! Anything else, or a value out of range, is an error on both sides (the
//! limits are the harness's, not the library's).

use asupersync::raptorq::decoder::{InactivationDecoder, ReceivedSymbol};
use asupersync::raptorq::systematic::SystematicEncoder;
use raptorq::{
    EncodingPacket, ObjectTransmissionInformation, PayloadId, SourceBlockDecoder,
    SourceBlockEncoder,
};
use serde_json::{Value, json};

use crate::scenario_gen::Rng;

/// Largest K, T and R a case may name: keeps the dense solver of asupersync's
/// implementation quick (a case is three encodes and three decodes).
const MAX_K: u32 = 2000;
const MAX_T: u32 = 256;
const MAX_R: u32 = 64;

/// 64-bit FNV-1a, the digest every side prints for a symbol or a block.
fn fnv1a(bytes: &[u8]) -> u64 {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for &byte in bytes {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

/// The source generator of tests/unit/raptorq/test_raptorq.c (`rq_fill`).
fn source_bytes(seed: u32, len: usize) -> Vec<u8> {
    let mut s = seed;
    (0..len)
        .map(|_| {
            s = s.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            (s >> 24) as u8
        })
        .collect()
}

/// The parsed case: `K T SEED R N MASK`.
struct Case {
    k: u32,
    t: u32,
    seed: u32,
    repairs: u32,
    extra: u32,
    mask: u32,
}

fn parse_case(input: &[u8]) -> Result<Case, String> {
    let text = std::str::from_utf8(input).map_err(|_| "not text".to_owned())?;
    let text = text.strip_suffix('\n').unwrap_or(text);
    let mut fields = [0u32; 6];
    let mut count = 0usize;
    for token in text.split(' ') {
        let value =
            if token.is_empty() || token.len() > 10 || !token.bytes().all(|b| b.is_ascii_digit()) {
                return Err("malformed field".to_owned());
            } else {
                token
                    .parse::<u64>()
                    .map_err(|_| "malformed field".to_owned())?
            };
        let value = u32::try_from(value).map_err(|_| "field out of range".to_owned())?;
        if count == 6 {
            return Err("too many fields".to_owned());
        }
        fields[count] = value;
        count += 1;
    }
    if count != 6 {
        return Err("too few fields".to_owned());
    }
    let case = Case {
        k: fields[0],
        t: fields[1],
        seed: fields[2],
        repairs: fields[3],
        extra: fields[4],
        mask: fields[5],
    };
    if !(1..=MAX_K).contains(&case.k)
        || !(1..=MAX_T).contains(&case.t)
        || case.repairs > MAX_R
        || case.extra > case.repairs
        || case.k + case.extra > pool_size(&case)
    {
        return Err("value out of range".to_owned());
    }
    Ok(case)
}

fn hex(value: u64) -> String {
    format!("{value:016x}")
}

/// The encoding symbol IDs a decoder is given: K+N of the pool, shuffled. The
/// pool is 0..K+R-1, or, when MASK is odd, the repair symbols K..K+R-1 alone
/// (a set with no systematic symbol, where the matrix is the whole story).
fn received_esis(case: &Case) -> Vec<u32> {
    let mut esis: Vec<u32> = if case.mask % 2 == 1 {
        (case.k..case.k + case.repairs).collect()
    } else {
        (0..case.k + case.repairs).collect()
    };
    let mut s = case.mask;
    for i in (1..esis.len()).rev() {
        s = s.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        let j = ((s >> 8) as usize) % (i + 1);
        esis.swap(i, j);
    }
    esis.truncate((case.k + case.extra) as usize);
    esis
}

/// The size of a case's pool of encoding symbols (see `received_esis`).
fn pool_size(case: &Case) -> u32 {
    if case.mask % 2 == 1 {
        case.repairs
    } else {
        case.k + case.repairs
    }
}

/// What the `raptorq` crate makes of a case: its repair symbols' digests and
/// whether its source packets are the source, and its decode of the received
/// set (the block's digest, or `fail`).
fn crate_oracle(case: &Case, source: &[u8], esis: &[u32]) -> (Vec<u64>, bool, String) {
    let t = u16::try_from(case.t).unwrap_or(u16::MAX);
    let config = ObjectTransmissionInformation::new(source.len() as u64, t, 1, 1, 1);
    let encoder = SourceBlockEncoder::new(0, &config, source);
    let size = case.t as usize;
    let systematic = encoder
        .source_packets()
        .iter()
        .enumerate()
        .all(|(i, p)| p.data() == &source[i * size..(i + 1) * size]);
    let repair_packets = if case.repairs == 0 {
        Vec::new()
    } else {
        encoder.repair_packets(0, case.repairs)
    };
    let repairs = repair_packets.iter().map(|p| fnv1a(p.data())).collect();
    let packets: Vec<EncodingPacket> = esis
        .iter()
        .map(|&esi| {
            let data = if esi < case.k {
                source[esi as usize * size..(esi as usize + 1) * size].to_vec()
            } else {
                repair_packets[(esi - case.k) as usize].data().to_vec()
            };
            EncodingPacket::new(PayloadId::new(0, esi), data)
        })
        .collect();
    let mut decoder = SourceBlockDecoder::new(0, &config, source.len() as u64);
    let decoded = decoder
        .decode(packets)
        .map_or_else(|| "fail".to_owned(), |block| hex(fnv1a(&block)));
    (repairs, systematic, decoded)
}

/// What asupersync makes of a case: its repair symbols' digests and its decode
/// of the received set, or the reason it gave none.
fn asupersync_oracle(
    case: &Case,
    source: &[u8],
    esis: &[u32],
) -> Result<(Vec<u64>, String, String), String> {
    let size = case.t as usize;
    let symbols: Vec<Vec<u8>> = source.chunks(size).map(<[u8]>::to_vec).collect();
    let encoder = SystematicEncoder::new(&symbols, size, 0)
        .ok_or_else(|| "constraint matrix singular".to_owned())?;
    let mut repair_data = Vec::new();
    for i in 0..case.repairs {
        repair_data.push(
            encoder
                .try_repair_symbol(case.k + i)
                .map_err(|err| format!("{err:?}"))?,
        );
    }
    let repairs = repair_data.iter().map(|s| fnv1a(s)).collect();

    let decoder = InactivationDecoder::new(case.k as usize, size, 0);
    let mut received = decoder.constraint_symbols();
    for &esi in esis {
        if esi < case.k {
            received.push(ReceivedSymbol::source(esi, symbols[esi as usize].clone()));
        } else {
            let (columns, coefficients) = decoder
                .repair_equation(esi)
                .map_err(|err| format!("{err:?}"))?;
            received.push(ReceivedSymbol::repair(
                esi,
                columns,
                coefficients,
                repair_data[(esi - case.k) as usize].clone(),
            ));
        }
    }
    let (decoded, reason) = match decoder.decode(&received) {
        Ok(result) => {
            let block: Vec<u8> = result.source.iter().flatten().copied().collect();
            (hex(fnv1a(&block)), String::new())
        }
        Err(err) => ("fail".to_owned(), format!("{err:?}")),
    };
    Ok((repairs, decoded, reason))
}

fn run_case(case: &Case) -> Value {
    let source = source_bytes(case.seed, case.k as usize * case.t as usize);
    let esis = received_esis(case);
    let (crate_repairs, systematic, crate_decoded) = crate_oracle(case, &source, &esis);
    match asupersync_oracle(case, &source, &esis) {
        Ok((repairs, decoded, reason)) => json!({
            "kind": "complete",
            "k": case.k,
            "t": case.t,
            "seed": case.seed,
            "repairs": case.repairs,
            "extra": case.extra,
            "mask": case.mask,
            "source_fnv": hex(fnv1a(&source)),
            "systematic_ok": systematic,
            "repair_fnv": repairs.into_iter().map(hex).collect::<Vec<_>>(),
            "cberner_fnv": crate_repairs.into_iter().map(hex).collect::<Vec<_>>(),
            "decode_fnv": decoded,
            "cberner_decode_fnv": crate_decoded,
            "decode_reason": reason,
        }),
        Err(err) => json!({"kind": "error", "detail": format!("asupersync: {err}")}),
    }
}

/// The outcome of one case file. A panic of either implementation is
/// reported, not swallowed.
pub fn outcome(input: &[u8]) -> Value {
    if input.is_empty() {
        return json!({"kind": "empty"});
    }
    match parse_case(input) {
        Err(detail) => json!({"kind": "error", "detail": detail}),
        Ok(case) => std::panic::catch_unwind(|| run_case(&case))
            .unwrap_or_else(|_| json!({"kind": "panic", "detail": "an implementation panicked"})),
    }
}

/// FNV-1a 64 continued from `h`: the digest of symbols hashed back to back.
fn fnv_extend(mut h: u64, bytes: &[u8]) -> u64 {
    for &byte in bytes {
        h ^= u64::from(byte);
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    h
}

fn spec_u64(item: &Value, key: &str) -> Result<u64, String> {
    item[key]
        .as_u64()
        .ok_or_else(|| format!("spec: {key} is not an unsigned integer"))
}

/// The vectors of one block row of tests/unit/raptorq/test_raptorq.c
/// `g_block_golden`: the digest of repair ESIs K..K+count-1 hashed back to
/// back, the first repair symbol, and the digest of ESI K+100000 then ESI
/// 2^24-1, from the `raptorq` crate (the reference) and from asupersync.
fn block_vector(item: &Value) -> Result<Value, String> {
    let k = u32::try_from(spec_u64(item, "k")?).map_err(|_| "spec: k".to_owned())?;
    let t = u32::try_from(spec_u64(item, "t")?).map_err(|_| "spec: t".to_owned())?;
    let seed = u32::try_from(spec_u64(item, "seed")?).map_err(|_| "spec: seed".to_owned())?;
    let count = u32::try_from(spec_u64(item, "count")?).map_err(|_| "spec: count".to_owned())?;
    let source = source_bytes(seed, k as usize * t as usize);
    let config = ObjectTransmissionInformation::new(
        source.len() as u64,
        u16::try_from(t).map_err(|_| "spec: t".to_owned())?,
        1,
        1,
        1,
    );
    let encoder = SourceBlockEncoder::new(0, &config, &source);
    let far_start = 100_000u32;
    let max_esi = (1u32 << 24) - 1;
    let mut digest = 0xcbf2_9ce4_8422_2325u64;
    let mut first = String::new();
    // `list` asks for the first repair symbols themselves (g_k10_repair).
    let list = usize::try_from(item["list"].as_u64().unwrap_or(0)).unwrap_or(0);
    let mut listed: Vec<String> = Vec::new();
    for (i, packet) in encoder
        .repair_packets(0, count.max(list as u32))
        .iter()
        .enumerate()
    {
        let as_hex: String = packet.data().iter().map(|b| format!("{b:02x}")).collect();
        if i < count as usize {
            digest = fnv_extend(digest, packet.data());
        }
        if i == 0 {
            first.clone_from(&as_hex);
        }
        if i < list {
            listed.push(as_hex);
        }
    }
    let mut far = 0xcbf2_9ce4_8422_2325u64;
    for repair_index in [far_start, max_esi - k] {
        far = fnv_extend(far, encoder.repair_packets(repair_index, 1)[0].data());
    }
    // asupersync: the same symbols through SystematicEncoder, or null where
    // it refuses an ESI.
    // Only up to MAX_K: its solver is dense, and K=8192 alone is minutes.
    let symbols: Vec<Vec<u8>> = source.chunks(t as usize).map(<[u8]>::to_vec).collect();
    let encoder_of_ours = if k <= MAX_K {
        SystematicEncoder::new(&symbols, t as usize, 0)
    } else {
        None
    };
    let ours = encoder_of_ours.map(|enc| {
        let mut d = 0xcbf2_9ce4_8422_2325u64;
        for i in 0..count {
            d = fnv_extend(d, &enc.repair_symbol(k + i));
        }
        let mut f = 0xcbf2_9ce4_8422_2325u64;
        let far = [k + far_start, max_esi]
            .iter()
            .map(|&esi| enc.try_repair_symbol(esi).ok())
            .collect::<Option<Vec<_>>>();
        let far = far.map(|syms| {
            for s in &syms {
                f = fnv_extend(f, s);
            }
            hex(f)
        });
        (hex(d), far)
    });
    Ok(json!({
        "digest": hex(digest),
        "first_repair": first,
        "listed": listed,
        "far_digest": hex(far),
        "asupersync_digest": ours.as_ref().map(|(d, _)| d.clone()),
        "asupersync_far_digest": ours.and_then(|(_, f)| f),
    }))
}

/// The vectors of one object row of `g_obj_golden`: the serialized OTI, and
/// per source block its K, the digest of its source symbols and the digest of
/// repair ESIs K..K+7 (the `raptorq` crate; asupersync has no equivalent
/// object framing at this rev).
fn object_vector(item: &Value) -> Result<Value, String> {
    let f = spec_u64(item, "f")?;
    let t = u16::try_from(spec_u64(item, "t")?).map_err(|_| "spec: t".to_owned())?;
    let z = u8::try_from(spec_u64(item, "z")?).map_err(|_| "spec: z".to_owned())?;
    let n = u16::try_from(spec_u64(item, "n")?).map_err(|_| "spec: n".to_owned())?;
    let al = u8::try_from(spec_u64(item, "al")?).map_err(|_| "spec: al".to_owned())?;
    let seed = u32::try_from(spec_u64(item, "seed")?).map_err(|_| "spec: seed".to_owned())?;
    let object = source_bytes(seed, usize::try_from(f).map_err(|_| "spec: f".to_owned())?);
    let config = ObjectTransmissionInformation::new(f, t, z, n, al);
    let encoder = raptorq::Encoder::new(&object, config);
    let blocks: Vec<Value> = encoder
        .get_block_encoders()
        .iter()
        .enumerate()
        .map(|(sbn, block)| {
            let sources = block.source_packets();
            let mut source_digest = 0xcbf2_9ce4_8422_2325u64;
            for p in &sources {
                source_digest = fnv_extend(source_digest, p.data());
            }
            let mut repair_digest = 0xcbf2_9ce4_8422_2325u64;
            for p in block.repair_packets(0, 8) {
                repair_digest = fnv_extend(repair_digest, p.data());
            }
            json!({
                "sbn": sbn,
                "k": sources.len(),
                "source_digest": hex(source_digest),
                "repair_digest": hex(repair_digest),
            })
        })
        .collect();
    let oti_hex: String = config
        .serialize()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    Ok(json!({"oti_hex": oti_hex, "blocks": blocks}))
}

/// The outcome of a vector spec (a JSON file: `{"blocks": [{"k", "t",
/// "seed", "count"}], "objects": [{"f", "t", "z", "n", "al", "seed"}]}`): what
/// the crate and asupersync compute for each row, for
/// tools/ci/check_raptorq_vectors.py to compare with the C test's table.
pub fn vectors_outcome(input: &[u8]) -> Value {
    let spec: Value = match serde_json::from_slice(input) {
        Ok(spec) => spec,
        Err(err) => return json!({"kind": "error", "detail": format!("spec: {err}")}),
    };
    let run = || -> Result<Value, String> {
        let blocks = spec["blocks"]
            .as_array()
            .map_or(&[][..], Vec::as_slice)
            .iter()
            .map(block_vector)
            .collect::<Result<Vec<_>, _>>()?;
        let objects = spec["objects"]
            .as_array()
            .map_or(&[][..], Vec::as_slice)
            .iter()
            .map(object_vector)
            .collect::<Result<Vec<_>, _>>()?;
        Ok(json!({"kind": "complete", "blocks": blocks, "objects": objects}))
    };
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(run))
        .unwrap_or_else(|_| Err("an implementation panicked".to_owned()))
        .unwrap_or_else(|detail| json!({"kind": "error", "detail": detail}))
}

fn text(case: &Case) -> Vec<u8> {
    format!(
        "{} {} {} {} {} {}\n",
        case.k, case.t, case.seed, case.repairs, case.extra, case.mask
    )
    .into_bytes()
}

/// Symbol sizes the corpus draws from: the odd and the aligned.
const SIZES: [u32; 11] = [1, 2, 3, 4, 5, 8, 15, 16, 31, 32, 64];

/// `count` cases for batch `seed`, deterministic: the cases of the C unit
/// test's golden table first, then every K from 1 to 120, then seeded cases
/// weighted to small K with an occasional larger one and to small N (exactly K
/// symbols, where a decoder may fail), and a few malformed cases.
pub fn inputs(seed: u64, count: u64) -> Vec<Vec<u8>> {
    let mut fixed: Vec<Vec<u8>> = Vec::new();
    // tests/unit/raptorq/test_raptorq.c g_block_golden.
    for (k, t, s, r) in [
        (1, 16, 1, 8),
        (2, 8, 2, 8),
        (10, 16, 3, 16),
        (101, 8, 4, 64),
        (1000, 4, 5, 64),
        (1024, 8, 6, 64),
    ] {
        fixed.push(text(&Case {
            k,
            t,
            seed: s,
            repairs: r,
            extra: 2,
            // Even: drawn from all the symbols, as the pool of 64 repair
            // symbols is smaller than K+2 for the large blocks.
            mask: s.wrapping_mul(2),
        }));
    }
    for k in 1..=120u32 {
        fixed.push(text(&Case {
            k,
            t: 8,
            seed: k,
            repairs: 12,
            extra: k % 3,
            // Odd (repair symbols only) where the 12 repair symbols hold
            // K+N of them, even otherwise.
            mask: (k.wrapping_mul(2_654_435_761) & !1) | u32::from(k <= 9),
        }));
    }
    let mut rng = Rng::new(seed);
    let mut inputs = Vec::new();
    for n in 0..count {
        if let Some(input) = usize::try_from(n).ok().and_then(|i| fixed.get(i)) {
            inputs.push(input.clone());
            continue;
        }
        let pick = rng.below(100);
        let k = if pick < 70 {
            1 + rng.below(40)
        } else if pick < 95 {
            1 + rng.below(120)
        } else {
            1 + rng.below(300)
        };
        let size = SIZES[usize::try_from(rng.below(SIZES.len() as u64)).unwrap_or(0)];
        let mut repairs = u32::try_from(rng.below(25)).unwrap_or(0);
        let extra_pick = rng.below(100);
        let extra = if extra_pick < 40 {
            0
        } else if extra_pick < 70 {
            1
        } else if extra_pick < 90 {
            2
        } else {
            u32::try_from(rng.below(u64::from(repairs) + 1)).unwrap_or(0)
        };
        let k = u32::try_from(k).unwrap_or(1);
        let mut extra = extra.min(repairs);
        let mut mask = u32::try_from(rng.below(1 << 32)).unwrap_or(0) & !1;
        // A third of the small blocks are given repair symbols only (odd
        // mask): R covers K+N of them, plus a little.
        if k <= 55 && rng.below(100) < 33 {
            extra = extra.min(2);
            repairs = (k + extra + u32::try_from(rng.below(5)).unwrap_or(0)).min(MAX_R);
            mask |= 1;
        }
        let case = Case {
            k,
            t: size,
            seed: u32::try_from(rng.below(1 << 32)).unwrap_or(0),
            repairs,
            extra,
            mask,
        };
        let mut data = text(&case);
        // One case in thirty is malformed in a way both sides must refuse.
        if rng.below(30) == 0 {
            match rng.below(5) {
                0 => data = b"0 8 1 4 0 0\n".to_vec(),
                1 => data = b"5 0 1 4 0 0\n".to_vec(),
                2 => data.extend_from_slice(b"7 "),
                3 => data = b"5  8 1 4 0 0\n".to_vec(),
                _ => data = b"5 8 1 4 5 0\n".to_vec(),
            }
        }
        inputs.push(data);
    }
    inputs
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_source_generator_is_the_c_test_s() {
        // rq_fill(1, ..): first byte of s = 1 * 1664525 + 1013904223.
        let s = 1u32.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        assert_eq!(source_bytes(1, 1), vec![(s >> 24) as u8]);
        assert_eq!(source_bytes(7, 0), Vec::<u8>::new());
    }

    #[test]
    fn a_case_parses_strictly() {
        assert!(parse_case(b"10 16 3 12 2 99\n").is_ok());
        assert!(parse_case(b"10 16 3 12 2 99").is_ok());
        for bad in [
            &b""[..],
            b"10 16 3 12 2",
            b"10 16 3 12 2 99 1",
            b"10  16 3 12 2 99",
            b"+10 16 3 12 2 99",
            b"0 16 3 12 2 99",
            b"10 0 3 12 2 99",
            b"10 16 3 65 2 99",
            b"10 16 3 12 13 99",
            b"2001 16 3 12 2 99",
            b"10 16 99999999999 12 2 99",
            b"10 16 3 12 2 99\n\n",
        ] {
            assert!(
                parse_case(bad).is_err(),
                "{:?}",
                String::from_utf8_lossy(bad)
            );
        }
    }

    #[test]
    fn the_received_set_is_a_shuffle_of_the_encoding_symbols() {
        let case = parse_case(b"10 16 3 12 2 5\n").unwrap();
        let esis = received_esis(&case);
        assert_eq!(esis.len(), 12);
        let mut sorted = esis.clone();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), 12, "no ESI twice");
        assert!(
            esis.iter().all(|&e| (10..22).contains(&e)),
            "odd mask: repair only"
        );
        assert_eq!(esis, received_esis(&case), "deterministic");
    }

    #[test]
    fn the_two_rust_implementations_agree_on_the_golden_k10_case() {
        // The first repair symbols of tests/unit/raptorq/test_raptorq.c
        // g_k10_repair: K=10, T=16, seed=3.
        let out = outcome(b"10 16 3 2 2 6\n");
        assert_eq!(out["kind"], "complete", "{out}");
        assert_eq!(out["systematic_ok"], true);
        assert_eq!(out["repair_fnv"], out["cberner_fnv"], "{out}");
        assert_eq!(out["decode_fnv"], out["cberner_decode_fnv"], "{out}");
        assert_eq!(
            out["decode_fnv"].as_str().unwrap(),
            out["source_fnv"].as_str().unwrap(),
            "ten of twelve symbols decode to the source: {out}"
        );
        let source = source_bytes(3, 160);
        let case = parse_case(b"10 16 3 1 0 0\n").unwrap();
        let (crate_repairs, _, _) = crate_oracle(&case, &source, &[]);
        let first: [u8; 16] = [
            0xe3, 0x9a, 0x28, 0x3a, 0x99, 0xcb, 0xcb, 0x50, 0x94, 0x1f, 0x13, 0x28, 0xa5, 0x58,
            0xab, 0xf5,
        ];
        assert_eq!(crate_repairs, vec![fnv1a(&first)]);
    }

    #[test]
    fn too_few_symbols_do_not_decode_in_either_implementation() {
        // K+0 symbols drawn at random from a 30-symbol pool for K=30 miss
        // some source symbol almost always, and a repair-only set needs the
        // matrix to be full rank; the oracles must agree whichever happens.
        let out = outcome(b"30 4 9 30 0 1\n");
        assert_eq!(out["kind"], "complete", "{out}");
        assert_eq!(out["decode_fnv"], out["cberner_decode_fnv"], "{out}");
    }

    #[test]
    fn inputs_are_deterministic_and_start_with_the_golden_cases() {
        let first = inputs(3, 200);
        assert_eq!(first, inputs(3, 200));
        assert_ne!(first, inputs(4, 200));
        assert_eq!(first[0], b"1 16 1 8 2 2\n");
        assert!(first.iter().all(|i| !i.is_empty()));
    }
}
