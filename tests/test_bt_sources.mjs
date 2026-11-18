// The Bluetooth audio image's third-party sources (docs/BT-AUDIO-TEST.md): third_party/jieli-bt's MEMBERS lists exactly the files
// there, each LLVM bitcode as published, with JieLi's Apache-2.0 text; every Android SBC file carries its Apache-2.0 header; and
// firmware/bt/members.txt names only files that exist, each once. (The image itself is built by `make bt`; this needs no Docker.)
import assert from "node:assert/strict";
import { readdirSync, readFileSync, statSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
const repo = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const dir = join(repo, "third_party/jieli-bt");
const listed = readFileSync(join(dir, "MEMBERS"), "utf8").split("\n").filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t")[2]);
assert.ok(listed.length > 0 && listed.every(Boolean), "MEMBERS has archive, member and file on each line");
const onDisk = ["btctrler", "btstack", "crypto"].flatMap((d) => readdirSync(join(dir, d)).map((f) => `${d}/${f}`));
assert.deepEqual([...listed].sort(), onDisk.sort(), "MEMBERS lists exactly the objects in the directory");
for (const f of listed) {
  const head = readFileSync(join(dir, f)).subarray(0, 4);
  assert.deepEqual([...head], [0x42, 0x43, 0xc0, 0xde], `${f} is LLVM bitcode, as published`);
  assert.ok(statSync(join(dir, f)).size > 100, `${f} is not empty`);
}
assert.equal(readFileSync(join(dir, "LICENSE-JieLi-Apache-2.0"), "utf8"), readFileSync(join(repo, "firmware/sdk/LICENSE-JieLi-Apache-2.0"), "utf8"), "the licence text is JieLi's");
const sbc = join(repo, "third_party/sbc-android");
let n = 0;
for (const part of ["decoder/include", "decoder/srce", "encoder/include", "encoder/srce"])
  for (const f of readdirSync(join(sbc, part))) {
    assert.match(readFileSync(join(sbc, part, f), "utf8"), /Licensed under the Apache License, Version 2\.0/, `${part}/${f} carries the Apache-2.0 header`);
    n++;
  }
assert.match(readFileSync(join(sbc, "LICENSE"), "utf8"), /Apache License\s+Version 2\.0, January 2004/, "the SBC codec's licence text");
const used = readFileSync(join(repo, "firmware/bt/members.txt"), "utf8").split("\n").map((l) => l.replace(/#.*/, "").trim()).filter(Boolean);
assert.equal(new Set(used).size, used.length, "members.txt names each object once");
for (const f of used) assert.ok(existsSync(join(repo, f)) && f.endsWith(".bc"), `members.txt: ${f} exists`);
console.log(`bt sources: ${listed.length} classic objects, ${n} SBC files, ${used.length} objects linked: ok`);
