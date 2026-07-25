// rcp/vectors — compact dense-vector codec for RCP/1 (spec §7.3.1).
//
// The wire carries embeddings either as JSON number arrays (`"json"`, the
// default) or as base64-encoded little-endian IEEE-754 binary32
// (`"f32-base64"`). A 1000×1024 batch is ~4 MB binary vs ~10–20 MB as decimal
// text, so the compact encoding is a real bandwidth win for bulk embedding.
//
// Zero dependencies (Node standard library only). This is the reference
// implementation every SDK matches byte-for-byte: little-endian, 4 bytes per
// float, standard base64 with padding.

export const JSON_ENC = "json";
export const F32_BASE64 = "f32-base64";

// Encode a list of float vectors for the wire. Returns { payload, meta } where
// `payload` is the value for `vectors` and `meta` is the extra result fields
// ({} for json, or {encoding, dimension} for a binary encoding). All vectors
// MUST share one dimension under a binary encoding.
export function encodeVectors(vectors, encoding = JSON_ENC) {
  if (encoding === JSON_ENC) {
    return { payload: vectors.map((v) => Array.from(v)), meta: {} };
  }
  if (encoding === F32_BASE64) {
    const dim = vectors.length ? vectors[0].length : 0;
    const payload = vectors.map((v) => {
      if (v.length !== dim) throw new Error("all vectors must share one dimension for f32-base64");
      const buf = Buffer.alloc(dim * 4);
      for (let i = 0; i < dim; i++) buf.writeFloatLE(v[i], i * 4);
      return buf.toString("base64");
    });
    return { payload, meta: { encoding: F32_BASE64, dimension: dim } };
  }
  throw new Error(`unknown vector encoding ${encoding}`);
}

// Decode the `vectors` field of an embed/retrieve result into number[][].
// `encoding` defaults to "json" when null/undefined (spec §7.3.1). Throws on a
// blob whose length is not a whole number of floats, or not dimension×4 bytes
// when `dimension` is given.
export function decodeVectors(payload, encoding = null, dimension = null) {
  if (encoding == null || encoding === JSON_ENC) {
    return payload.map((v) => Array.from(v));
  }
  if (encoding === F32_BASE64) {
    return payload.map((blob) => {
      const buf = Buffer.from(blob, "base64");
      if (buf.length % 4 !== 0) throw new Error("f32-base64 blob length is not a multiple of 4 bytes");
      const n = buf.length / 4;
      if (dimension != null && n !== dimension) {
        throw new Error(`blob has ${n} floats, expected dimension ${dimension}`);
      }
      const out = new Array(n);
      for (let i = 0; i < n; i++) out[i] = buf.readFloatLE(i * 4);
      return out;
    });
  }
  throw new Error(`unknown vector encoding ${encoding}`);
}
