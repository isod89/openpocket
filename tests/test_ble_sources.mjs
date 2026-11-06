// The Bluetooth test image's vendor objects (third_party/jieli-ble, docs/BLE-TEST.md): MEMBERS lists exactly the files
// that are there, each is LLVM bitcode, and the licence text is JieLi's Apache-2.0 file. (The image itself is built by
// `make ble`; this needs no Docker.)
import assert from "node:assert/strict";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repo = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const dir = join(repo, "third_party/jieli-ble");
const listed = readFileSync(join(dir, "MEMBERS"), "utf8").split("\n").filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t")[2]);
assert.ok(listed.length > 0 && listed.every(Boolean), "MEMBERS has archive, member and file on each line");
const onDisk = ["btctrler", "btstack", "ccm", "cpu"].flatMap((d) => readdirSync(join(dir, d)).map((f) => `${d}/${f}`));
assert.deepEqual([...listed].sort(), onDisk.sort(), "MEMBERS lists exactly the objects in the directory");
for (const f of listed) {
  const head = readFileSync(join(dir, f)).subarray(0, 4);
  assert.deepEqual([...head], [0x42, 0x43, 0xc0, 0xde], `${f} is LLVM bitcode, as published`);
  assert.ok(statSync(join(dir, f)).size > 100, `${f} is not empty`);
}
assert.equal(readFileSync(join(dir, "LICENSE-JieLi-Apache-2.0"), "utf8"), readFileSync(join(repo, "firmware/sdk/LICENSE-JieLi-Apache-2.0"), "utf8"), "the licence text is JieLi's");
console.log(`ble sources: ${listed.length} objects ok`);
