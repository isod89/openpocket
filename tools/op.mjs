#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// The one host-side entry point of the OpenPocket build (macOS, Linux, Windows; Node 18+). The Makefile and the
// .bat files only call this. Every Docker command line lives here.
//   node tools/op.mjs doctor | build | ble | test [--built] | package | clean | shell | image | serve [editor|build|ble]
//   node tools/op.mjs all        every product of this tree and the tests: OpenPocket (package, with the release pin), each
//                                app of firmware/apps, then the host tests with the packages just built (test --built)
//   node tools/op.mjs app NAME   another app on the base firmware (firmware/apps/NAME, docs/BASE-FIRMWARE.md) -> build/apps/NAME/
//   node tools/op.mjs serve app NAME   the installer with that app offered beside OpenPocket (index.html?app=NAME)
//   node tools/op.mjs flash --dry-run | --confirm-flash [--rescue] [--package FILE] [--app NAME]   (writes to a connected pedal: docs/BUILDING.md)
//   node tools/op.mjs log [--follow] [--json] [--wait SEC] [--scan SEC]   (reads a connected pedal's state: docs/BLE-TEST.md)
//   node tools/op.mjs dsp [--presets A,B] [--seconds SEC] [--json]   (the DSP's load, late blocks and bypass per preset, over USB)
//   node tools/op.mjs render in.wav [out.wav] [preset]   |   node tools/op.mjs appimage [fm1-1.bin]
//   node tools/op.mjs rescue [--write] [options]   (restore a looping BlackBox from its UBOOT state; read-only without --write; docs/RESCUE.md)
//   node tools/op.mjs rescue-check   (the UBOOT rescue tool against a simulated pedal: Python 3 only, no Docker)
//   node tools/op.mjs sim [--port 8766] [--flash FILE] [--reset] | --update FLASH.bin | --app NAME   the hosted simulated pedal
//   node tools/op.mjs wasm   the engine as WebAssembly for the simulated pedal's sound (tools/sim/px_engine.wasm)
// NATIVE=1 runs the commands on this machine (toolchain from firmware/setup.sh) instead of in the container.
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync, rmSync } from "node:fs";
import { dirname, isAbsolute, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const IMAGE = "openpocket-build";
const native = Boolean(process.env.NATIVE || process.env.OP_IN_CONTAINER);
const python = process.platform === "win32" ? ["py", "-3"] : ["python3"];
const OUTS = "firmware/openpocket.bin docs/firmware/*.fwsc docs/firmware/manifest.json";
const TEST = "firmware/openpocket-midi-test.bin docs/firmware/midi-test/*.fwsc docs/firmware/midi-test/manifest.json";

function fail(message) {
  console.error(`\nERROR: ${message}`);
  process.exit(1);
}

function exec(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { stdio: "inherit", cwd: root, ...opts });
  if (r.error) fail(`could not run ${cmd}: ${r.error.message}`);
  return r.status ?? 1;
}

function needDocker() {
  const r = spawnSync("docker", ["info"], { stdio: "ignore" });
  if (r.error || r.status !== 0) {
    fail("Docker is not installed or not running. Install and start Docker Desktop (or Docker on Linux, or `colima start`). See docs/BUILDING.md.");
  }
}

function image() {
  needDocker();
  return exec("docker", ["build", "--platform", "linux/amd64", "-t", IMAGE, "."]);
}

function inContainer(args, { tty = false } = {}) {
  if (native) return exec(args[0], args.slice(1));
  const built = image();
  if (built !== 0) return built;
  const user = process.getuid ? ["--user", `${process.getuid()}:${process.getgid()}`] : [];
  return exec("docker", [
    "run", "--rm", ...(tty ? ["-it"] : []), "--platform", "linux/amd64", ...user,
    "-e", "HOME=/tmp", "-v", `${root}:/src`, "-w", "/src", IMAGE, ...args,
  ]);
}

function inside(path) {
  const rel = relative(root, resolve(process.cwd(), path));
  if (rel === "" || rel.startsWith("..") || isAbsolute(rel)) fail(`${path} must be inside the OpenPocket folder (${root}).`);
  return rel.split(sep).join("/");
}

// the apps of this tree other than OpenPocket: the folders of firmware/apps that hold an app.sh
const apps = () => { const d = resolve(root, "firmware/apps"); return existsSync(d) ? readdirSync(d).filter((n) => existsSync(resolve(d, n, "app.sh"))).sort() : []; };

