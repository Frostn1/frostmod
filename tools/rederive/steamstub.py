"""SteamStub (Steam DRM) v3.x unpacker.

The shipping mxbikes.exe/gpbikes.exe are SteamStub-wrapped: `.text` is AES-encrypted
in the file and only decrypted in memory at load. Every RVA in offsets.h is derived
from the *decrypted* image, so re-deriving anything starts here.

Verified byte-exact against the Steamless-produced mxbikes.exe.unpacked.exe for
beta21e (build 0x6A21833D): all 0x31F800 bytes of .text match.

The stub header sits in the 0xF0 bytes immediately before the packed entry point and
is obfuscated with a rolling XOR (each dword is XORed with the previous *ciphertext*
dword, seeded 0). Layout, once decoded:

    +0x00 u32  XorKey
    +0x04 u32  Signature = 0xC0DEC0DF
    +0x08 u64  ImageBase
    +0x10 u64  AddressOfEntryPoint (packed)
    +0x18 u32  BindSectionOffset
    +0x20 u64  OriginalEntryPoint   <- what we restore
    +0x2C u32  PayloadSize
    +0x38 u32  SteamAppID
    +0x3C u32  Flags                 (bit 2 = NoEncryption)
    +0x48 u64  CodeSectionVirtualAddress
    +0x50 u64  CodeSectionRawSize
    +0x58 b32  AES key
    +0x78 b16  AES IV   (itself ECB-encrypted under the same key)
    +0x88 b16  CodeSectionStolenData (the first block, lifted out of the file)

Decryption: iv = AES-256-ECB-decrypt(header.iv, key); plaintext = AES-256-CBC-decrypt(
stolen16 || packed_text, key, iv). The 16 stolen bytes are the first cipher block, so
the plaintext is exactly the section's raw size and drops straight back into place.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

SIGNATURE = 0xC0DEC0DF
HEADER_SIZE = 0xF0
FLAG_NO_ENCRYPTION = 0x4


class NotPacked(Exception):
    """The image carries no SteamStub header — it is already unpacked."""


@dataclass
class StubHeader:
    xor_key: int
    image_base: int
    packed_entry: int
    original_entry: int
    steam_app_id: int
    flags: int
    code_rva: int
    code_raw_size: int
    aes_key: bytes
    aes_iv: bytes
    stolen: bytes


def _rolling_xor(blob: bytes) -> bytes:
    out = bytearray(len(blob))
    prev = 0
    for i in range(0, len(blob) - 3, 4):
        cur = struct.unpack_from("<I", blob, i)[0]
        struct.pack_into("<I", out, i, cur ^ prev)
        prev = cur
    return bytes(out)


def read_header(pe) -> StubHeader:
    """Decode the stub header sitting before the packed entry point."""
    ep_off = pe.off(pe.entry_rva)
    if ep_off is None or ep_off < HEADER_SIZE:
        raise NotPacked("entry point has no room for a stub header")
    h = _rolling_xor(pe.data[ep_off - HEADER_SIZE:ep_off])
    if struct.unpack_from("<I", h, 4)[0] != SIGNATURE:
        raise NotPacked("no 0xC0DEC0DF signature before the entry point")
    return StubHeader(
        xor_key=struct.unpack_from("<I", h, 0x00)[0],
        image_base=struct.unpack_from("<Q", h, 0x08)[0],
        packed_entry=struct.unpack_from("<Q", h, 0x10)[0],
        original_entry=struct.unpack_from("<Q", h, 0x20)[0],
        steam_app_id=struct.unpack_from("<I", h, 0x38)[0],
        flags=struct.unpack_from("<I", h, 0x3C)[0],
        code_rva=struct.unpack_from("<Q", h, 0x48)[0],
        code_raw_size=struct.unpack_from("<Q", h, 0x50)[0],
        aes_key=h[0x58:0x78],
        aes_iv=h[0x78:0x88],
        stolen=h[0x88:0x98],
    )


def _aes(key: bytes, iv: bytes | None, data: bytes) -> bytes:
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError as exc:  # pragma: no cover - environment problem, not logic
        raise SystemExit(
            "the SteamStub unpacker needs a crypto backend: pip install cryptography"
        ) from exc
    mode = modes.CBC(iv) if iv is not None else modes.ECB()
    d = Cipher(algorithms.AES(key), mode).decryptor()
    return d.update(data) + d.finalize()


def unpack(pe) -> bytes:
    """Return a new image with .text decrypted and the entry point restored.

    Raises NotPacked if the file has no stub, so callers can accept either form.
    """
    h = read_header(pe)
    out = bytearray(pe.data)

    sec = pe.section_of(h.code_rva)
    if sec is None:
        raise ValueError(f"stub names a code section at {h.code_rva:#x} that isn't mapped")

    if not h.flags & FLAG_NO_ENCRYPTION:
        n = int(h.code_raw_size)
        cipher = h.stolen + pe.data[sec.rawptr:sec.rawptr + n]
        cipher = cipher[:len(cipher) // 16 * 16]
        iv = _aes(h.aes_key, None, h.aes_iv)
        plain = _aes(h.aes_key, iv, cipher)
        out[sec.rawptr:sec.rawptr + len(plain)] = plain

    # Point the entry point back at the game's own OEP so the image analyses as itself.
    struct.pack_into("<I", out, pe.opt + 16, int(h.original_entry) & 0xFFFFFFFF)
    return bytes(out)


def describe(h: StubHeader) -> str:
    return (
        f"SteamStub v3.x  app {h.steam_app_id}  flags {h.flags:#x}  "
        f"OEP {h.original_entry:#x}  code {h.code_rva:#x}+{h.code_raw_size:#x}"
    )
