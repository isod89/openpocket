// tools/op.mjs, the build entry point, against a fake docker on PATH: the docker argv of each command, the friendly errors,
// clean without Docker, and the Makefile delegating to it. Run by tests/run_tests.sh.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { chmodSync, cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repo = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const tmp = realpathSync(mkdtempSync(join(tmpdir(), "op-test-")));
const fakebin = join(tmp, "bin");
const log = join(tmp, "docker.log");
mkdirSync(fakebin);
writeFileSync(join(fakebin, "docker"), `#!/bin/sh
first="$1"
{ printf '%s' "$1"; shift; for a in "$@"; do printf '\\t%s' "$a"; done; printf '\\n'; } >> "$FAKE_DOCKER_LOG"
[ "$first" = info ] && [ -n "$FAKE_DOCKER_DOWN" ] && exit 1
exit 0
`);
chmodSync(join(fakebin, "docker"), 0o755);

const root = join(tmp, "work");
mkdirSync(join(root, "tools"), { recursive: true });
cpSync(join(repo, "tools/op.mjs"), join(root, "tools/op.mjs"));

function op(args, { env = {}, cwd = root, path = fakebin, script = join(root, "tools/op.mjs") } = {}) {
  rmSync(log, { force: true });
  const e = { ...process.env, ...env, PATH: path, FAKE_DOCKER_LOG: log };
  for (const k of ["NATIVE", "OP_IN_CONTAINER"]) delete e[k];
  const r = spawnSync(process.execPath, [script, ...args], { cwd, env: e, encoding: "utf8" });
  const calls = existsSync(log) ? readFileSync(log, "utf8").trim().split("\n").filter(Boolean).map((l) => l.split("\t")) : [];
  return { status: r.status, err: r.stderr, calls };
}
const user = process.getuid ? ["--user", `${process.getuid()}:${process.getgid()}`] : [];
const base = (img) => ["run", "--rm", "--platform", "linux/amd64", ...user, "-e", "HOME=/tmp", "-v", `${root}:/src`, "-w", "/src", "openpocket-build", ...img];
const BUILD = ["build", "--platform", "linux/amd64", "-t", "openpocket-build", "."];
const touch = (p) => { mkdirSync(dirname(join(root, p)), { recursive: true }); writeFileSync(join(root, p), "x"); };

let r = op(["build"]);
assert.equal(r.status, 0);
assert.deepEqual(r.calls, [["info"], BUILD, base(["sh", "firmware/build.sh"])], "build");

r = op(["test"]);
assert.deepEqual(r.calls.at(-1), base(["sh", "tests/run_tests.sh"]), "test");

r = op(["package"]);
assert.equal(r.calls.filter((c) => c[0] === "run").length, 1, "package is one container run");
assert.deepEqual(r.calls.at(-1).slice(0, -1), base(["sh", "-c"]));
assert.match(r.calls.at(-1).at(-1), /^sh firmware\/build\.sh && .*sha256sum .*openpocket\.bin.*> build\/SHA256SUMS/);

r = op(["package-test"]);
assert.equal(r.calls.filter((c) => c[0] === "run").length, 1, "package-test is one container run");
assert.match(r.calls.at(-1).at(-1), /^OP_CHANNEL=midi-test sh firmware\/build\.sh && .*sha256sum firmware\/openpocket-midi-test\.bin docs\/firmware\/midi-test\/\*\.fwsc/);
assert.doesNotMatch(r.calls.at(-1).at(-1), /docs\/firmware\/manifest\.json/, "the test package never touches the default manifest");

r = op(["shell"]);
assert.deepEqual(r.calls.at(-1), ["run", "--rm", "-it", ...base(["bash"]).slice(2)], "shell has a tty and the user");

r = op(["doctor"]);
assert.equal(r.status, 0, "doctor with Docker up");
assert.deepEqual(r.calls, [["info"], ["image", "inspect", "openpocket-build"]], "doctor looks, builds nothing");
r = op(["doctor"], { env: { FAKE_DOCKER_DOWN: "1" } });
assert.notEqual(r.status, 0, "doctor fails with Docker down");
assert.deepEqual(r.calls, [["info"]]);