const commands = {
  doctor() {                                   // prerequisites, one line each; exit 1 when the build cannot work
    let bad = 0;
    const line = (ok, text) => { console.log(`${ok ? "ok  " : "FAIL"} ${text}`); if (!ok) bad = 1; };
    line(Number(process.versions.node.split(".")[0]) >= 18, `Node ${process.versions.node} (18 or newer)`);
    const info = spawnSync("docker", ["info"], { stdio: "ignore" });
    const up = !info.error && info.status === 0;
    line(up, up ? "Docker is running" : "Docker is not installed or not running (Docker Desktop, Docker, or `colima start`)");
    if (up) {
      const img = spawnSync("docker", ["image", "inspect", IMAGE], { stdio: "ignore" });
      console.log(img.status === 0 ? `ok   image ${IMAGE} is built` : `note image ${IMAGE} is not built yet: the first build or \`image\` makes it (about 1 minute)`);
    }
    const rescue = spawnSync(python[0], [...python.slice(1), "tools/uboot_rescue.py", "--check"], { cwd: root, encoding: "utf8" });
    if (rescue.error) console.log("note rescue (looping pedal): Python 3 not found, `make rescue` is unavailable");
    else for (const l of rescue.stdout.split("\n").filter(Boolean)) console.log(l.startsWith("FAIL") ? `note rescue: ${l.slice(5)}` : `${l.slice(0, 4)} rescue: ${l.slice(5)}`);
    const cc = spawnSync(process.env.CC || "cc", ["--version"], { stdio: "ignore" });
    console.log(cc.error || cc.status !== 0 ? "note no C compiler (cc): `sim`, the hosted simulated pedal, needs one (Xcode command line tools, build-essential)" : "ok   C compiler for `sim`");
    if (process.arch === "arm64") console.log("note ARM machine: the linux/amd64 toolchain runs under emulation (a `test` takes about 1 minute)");
    return bad;
  },
  image,
  build: () => inContainer(["sh", "firmware/build.sh"]),
  ble: () => inContainer(["sh", "-c", "OP_BLE=1 sh firmware/build.sh && sha256sum firmware/openpocket-ble.bin build/ble/*.fwsc"]),   // the Bluetooth MIDI test image (docs/BLE-TEST.md)
  app([name]) {                                // an app on the base firmware other than OpenPocket: its own outputs, build/apps/<name>/
    if (!/^[a-z0-9-]+$/.test(name || "")) fail("usage: app NAME   (a folder of firmware/apps, such as starter). See docs/ALT-FIRMWARE.md.");
    if (!existsSync(resolve(root, "firmware/apps", name, "app.sh"))) fail(`firmware/apps/${name}/app.sh not found.`);
    const out = `build/apps/${name}`;
    return inContainer(["sh", "-c", `OP_APP=${name} sh firmware/build.sh && sha256sum ${out}/${name}.bin ${out}/*.fwsc ${out}/manifest.json > ${out}/SHA256SUMS && cat ${out}/SHA256SUMS`]);
  },
  // --built: the tests also take the packages the build made (OpenPocket's, each app's), which must be there
  test: (args = []) => inContainer(args.includes("--built") ? ["sh", "-c", "OP_BUILT=1 sh tests/run_tests.sh"] : ["sh", "tests/run_tests.sh"]),
  all() {                                      // both kinds of product from one tree, then the tests on what was built
    let rc = commands.package();
    for (const name of apps()) rc = rc || commands.app([name]);
    return rc || commands.test(["--built"]);
  },
  // (the release pin, tests/release_pin.sh: OpenPocket's outputs must be the pinned release's bytes, or this fails)
  package: () => inContainer(["sh", "-c", `sh firmware/build.sh && mkdir -p build && sha256sum ${OUTS} > build/SHA256SUMS && sh tests/release_pin.sh`]),
  "package-test": () => inContainer(["sh", "-c", `OP_CHANNEL=midi-test sh firmware/build.sh && mkdir -p build && sha256sum ${TEST} > build/SHA256SUMS-midi-test && cat build/SHA256SUMS-midi-test`]),
  clean() {
    for (const p of ["build", "firmware/build", "firmware/build-ble", "firmware/openpocket.bin", "firmware/openpocket-ble.bin", "firmware/openpocket-midi-test.bin", "docs/firmware/midi-test", "docs/firmware/manifest.json", "engine/px_render"]) {
      rmSync(resolve(root, p), { recursive: true, force: true });
    }
    const dir = resolve(root, "docs/firmware");
    for (const f of existsSync(dir) ? readdirSync(dir) : []) if (f.endsWith(".fwsc")) rmSync(resolve(dir, f));
    return 0;
  },
  shell: () => inContainer(["bash"], { tty: true }),
  async flash(args) {                          // runs on this machine: the pedal is a USB device, not in the container
    return (await import("./flash.mjs")).main(args);
  },
  async log(args) {                            // the same: reads the connected pedal, writes nothing
    return (await import("./log.mjs")).main(args);
  },
  async dsp(args) {                            // the same: the DSP's load, late blocks and bypass per preset (selects, saves nothing)
    return (await import("./dsp.mjs")).main(args);
  },
  serve([arg, name]) {
    const ble = arg === "ble";                 // the Bluetooth test packages (build/ble) instead of docs/firmware
    const alt = arg === "app";                 // an alternative firmware offered beside OpenPocket (its packages: build/apps/NAME)
    if (alt && (!/^[a-z0-9-]+$/.test(name || "") || !existsSync(resolve(root, "firmware/apps", name, "app.sh")))) fail("usage: serve app NAME   (a folder of firmware/apps, such as starter). See docs/ALT-FIRMWARE.md.");
    if (ble ? !existsSync(resolve(root, "build/ble/manifest.json")) : arg === "build" || !existsSync(resolve(root, "docs/firmware/manifest.json"))) {
      const built = ble ? commands.ble() : commands.build();
      if (built !== 0) return built;
    }
    if (alt && !existsSync(resolve(root, "build/apps", name, "manifest.json"))) {
      const built = commands.app([name]);
      if (built !== 0) return built;
    }
    const py = python;
    const page = arg === "editor" ? "editor.html" : alt ? `index.html?app=${name}` : "index.html";
    const r = spawnSync(py[0], [...py.slice(1), "tools/serve.py", page, ...(ble ? ["build/ble"] : [])], { stdio: "inherit", cwd: root });
    if (r.error) fail("Python 3 is needed to serve the installer offline (python.org).");
    return r.status ?? 0;
  },
  rescue(args) {                               // read-only unless --write; macOS needs root to take the pedal from its own driver
    if (process.platform === "win32") fail("the rescue runs on macOS or Linux (it needs libusb and root). See docs/RESCUE.md.");
    const root_ = typeof process.getuid === "function" && process.getuid() === 0;
    const cmd = root_ || args.includes("--check") ? python : ["sudo", ...python];
    if (cmd[0] === "sudo") console.log(`rescue: ${args.includes("--write") ? "WRITE" : "read-only"} run; sudo asks for your password, then the tool waits for the pedal in its UBOOT state`);
    return exec(cmd[0], [...cmd.slice(1), "tools/uboot_rescue.py", ...args]);
  },
  "rescue-check"() {
    const r = spawnSync(python[0], [...python.slice(1), "tests/test_uboot_rescue.py"], { stdio: "inherit", cwd: root });
    if (r.error) fail("Python 3 is needed for the rescue tool tests (python.org).");
    return r.status ?? 1;
  },
  sim: (args) => exec(process.execPath, ["tools/sim.mjs", ...args]),   // the host's own Node and C compiler: no container
  wasm: () => inContainer(["sh", "engine/wasm.sh"]),
  render([input, output, preset]) {
    if (!input) fail("usage: render in.wav [out.wav] [preset 1..52]");
    const rel = inside(input);
    if (!existsSync(resolve(root, rel))) fail(`${input} not found.`);
    const out = output ? inside(output) : rel.replace(/\.[^./]*$/, "") + "_OpenPocket.wav";
    return inContainer(["bash", "engine/render.sh", rel, out, preset || "1"]);
  },
  appimage([dump]) {
    const rel = inside(dump || "fm1-1.bin");
    if (!existsSync(resolve(root, rel))) fail(`${rel} not found. Put your factory backup (the fm1-1.bin dump) there, or pass its path.`);
    if (!existsSync(resolve(root, "firmware/openpocket.bin"))) fail("firmware/openpocket.bin not found. Build first: node tools/op.mjs build (make build, build.bat).");
    return inContainer(["python3", "tools/pocketx_appimage.py", rel, "firmware/openpocket.bin", "fm1-300.bin"]);
  },
};

const [name, ...rest] = process.argv.slice(2);
if (!Object.hasOwn(commands, name)) fail(`usage: node tools/op.mjs ${Object.keys(commands).join(" | ")}`);
process.exit(await commands[name](rest));
