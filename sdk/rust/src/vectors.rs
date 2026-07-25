//! vectors.rs — compact dense-vector codec for RCP/1 (spec §7.3.1).
//!
//! The wire carries embeddings either as JSON number arrays (`"json"`, the
//! default) or as base64-encoded little-endian IEEE-754 binary32
//! (`"f32-base64"`). A 1000×1024 batch is ~4 MB binary vs ~10–20 MB as decimal
//! text, so the compact encoding is a real bandwidth win for bulk embedding.
//!
//! Zero dependencies (Rust standard library only). This is the reference
//! implementation every SDK matches byte-for-byte: little-endian, 4 bytes per
//! float, standard base64 with padding.

/// The `"json"` encoding (default): vectors as JSON number arrays.
pub const JSON: &str = "json";
/// The `"f32-base64"` encoding: little-endian binary32, base64 with padding.
pub const F32_BASE64: &str = "f32-base64";

/// Encoded vectors plus the extra result fields to emit alongside them.
pub struct Encoded {
    /// One base64 blob per vector (for `f32-base64`), else empty when `json`.
    pub blobs: Vec<String>,
    /// The vectors themselves, when `json` was requested; else empty.
    pub json: Vec<Vec<f32>>,
    /// `Some("f32-base64", dimension)` for a binary encoding; `None` for json.
    pub meta: Option<(String, usize)>,
}

/// Encode a batch of float vectors for the wire under `encoding`.
///
/// All vectors MUST share one dimension under a binary encoding; returns `Err`
/// otherwise (or on an unknown encoding).
pub fn encode_vectors(vectors: &[Vec<f32>], encoding: &str) -> Result<Encoded, String> {
    match encoding {
        JSON => Ok(Encoded {
            blobs: Vec::new(),
            json: vectors.to_vec(),
            meta: None,
        }),
        F32_BASE64 => {
            let dim = vectors.first().map_or(0, |v| v.len());
            let mut blobs = Vec::with_capacity(vectors.len());
            for v in vectors {
                if v.len() != dim {
                    return Err("all vectors must share one dimension for f32-base64".into());
                }
                let mut raw = Vec::with_capacity(dim * 4);
                for &x in v {
                    raw.extend_from_slice(&x.to_le_bytes());
                }
                blobs.push(base64_encode(&raw));
            }
            Ok(Encoded {
                blobs,
                json: Vec::new(),
                meta: Some((F32_BASE64.to_string(), dim)),
            })
        }
        other => Err(format!("unknown vector encoding {other:?}")),
    }
}

/// Decode base64 blobs (as produced by [`encode_vectors`]) back into vectors.
///
/// `dimension`, when given, is enforced. Returns `Err` on a blob whose byte
/// length is not a whole number of floats, or not `dimension×4` bytes.
pub fn decode_f32_base64(blobs: &[String], dimension: Option<usize>) -> Result<Vec<Vec<f32>>, String> {
    let mut out = Vec::with_capacity(blobs.len());
    for blob in blobs {
        let raw = base64_decode(blob)?;
        if raw.len() % 4 != 0 {
            return Err("f32-base64 blob length is not a multiple of 4 bytes".into());
        }
        let n = raw.len() / 4;
        if let Some(d) = dimension {
            if n != d {
                return Err(format!("blob has {n} floats, expected dimension {d}"));
            }
        }
        let mut v = Vec::with_capacity(n);
        for chunk in raw.chunks_exact(4) {
            v.push(f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
        }
        out.push(v);
    }
    Ok(out)
}

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

fn base64_encode(raw: &[u8]) -> String {
    let mut out = String::with_capacity((raw.len() + 2) / 3 * 4);
    for chunk in raw.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(B64[((n >> 18) & 63) as usize] as char);
        out.push(B64[((n >> 12) & 63) as usize] as char);
        out.push(if chunk.len() > 1 { B64[((n >> 6) & 63) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { B64[(n & 63) as usize] as char } else { '=' });
    }
    out
}

fn base64_decode(s: &str) -> Result<Vec<u8>, String> {
    fn val(c: u8) -> Result<u32, String> {
        match c {
            b'A'..=b'Z' => Ok((c - b'A') as u32),
            b'a'..=b'z' => Ok((c - b'a' + 26) as u32),
            b'0'..=b'9' => Ok((c - b'0' + 52) as u32),
            b'+' => Ok(62),
            b'/' => Ok(63),
            _ => Err(format!("invalid base64 byte {c:?}")),
        }
    }
    let bytes: Vec<u8> = s.bytes().filter(|&c| c != b'=' && !c.is_ascii_whitespace()).collect();
    let mut out = Vec::with_capacity(bytes.len() / 4 * 3);
    for chunk in bytes.chunks(4) {
        // Accumulate 6 bits per sextet, then emit one byte per completed 8 bits.
        let mut acc = 0u32;
        let mut bits = 0u32;
        for &c in chunk {
            acc = (acc << 6) | val(c)?;
            bits += 6;
            if bits >= 8 {
                bits -= 8;
                out.push(((acc >> bits) & 0xff) as u8);
            }
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn f32_base64_round_trips() {
        let vecs = vec![vec![1.5f32, -2.25, 0.0, 3.125], vec![0.5, 0.5, 0.5, 0.5]];
        let enc = encode_vectors(&vecs, F32_BASE64).unwrap();
        assert_eq!(enc.meta, Some(("f32-base64".to_string(), 4)));
        let back = decode_f32_base64(&enc.blobs, Some(4)).unwrap();
        assert_eq!(back, vecs);
    }

    #[test]
    fn json_passes_through() {
        let vecs = vec![vec![1.0f32, 2.0, 3.0]];
        let enc = encode_vectors(&vecs, JSON).unwrap();
        assert!(enc.meta.is_none());
        assert_eq!(enc.json, vecs);
    }

    #[test]
    fn base64_matches_reference_vectors() {
        // Cross-check the codec against known base64 (interop with other SDKs).
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
        assert_eq!(base64_decode("Zm9vYmFy").unwrap(), b"foobar");
        assert_eq!(base64_decode("Zm8=").unwrap(), b"fo");
    }

    #[test]
    fn ragged_rejected_and_dimension_enforced() {
        assert!(encode_vectors(&[vec![1.0, 2.0], vec![3.0]], F32_BASE64).is_err());
        let enc = encode_vectors(&[vec![1.0f32, 2.0, 3.0, 4.0]], F32_BASE64).unwrap();
        assert!(decode_f32_base64(&enc.blobs, Some(8)).is_err());
    }
}