r = op(["image"]);
assert.deepEqual(r.calls, [["info"], BUILD], "image");

touch("guitar.wav");
r = op(["render", "guitar.wav"]);
assert.deepEqual(r.calls.at(-1), base(["bash", "engine/render.sh", "guitar.wav", "guitar_OpenPocket.wav", "1"]), "render defaults");
r = op(["render", "guitar.wav", "o.wav", "7"]);
assert.deepEqual(r.calls.at(-1), base(["bash", "engine/render.sh", "guitar.wav", "o.wav", "7"]), "render args");
r = op(["render", "../elsewhere.wav"]);
assert.notEqual(r.status, 0);
assert.match(r.err, /must be inside/);
r = op(["render", "missing.wav"]);
assert.match(r.err, /missing\.wav not found/);
assert.deepEqual(r.calls, [], "no docker call for a missing input");

r = op(["appimage"]);
assert.notEqual(r.status, 0);
assert.match(r.err, /fm1-1\.bin not found/);
assert.deepEqual(r.calls, []);
touch("fm1-1.bin");
r = op(["appimage"]);
assert.notEqual(r.status, 0);
assert.match(r.err, /firmware\/openpocket\.bin not found.*build/);
assert.deepEqual(r.calls, []);
touch("firmware/openpocket.bin");
r = op(["appimage"]);
assert.equal(r.status, 0);
assert.deepEqual(r.calls.at(-1), base(["python3", "tools/pocketx_appimage.py", "fm1-1.bin", "firmware/openpocket.bin", "fm1-300.bin"]), "appimage");

r = op(["build"], { env: { FAKE_DOCKER_DOWN: "1" } });
assert.notEqual(r.status, 0);
assert.match(r.err, /Docker is not installed or not running/);
assert.deepEqual(r.calls, [["info"]], "nothing runs once docker is down");

r = op(["build"], { path: join(tmp, "empty") });
assert.notEqual(r.status, 0);
assert.match(r.err, /Docker is not installed or not running/, "no docker at all");

for (const p of ["firmware/openpocket-midi-test.bin", "docs/firmware/midi-test/PocketX_937.fwsc", "build/x", "firmware/build/y", "docs/firmware/PocketX_900.fwsc", "docs/firmware/manifest.json", "engine/px_render"]) touch(p);
touch("docs/firmware/README.md");
touch("engine/px_dsp.c");
r = op(["clean"], { path: join(tmp, "empty") });
assert.equal(r.status, 0, "clean needs no docker");
for (const p of ["build", "firmware/build", "firmware/openpocket.bin", "firmware/openpocket-midi-test.bin", "docs/firmware/midi-test", "docs/firmware/PocketX_900.fwsc", "docs/firmware/manifest.json", "engine/px_render"]) {
  assert.ok(!existsSync(join(root, p)), `${p} removed`);
}
for (const p of ["docs/firmware/README.md", "engine/px_dsp.c", "guitar.wav"]) assert.ok(existsSync(join(root, p)), `${p} kept`);

const make = spawnSync("make", ["--version"], { encoding: "utf8" });
if (make.status === 0) {
  rmSync(log, { force: true });
  const env = { ...process.env, PATH: `${fakebin}:${process.env.PATH}`, FAKE_DOCKER_LOG: log };
  for (const k of ["NATIVE", "OP_IN_CONTAINER"]) delete env[k];
  const m = spawnSync("make", ["-C", repo, "shell"], { env, encoding: "utf8" });
  assert.equal(m.status, 0, m.stderr);
  const calls = readFileSync(log, "utf8").trim().split("\n").map((l) => l.split("\t"));
  assert.equal(calls.at(-1)[2], "-it", "make shell goes through op.mjs");
  assert.ok(calls.at(-1).includes("openpocket-build") && calls.at(-1).at(-1) === "bash");
} else {
  console.log("(make not installed: Makefile delegation skipped)");
}

rmSync(tmp, { recursive: true, force: true });
console.log("op.mjs: ok");
