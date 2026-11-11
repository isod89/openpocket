// SPDX-License-Identifier: GPL-3.0-only
// Read the complete factory sound bank from running BlackBox V21 over USB MIDI.
import { pack7, unpack7 } from "./pxota.js";
import { sha256hex } from "./pxpkg.js";

export const BLACKBOX_V21 = "BlackBox_021";
const AMP_CAB_SHA = "7e77ce724e53b8880415b5f3c40e60b5760a49ea110012931beeab6b93e7ec0d";
// The vendor file has a ten-byte trailer. MIDI returns only the 80 preset records.
const PRESET_SHA = "506e835c3315971a3914661cbbffa144a5221dedc06cea2562f35711d5d47444";
const checksum = (bytes) => ~bytes.reduce((sum, b) => sum + b, 0) & 0xff;

async function read(link, object, address, length) {
  const body = Uint8Array.of(0, 0x59, 0x23, 8, 0, 0, object, 0, 0, 0, 0, 0, 0, 0, 0);
  const fields = new DataView(body.buffer);
  fields.setUint32(7, address, true);
  fields.setUint16(11, length, true);
  body[14] = checksum(body.subarray(6, 14));
  link.drain();
  if (!link.send([0xf0, ...pack7(body), 0xf7])) throw new Error("BlackBox MIDI disconnected");

  const end = Date.now() + 1800;
  while (Date.now() < end) {
    const packet = await link.read(end - Date.now());
    if (!packet) break;
    if (packet[0] !== 0xf0 || packet.at(-1) !== 0xf7) continue;
    const bytes = Uint8Array.from(unpack7(packet.slice(1, -1)));
    if (bytes[0] !== 0 || bytes[1] !== 0x59 || bytes.length < 8 ||
        checksum(bytes.subarray(6, -1)) !== bytes.at(-1)) continue;
    if (bytes.length === 8 && bytes[2] === 0 && bytes[6] !== 0)
      throw new Error("V21 refused access to its sound bank. Run the factory sound restore below.");
    if (bytes.length !== length + 15 || bytes[2] !== 0x23 || bytes[6] !== object) continue;
    const reply = new DataView(bytes.buffer);
    if (reply.getUint32(7, true) === address && reply.getUint16(11, true) === length)
      return bytes.subarray(14, -1);
  }
  throw new Error("No matching reply from the BlackBox sound bank");
}

async function readBank(link, object, start, length, chunkSize, onProgress) {
  const bank = new Uint8Array(length);
  for (let offset = 0; offset < length; offset += chunkSize) {
    bank.set(await read(link, object, start + offset, Math.min(chunkSize, length - offset)), offset);
    onProgress(offset / length);
  }
  return bank;
}

export async function verifyFactorySounds(updater, onProgress = () => {}) {
  const dev = await updater.find((id) => id.text === BLACKBOX_V21);
  if (!dev) throw new Error("Connect a BlackBox running official V21");
  try {
    const models = await readBank(dev.link, 5, 0x70000000, 0x3C000, 512, (p) => onProgress(p * 0.95));
    const presets = await readBank(dev.link, 4, 0, 7360, 92, (p) => onProgress(0.95 + p * 0.05));
    if (await sha256hex(models) !== AMP_CAB_SHA || await sha256hex(presets) !== PRESET_SHA)
      throw new Error("The sound bank differs from the vendor’s AM4 factory bank");
    onProgress(1);
  } finally {
    dev.link.close();
  }
}
