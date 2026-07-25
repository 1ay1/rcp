"""Compact dense-vector codec for RCP/1 (spec §7.3.1).

The wire carries embeddings either as JSON number arrays (`"json"`, the default)
or as base64-encoded little-endian IEEE-754 ``binary32`` (`"f32-base64"`). A
1000×1024 batch is ~4 MB binary vs ~10–20 MB as decimal text, so the compact
encoding is a real bandwidth win for any server that embeds in bulk.

These helpers are dependency-free (``struct`` + ``base64`` only) and are the
reference implementation every SDK matches byte-for-byte: little-endian, 4 bytes
per float, standard base64 with padding.
"""
from __future__ import annotations

import base64
import struct

JSON = "json"
F32_BASE64 = "f32-base64"


def encode_vectors(vectors, encoding: str = JSON):
    """Encode a list of float vectors for the wire.

    Returns ``(payload, meta)`` where ``payload`` is the value for ``vectors``
    and ``meta`` is the extra result fields (``{}`` for json, or
    ``{"encoding": ..., "dimension": ...}`` for a binary encoding). All vectors
    MUST share one dimension under a binary encoding.
    """
    if encoding == JSON:
        return [list(v) for v in vectors], {}
    if encoding == F32_BASE64:
        dim = len(vectors[0]) if vectors else 0
        out = []
        for v in vectors:
            if len(v) != dim:
                raise ValueError("all vectors must share one dimension for f32-base64")
            out.append(base64.b64encode(struct.pack(f"<{dim}f", *v)).decode("ascii"))
        return out, {"encoding": F32_BASE64, "dimension": dim}
    raise ValueError(f"unknown vector encoding {encoding!r}")


def decode_vectors(payload, encoding: str | None = None, dimension: int | None = None):
    """Decode the ``vectors`` field of an embed/multi/retrieve result into a
    list of ``list[float]``. ``encoding`` defaults to ``"json"`` when absent
    (spec §7.3.1). Raises ``ValueError`` on a blob whose length is not a whole
    number of floats, or not ``dimension × 4`` bytes when ``dimension`` is given.
    """
    if encoding in (None, JSON):
        return [list(v) for v in payload]
    if encoding == F32_BASE64:
        out = []
        for blob in payload:
            raw = base64.b64decode(blob, validate=True)
            if len(raw) % 4 != 0:
                raise ValueError("f32-base64 blob length is not a multiple of 4 bytes")
            n = len(raw) // 4
            if dimension is not None and n != dimension:
                raise ValueError(f"blob has {n} floats, expected dimension {dimension}")
            out.append(list(struct.unpack(f"<{n}f", raw)))
        return out
    raise ValueError(f"unknown vector encoding {encoding!r}")
