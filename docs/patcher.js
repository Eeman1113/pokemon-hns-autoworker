/* Auto Worker web patcher.
 *
 * Everything happens in the browser. The ROM is read with FileReader and never
 * sent anywhere -- there is no upload endpoint and no network call except
 * fetching the patch file itself from this same site. That is not a privacy
 * nicety, it is the point: distributing a patch is fine, distributing someone
 * else's game is not, so the user's own copy has to stay on their machine. */

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes, start = 0, end = bytes.length) {
  let c = 0xFFFFFFFF;
  for (let i = start; i < end; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}
const hex8 = n => n.toString(16).padStart(8, "0");

/* UPS stores a variable-length value with a continuation bit in bit 7. */
function readVLV(buf, pos) {
  let value = 0, shift = 1;
  for (;;) {
    const b = buf[pos++];
    value += (b & 0x7F) * shift;
    if (b & 0x80) break;
    shift <<= 7;
    value += shift;
  }
  return [value, pos];
}

/* Apply, verifying every checksum the format carries. A patcher that skips the
 * source CRC will cheerfully build a corrupt ROM from the wrong base, and the
 * user finds out hours into a playthrough rather than here. */
function applyUps(rom, patch, onProgress) {
  if (String.fromCharCode(...patch.slice(0, 4)) !== "UPS1")
    throw new Error("That patch file is not a UPS patch.");

  const n = patch.length;
  const crcSrc = new DataView(patch.buffer, patch.byteOffset + n - 12, 4).getUint32(0, true);
  const crcDst = new DataView(patch.buffer, patch.byteOffset + n - 8, 4).getUint32(0, true);
  const crcPat = new DataView(patch.buffer, patch.byteOffset + n - 4, 4).getUint32(0, true);

  if (crc32(patch, 0, n - 4) !== crcPat)
    throw new Error("The patch file is corrupt (its own checksum does not match).");

  const haveSrc = crc32(rom);
  if (haveSrc !== crcSrc)
    throw new Error(
      `Wrong base ROM.\n\nYours:     ${hex8(haveSrc)}\nExpected:  ${hex8(crcSrc)}\n\n` +
      `This patch needs an unmodified Pokémon Emerald (USA, Europe), 16 MiB. ` +
      `A different region, a trimmed dump or an already-patched ROM will not work.`);

  let pos = 4, sizeIn, sizeOut;
  [sizeIn, pos] = readVLV(patch, pos);
  [sizeOut, pos] = readVLV(patch, pos);
  if (sizeIn !== rom.length)
    throw new Error(`Base ROM is ${rom.length.toLocaleString()} bytes; this patch expects ${sizeIn.toLocaleString()}.`);

  const out = new Uint8Array(sizeOut);
  out.set(rom.subarray(0, Math.min(rom.length, sizeOut)));

  let offset = 0;
  const end = n - 12;
  let lastTick = 0;
  while (pos < end) {
    let rel;
    [rel, pos] = readVLV(patch, pos);
    offset += rel;
    for (;;) {
      const x = patch[pos++];
      if (x === 0) { offset++; break; }
      if (offset < sizeOut) out[offset] ^= x;
      offset++;
    }
    if (onProgress && pos - lastTick > 1_000_000) { lastTick = pos; onProgress(pos / end); }
  }

  const haveDst = crc32(out);
  if (haveDst !== crcDst)
    throw new Error(`Patching produced an unexpected result (${hex8(haveDst)} vs ${hex8(crcDst)}). Please report this.`);

  return out;
}
