#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// The one host-side entry point of the OpenPocket build (macOS, Linux, Windows; Node 18+). The Makefile and the
// .bat files only call this. Every Docker command line lives here.
//   node tools/op.mjs doctor | build | test | package | clean | shell | image | serve [editor|build]
//   node tools/op.mjs render in.wav [out.wav] [preset]   |   node tools/op.mjs appimage [fm1-1.bin]
//   node tools/op.mjs sim [--port 8766] [--flash FILE] [--reset] | --update FLASH.bin   the hosted simulated pedal
// NATIVE=1 runs the commands on this machine (toolchain from firmware/setup.sh) instead of in the container.
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync, rmSync } from "node:fs";
import { dirname, isAbsolute, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const IMAGE = "openpocket-build";
const native = Boolean(process.env.NATIVE || process.env.OP_IN_CONTAINER);
const OUTS = "firmware/openpocket.bin docs/firmware/*.fwsc docs/firmware/manifest.json";

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
    const cc = spawnSync(process.env.CC || "cc", ["--version"], { stdio: "ignore" });
    console.log(cc.error || cc.status !== 0 ? "note no C compiler (cc): `sim`, the hosted simulated pedal, needs one (Xcode command line tools, build-essential)" : "ok   C compiler for `sim`");
    if (process.arch === "arm64") console.log("note ARM machine: the linux/amd64 toolchain runs under emulation (a `test` takes about 1 minute)");
    return bad;
  },
  image,
  build: () => inContainer(["sh", "firmware/build.sh"]),
  test: () => inContainer(["sh", "tests/run_tests.sh"]),
  package: () => inContainer(["sh", "-c", `sh firmware/build.sh && mkdir -p build && sha256sum ${OUTS} > build/SHA256SUMS && cat build/SHA256SUMS`]),
  clean() {
    for (const p of ["build", "firmware/build", "firmware/openpocket.bin", "docs/firmware/manifest.json", "engine/px_render"]) {
      rmSync(resolve(root, p), { recursive: true, force: true });
    }
    const dir = resolve(root, "docs/firmware");
    for (const f of existsSync(dir) ? readdirSync(dir) : []) if (f.endsWith(".fwsc")) rmSync(resolve(dir, f));
    return 0;
  },
  shell: () => inContainer(["bash"], { tty: true }),
  serve([arg]) {
    if (arg === "build" || !existsSync(resolve(root, "docs/firmware/manifest.json"))) {
      const built = commands.build();
      if (built !== 0) return built;
    }
    const py = process.platform === "win32" ? ["py", "-3"] : ["python3"];
    const r = spawnSync(py[0], [...py.slice(1), "tools/serve.py", arg === "editor" ? "editor.html" : "index.html"], { stdio: "inherit", cwd: root });
    if (r.error) fail("Python 3 is needed to serve the installer offline (python.org).");
    return r.status ?? 0;
  },
  sim: (args) => exec(process.execPath, ["tools/sim.mjs", ...args]),   // the host's own Node and C compiler: no container
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
process.exit(commands[name](rest));
