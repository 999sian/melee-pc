// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * Bounded block cache over the player's disc image (a File or Blob).
 *
 * read(offset, size) returns a Uint8Array synchronously when every block is
 * resident, so a hit never suspends the wasm, and a Promise of one otherwise.
 */
export function createDiscCache(file, { blockBytes = 512 * 1024, maxBytes = 32 * 1024 * 1024 } = {}) {
  if (!file || !Number.isSafeInteger(file.size) || blockBytes < 1 || maxBytes < blockBytes) {
    throw Error('Invalid disc cache configuration.');
  }
  const blocks = new Map(); // insertion order doubles as LRU order
  const pending = new Map(); // concurrent misses on one block share a read
  let bytes = 0;

  function touch(index, data) {
    blocks.delete(index);
    blocks.set(index, data);
  }

  async function fetchBlock(index) {
    if (pending.has(index)) return pending.get(index);
    const promise = (async () => {
      const start = index * blockBytes;
      const end = Math.min(start + blockBytes, file.size);
      const data = new Uint8Array(await file.slice(start, end).arrayBuffer());
      if (data.length !== end - start) throw Error('Short disc read.');
      touch(index, data);
      bytes += data.length;
      while (bytes > maxBytes) {
        const [oldest, evicted] = blocks.entries().next().value;
        blocks.delete(oldest);
        bytes -= evicted.length;
      }
      return data;
    })();
    pending.set(index, promise);
    try {
      return await promise;
    } finally {
      pending.delete(index);
    }
  }

  function read(offset, size) {
    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(size) || offset < 0 || size < 0 ||
        offset + size > file.size) {
      throw Error('Disc read is out of bounds.');
    }
    if (!size) return new Uint8Array();
    const first = Math.floor(offset / blockBytes);
    const last = Math.floor((offset + size - 1) / blockBytes);
    const pieces = [];
    let missing = false;
    for (let index = first; index <= last; index++) {
      const data = blocks.get(index);
      if (data) {
        touch(index, data);
        pieces.push(data);
      } else {
        missing = true;
        pieces.push(fetchBlock(index));
      }
    }
    const combine = (values) => {
      const skip = offset - first * blockBytes;
      if (first === last) return values[0].subarray(skip, skip + size);
      const result = new Uint8Array(size);
      let written = 0;
      values.forEach((data, i) => {
        const start = i === 0 ? skip : 0;
        const n = Math.min(data.length - start, size - written);
        result.set(data.subarray(start, start + n), written);
        written += n;
      });
      return result;
    };
    return missing ? Promise.all(pieces).then(combine) : combine(pieces);
  }

  return { read, get residentBytes() { return bytes; } };
}
