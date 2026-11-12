// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — `node tools/op.mjs log` (tools/log.mjs): the DIAG request against the pedal's real editor and status-text code
// (tests/edsim.c built with -DOP_BLE_TEST=1), the same against firmware without it, and the states where less can be read.
//   cc -O1 -DOP_BLE_TEST=1 -o build/edsim_diag tests/edsim.c engine/px_dsp.c firmware/px_math.c -lm && node tests/test_log.mjs
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { BOOT_STAGES, RESET_CAUSES, a2dpWords, c1Words, diagFields, fifoWords, judge, main, parseBle, parseBoot, parseLast, show, vmWords, wWords } from "../tools/log.mjs";
import { simAccess } from "./simpedal.mjs";

const ROOT = fileURLToPath(new URL("..", import.meta.url));
let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(72)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// a simulated pedal on a fake Web MIDI access; ble() steers its Bluetooth state
function sim(bin) {
  const child = spawn(join(ROOT, "build", bin), [], { stdio: ["pipe", "pipe", "inherit"] });
  const access = { inputs: new Map(), outputs: new Map() };
  let input = null, buf = "", readyFn;
  const ready = new Promise((r) => { readyFn = r; });
  const states = [];
  child.stdout.on("data", (d) => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const ln = buf.slice(0, i); buf = buf.slice(i + 1);
      if (ln.startsWith("#port ")) {
        const name = ln.slice(6);
        input = { name, state: "connected", onmidimessage: null, open: async () => {} };
        access.inputs.set(name, input);
        access.outputs.set(name, { name, state: "connected", open: async () => {}, send: (b) => child.stdin.write(Buffer.from(b).toString("hex") + "\n") });
        readyFn();
      } else if (ln.startsWith("#state ")) states.push(ln);
      else if (ln && input?.onmidimessage) input.onmidimessage({ data: Uint8Array.from(Buffer.from(ln, "hex")) });
    }
  });
  const done = new Promise((r) => child.on("exit", r));
  return { access, ready, states, ble: (l) => child.stdin.write(`#ble ${l}\n`), dump: () => child.stdin.write("#dump\n"), end: () => { child.stdin.end(); return done; } };
}

async function run(access, argv, opts = {}) {
  const out = [];
  const code = await main(argv, { access, say: (l) => out.push(l), ...opts });
  return { code, text: out.join("\n"), last: argv.includes("--json") ? JSON.parse(out[out.length - 1]) : null };
}

// ---- the text formats ----
const adv = "ADV i12 e5 r0 t340 s0 CON 2001 IEN 101 IPND 0 STAT 80 IC45 3 IC29 3 BEN 1";
let b = parseBle(adv);
ok(b.state === "advertising" && b.i === 12 && b.t === 340 && b.CON === 0x2001 && b.IC45 === 3 && b.BEN === 1 && !b.vm, "advertising line, every field");
b = parseBle("ADV i12 e5 r0 t340 s0 V 7 A3 12AB CON 2001 IEN 101 IPND 0 STAT 80 IC45 3 IC29 3 BEN 1");
ok(b.vm?.found === 7 && b.vm.area === 0xA3 && b.vm.trim === 0x12AB && b.CON === 0x2001, "advertising line with the stored radio records");
b = parseBle("FAILED 5 3  step timeout stage radio-init wait 01C08000");
ok(b.state === "failed" && b.step === 5 && b.code === 3 && b.stage === "radio-init" && b.wait === "01C08000", "failure line");
ok(parseBle("FAILED 2 -7  step btctrler_task_init stage init").code === -7, "failure line, negative code");
ok(parseBle("STARTING 4").stage === 4 && parseBle("OFF").state === "off", "starting and off");
ok(parseBle("ADVERTISING?").state === "unknown", "a line this tool does not know stays unknown");
{ // the Bluetooth audio image (docs/BT-AUDIO-TEST.md): visible, Bluetooth off by REV at power-on, an earlier stall's record
  const boot = { stage: 8, clockMs: 9000, safe: false }, at = (ble, link = "TASKS") => [{ level: "full", time: "2026-11-10T20:00:00Z", boot, ble: parseBle(ble), link }];
  let v = judge(at("BT VIS IRQ 40:1234 41:567 EV 4 RDY SCAN 1"));
  ok(v.result === "running" && /visible, no phone linked/.test(v.reason), "Bluetooth audio image, visible: running");
  v = judge(at("BT OFF: REV HELD AT POWER-ON (THIS BOOT WITHOUT BLUETOOTH)"));
  ok(v.result === "running" && /REV held/.test(v.reason) && !/visible/.test(v.reason), `REV held at power-on: Bluetooth off, said so: ${v.reason}`);
  v = judge(at("STALL btctrler S8 BB 00000107 0000FE02 00000000 0000FFFF 00000001"));
  ok(v.result === "running" && /earlier stall/.test(v.reason), "an earlier stall's record while Bluetooth is off is named, not unknown");
  // test build 12's a2dp line (firmware/bt/bt_a2dp.c bt_a2dp_line: "A2" and 23 words in hex), named by the tool
  const w = ["00000123", "00000000", "00300041", "00000122", "00000001", "10004100", "0001ABCD", "10004101", "0000ABCD", "00000000",
    "10004101", "0000ABCE", "10004100", "0000ABCF", "10004100", "FFFFFFEA", "00000122", "00000003", "0003579B", "10004100",
    "01C5F0FA", "00010002", "00000001"];
  const named = a2dpWords(`A2 ${w.join(" ")}`);
  ok(named && /^n 291, kept 0, hc 0030:0041, B 290, Bn 1, Bst 10004100/.test(named) && /RC FFFFFFEA, H 290, W 3, W0 109517ms\/1/.test(named)
     && /DROP 2\/1, DUP 1$/.test(named), "the a2dp line's 23 words, named");
  ok(a2dpWords("A2 0000 0001") === null && a2dpWords(`A2 ${w.slice(1).join(" ")} XYZ12345`) === null, "an a2dp line cut short or damaged is not named");
  // test build 13's line 6 (firmware/bt/bt_a2dp.c bt_fifo_line: "FIFO" and 4 words in hex), named by the tool
  ok(fifoWords("FIFO 000004D2 00010003 00000005 00000002") === "taken 1234, dropped 3 full / 1 too big, given back 5, heals 2",
     "the FIFO line's 4 words, named");
  ok(fifoWords("FIFO 000004D2 0001") === null && fifoWords("FIFO 000004D2 00010003 00000005 0000000G") === null
     && fifoWords("FIFO 000004D2 00010003 00000005") === null && fifoWords(`A2 ${w.slice(0, 4).join(" ")}`) === null,
     "a FIFO line cut short or damaged is not named");
  // test build 13's W in the c1 line (bt_c1_line): the reverb loop's cuts, then n, k, the first cut's time and S word
  const c1 = "C1 1 G 0@0 K 2232 KEY 0 S 00000000 F 0/0";
  const fault = " 00200000 00000000 01C6A5A0 01C6A5B4 00000000 000007A8 00000012 00000011 01C5F000 00000000 00000000 00000001";
  let ww = wWords(`${c1} W 0`);
  ok(ww?.cuts === 0 && /no cut .* counted in this boot/.test(ww.text) && /uncounted/.test(ww.text), "W 0: no cut counted, and what it cannot rule out");
  ww = wWords(`C1 0 G 4@1960 K 2232 KEY 0 S 3C400123 F 1/0 W 0${fault}`);
  ok(ww?.cuts === 0 && ww.n === undefined, "W 0 before a fault's words: the fault's words are not taken for W's");
  // (the main loop's latch: a cut during the radio's start-up replay is seen only after it, at stage 7 RF_DONE or a step after it)
  ww = wWords(`${c1} W 1 00000010 00000040 000008A2 71400123`);
  ok(ww?.n === 16 && ww.k === 64 && ww.t === 2210 && /\(B\)/.test(ww.text) && /one-off wrong read of n/.test(ww.text)
     && /first saw it at 2210 ms \(the pedal clock wraps every 179 s\): stage 7, state 1, DSP held 0, IRQ 40 291/.test(ww.text),
     `W, n 16 and k 64: a one-off wrong read of n (B): ${ww?.text}`);
  ww = wWords(`C1 0 G 4@1960 K 2232 KEY 3 S 3C400123 F 1/0 W 2 0000BEEF 00000040 000007A8 30400123${fault}`);
  ok(ww?.cuts === 2 && ww.n === 0xBEEF && /overwritten \(A\)/.test(ww.text), "W, n overwritten: (A), with a fault's words after it");
  ww = wWords(`${c1} W 1 00000010 00200041 000007A8 30400123`);
  ok(ww?.k === 0x200041 && /k jumped forward/.test(ww.text) && /misread signature/.test(ww.text), "W, k jumped: the misread signature named");
  ww = wWords(`${c1} W 1 00000010 00000005 000008A2 71400123`);
  ok(ww?.k === 5 && /cannot pass the cut's check/.test(ww.text) && !/jumped/.test(ww.text), "W, k below 64: the cut's own check misfired, not a jump");
  ww = wWords(`${c1} W 1 00000040 00000040 00000000 00000000`);
  ok(/chain's call/.test(ww?.text) && /not yet latched/.test(ww.text), "W, n 64: the chain's call; the first cut not latched yet");
  // the image's nine lines through the tool's own classification (diagFields) and report (show): build 13, 12, 9-11, and the
  // safe-mode boot after a crash (the DSP off: W 0 there says nothing)
  const info = { identity: "OpenPocket", version: "v40", port: "OpenPocket" };
  const report = (l6, c1l, boot = "UP 30000 SAFE 0 STAGE 8 RST 1 GUARD 0", last = "LAST OK", ble = "BT VIS IRQ 40:1234 41:567 EV 4 RDY SCAN 1") => {
    const out = [], r = { time: "", level: "full", info, ...diagFields([boot, last, ble, "LINK", "EV", "HCI", l6, "ISR", c1l]) };
    show(r, null, (x) => out.push(x));
    return { r, text: out.join("\n") };
  };
  let rp = report("FIFO 000004D2 00010003 00000005 00000002", `${c1} W 1 00000010 00000040 000008A2 71400123`);
  ok(rp.r.fifo && !rp.r.a2dp && !rp.r.hci0 && /^fifo {7}FIFO 000004D2/m.test(rp.text) && /^ {11}taken 1234, dropped 3 full \/ 1 too big/m.test(rp.text)
     && /^ {11}W 1 cut: n 16 \(0x10\), k 64 \(0x40\)/m.test(rp.text), "build 13's lines: the FIFO line and W named in the report");
  rp = report(`A2 ${w.join(" ")}`, c1);
  ok(rp.r.a2dp && !rp.r.fifo && /^ {11}n 291, kept 0/m.test(rp.text) && !/^ {11}W /m.test(rp.text), "build 12's lines: the a2dp line named, no W");
  rp = report("00 S2 0101", undefined);
  ok(rp.r.hci0 === "00 S2 0101" && !rp.r.fifo && /^hci0 {7}00 S2 0101/m.test(rp.text), "builds 9 to 11: line 6 is hci0");
  rp = report("FIFO 00000000 00000000 00000000 00000000", `${c1} W 0`, "UP 21000 SAFE 1 STAGE 7 RST 44 GUARD 1",
    "LAST FAILED STAGE 7 BLOCKS 227 LOAD 9 PEAK 16 NAN 0 CPU1 1 LOOPS 713 CRASH VEC 1 PC 1C67A04 RETS 1C67A6E DBG 2000 EMU 0 UP 1971", "BT OFF");
  ok(/^ {11}W not measured: safe mode/m.test(rp.text) && !/no cut/.test(rp.text) && /firmware\/build-bt\/symbols\.txt/.test(rp.text),
     "the safe-mode boot after a crash: W not measured; the Bluetooth audio image's symbols named");
  ok(wWords(`${c1} W 1 00000010 00000040`) === null && wWords(`${c1} W 1`) === null && wWords(`${c1} W 1 0000001`) === null
     && wWords(c1) === null && wWords(undefined) === null, "a W cut short, damaged or missing (build 12's c1 line) is not named");

  // test build 14's c1 line (bt_c1_line): KEY, P, V, D, SE, E, CN before S; W with the latest cut's loop; the fault's 9 words and X
  const c14 = "C1 1 G 0@0 K 1976 KEY 0 P 03/0B V 1452 D 3452 SE 1@1453 E 0/0 CN 0 S 00000000 F 0/0";
  let cw = c1Words(`${c14} W 0`);
  ok(cw?.key === 0 && cw.vdc14 === 3 && cw.sysvdd === 11 && !cw.strong && cw.v === 1452 && cw.d === 3452 && cw.se === 1 && cw.seMs === 1453
     && cw.echo === 0 && cw.late === 0 && cw.cn === 0 && cw.faults === 0 && cw.record === null
     && /KEY 0: none: two cores/.test(cw.text) && /VDC14 3 \(1\.40 V\), SYSVDD 11 \(1\.26 V\), the boot loader's/.test(cw.text)
     && /D 3452 ms: the DSP on \(2000 ms after VISIBLE\)/.test(cw.text) && /SE 1 scan enable, the last at 1453 ms/.test(cw.text)
     && /E 0\/0: CPU0's exceptions resumed/.test(cw.text) && /canary after the Bluetooth heap intact/.test(cw.text), `build 14's fields named: ${cw?.text}`);
  cw = c1Words("C1 1 G 0@0 K 1976 KEY 1 P 0F/1F V 1560 D 1503 SE 3@9120 E 1/1 01C67A04 CN 2 DEADBEEF S 42400006 F 0/0 W 0");
  ok(cw?.key === 1 && cw.vdc14 === 7 && cw.sysvdd === 15 && cw.strong === 1 && /CAB: one core/.test(cw.text)
     && /VDC14 7 \(1\.60 V\), SYSVDD 15 \(1\.38 V\) with its strong drive, the stock firmware's/.test(cw.text) && /57 ms before VISIBLE/.test(cw.text)
     && cw.echo === 1 && cw.late === 1 && cw.epc === 0x01C67A04 && /1 echo of the second core's fault, 1 late flag of holding it \(the latest with CPU0 at PC 01C67A04\)/.test(cw.text)
     && cw.cn === 2 && cw.cnFirst === 0xDEADBEEF && /changed \(2 words; the first read DEADBEEF\)/.test(cw.text), `CAB, E and CN named: ${cw?.text}`);
  const rec9 = " 00040000 00000001 01C6F6DE 01C6C08E 00000001 000007AC 00000129 00000128 01C5FD46";
  cw = c1Words(`C1 0 G 4@1967 K 1976 KEY 2 P 03/0B V 1452 D 1503 SE 1@1453 E 1/0 01C67A04 CN 0 S 42400006 F 1/1 W 0${rec9} X 0 0 DE 7E 40 140 3FF 3FF 3FF 3FF`);
  ok(cw?.key === 2 && cw.faults === 1 && cw.cpu === 1 && cw.record.DBG === 0x40000 && cw.record.PC === 0x01C6F6DE && cw.record.ms === 1964
     && cw.record.fn === 0x01C5FD46 && cw.x?.length === 10 && cw.x[2] === 0xDE && cw.x[9] === 0x3FF
     && /DLY: no central role/.test(cw.text) && /faulted 1 time, the last taken by CPU1 at 1964 ms, PC 01C6F6DE, DBG 00040000; debug unit 0 0 DE 7E/.test(cw.text),
     `a fault after W 0: its 9 words and X, not taken for W's: ${cw?.text}`);
  cw = c1Words(`C1 0 G 4@1967 K 1976 KEY 2 P 03/0B V 1452 D 1503 SE 1@1453 E 0/0 CN 0 S 42400006 F 1/1 W 1/2 00000010 00000041 000007AE 42400006${rec9.slice(0, 58)}`);
  ok(cw?.record.DBG === 0x40000 && cw.record.ms === 1964 && Object.keys(cw.record).length === 6 && cw.x === null && /3 of its words cut off/.test(cw.text),
     "a fault after a cut's words, the line cut at 223: the words left named, the cut said");
  // (bt_c1_line's own output, built on the host from its text with an echo resumed, a fault and a cut in the delay's loop: 253
  // characters built, cut at 223 after X's first word)
  const cut223 = "C1 0 G 4@1967 K 1976 KEY 2 P 03/0B V 1452 D 1503 SE 1@1453 E 1/0 01C67A04 CN 0 S 42400006 F 1/1 W 1/2 00000010 00000041 000007AE 42400006 00040000 00000001 01C6F6DE 01C6C08E 00000001 000007AC 00000129 00000128 01C5FD46 X 0 ";
  cw = c1Words(cut223); ww = wWords(cut223);
  ok(cut223.length === 223 && Object.keys(cw?.record ?? {}).length === 9 && cw.record.fn === 0x01C5FD46 && cw.x.join() === "0"
     && cw.epc === 0x01C67A04 && /debug unit 0 \(the rest cut off at 223 characters\)/.test(cw.text) && ww?.id === 2 && ww.k === 0x41,
     "the line cut at 223 inside X: the record whole, X as far as it got, W's loop named");
  const wide = `C1 0 G 4@178956 K 1976 KEY 2 P 03/0B V 178956 D 101503 SE 1@178956 E 1/0 01C67A04 CN 1 DEADBEEF S 42400006 F 1/1 W 0${rec9}`
    + " X 0 0 DE 7E 40 140 3FF 3FF 3FF 3FF";             // (232 characters: bt_line_out keeps 223, inside X's last word)
  cw = c1Words(wide.slice(0, 223));
  ok(wide.length === 232 && cw?.x.join() === "0,0,222,126,64,320,1023" && cw.cn === 1 && cw.cnFirst === 0xDEADBEEF
     && /debug unit 0 0 DE 7E 40 140 3FF \(the rest cut off at 223 characters\)/.test(cw.text),
     "a line cut inside X's last word: that word dropped, the rest named");
  cw = c1Words(wide.slice(0, 180));
  ok(Object.keys(cw?.record ?? {}).length === 7 && cw.record["job handed"] === 0x129 && cw.x === null && /2 of its words cut off/.test(cw.text),
     "a record cut short: its whole words named, the number missing said");
  ok(c1Words(`${c1} W 0`) === null && c1Words(undefined) === null, "build 12 and 13's c1 lines (no P): no build-14 fields");
  ww = wWords(`${c14} W 2/2 00000010 00000041 000008A2 71400123`);
  ok(ww?.id === 2 && ww.cuts === 2 && /the latest in the delay's loop/.test(ww.text) && /k jumped forward/.test(ww.text), `W, the delay's loop: ${ww?.text}`);
  ww = wWords(`${c14} W 1/3 0000012C 00000001 000008A2 71400123`);
  ok(ww?.id === 3 && /a wet lane's frame count was read as 300 \(128 at most\) for lane 1/.test(ww.text), "W, a lane's count held to 128");
  ww = wWords(`${c14} W 1/4 00000007 00000000 000008A2 71400123`);
  ok(ww?.id === 4 && /buffer index was read as 7 \(0 or 1\)/.test(ww.text), "W, the lanes' buffer index held");
  ww = wWords(`${c14} W 1/1 00000010 00000040 000008A2 71400123`);
  ok(ww?.id === 1 && /the latest in the reverb's loop/.test(ww.text) && /one-off wrong read of n/.test(ww.text), "W, the reverb's loop (id 1)");
  ww = wWords(`${c14} W 0`);
  ok(ww?.cuts === 0 && /reverb's or the delay's loop, and no lane count/.test(ww.text), "W 0 in build 14 names both loops and the clamps");
  // the last-boot line's C1F (test build 14, firmware/ble/ble_diag.h): the second core's record at CPU0's crash
  const last14 = "LAST FAILED STAGE 7 BLOCKS 227 LOAD 9 PEAK 16 NAN 0 CPU1 1 LOOPS 713 CRASH VEC 1 PC 1C67A04 RETS 1C67A6E DBG 0 EMU 0 UP 1964";
  let lb = parseLast(`${last14} C1F 1/1 1964 1C6F6DE`);
  ok(lb.crash.c1?.count === 1 && lb.crash.c1.cpu === 1 && lb.crash.c1.ms === 1964 && lb.crash.c1.pc === 0x1C6F6DE && lb.crash.clockMs === 1964,
     "last-boot line: the second core's record at the crash");
  ok(parseLast(last14).crash.c1 === undefined, "last-boot line without C1F (builds up to 13): no record");
  rp = report("FIFO 00000000 00000000 00000000 00000000", `${c14} W 0`, "UP 21000 SAFE 1 STAGE 7 RST 44 GUARD 1", `${last14} C1F 1/1 1964 1C6F6DE`, "BT OFF");
  ok(/the second core had faulted 1 time, the last taken by CPU1 at 1964 ms \(PC 01C6F6DE\): the same moment as this crash/.test(rp.text)
     && /^ {11}KEY 0: none/m.test(rp.text), "the safe-mode report names the second core's record and the build-14 fields");
  rp = report("FIFO 00000000 00000000 00000000 00000000", `${c14} W 0`, "UP 21000 SAFE 1 STAGE 7 RST 44 GUARD 1", `${last14} C1F 0/0 0 0`, "BT OFF");
  ok(/the second core had not faulted/.test(rp.text), "a crash with the second core's record empty: said so");
  // test build 14's VM r/w/s/f/q in the isr line (bt_isr_line, firmware/bt/bt_ps.h): the pairing store named
  const isr14 = (vm) => `ISR 40:12/310/1450 41:4/40/90 T5 9/31 AG 2144/2935 AI 846/942 EN 0/344/344/0/0 OC 0/3 OD 0 OG 12/240 C1 1 G 0 NAM 1 LD 42 PK 55/97 XR 3 CL 0 WC 0 BYP 0 TM 1234 VM ${vm} OVF 0 NAN 0`;
  let vw = vmWords(isr14("21/2/1/0/1"));
  ok(vw?.reads === 21 && vw.writes === 2 && vw.saves === 1 && vw.failed === 0 && vw.seq === 1
     && /read 21 and wrote 2; 1 saved to the flash since power-on \(sequence now 1\)$/.test(vw.text), `VM after a pairing: ${vw?.text}`);
  vw = vmWords(isr14("21/0/0/0/3"));
  ok(vw?.seq === 3 && /a copy loaded from the flash at power-on \(sequence 3\), none saved since$/.test(vw.text), "VM after a power-on: the copy loaded");
  vw = vmWords(isr14("21/0/0/0/0"));
  ok(vw?.seq === 0 && /no copy in the flash yet/.test(vw.text), "VM with nothing kept yet");
  vw = vmWords(isr14("21/0/0/3/0"));
  ok(/the flash copy left alone this session/.test(vw?.text) && !/rounds failed/.test(vw.text), "VM sequence 0 with f 3: the flash left alone at power-on");
  vw = vmWords(isr14("21/3/1/2/6"));
  ok(vw?.failed === 2 && /2 save rounds failed \(each tried again 1 s later\)$/.test(vw.text), "VM with failed save rounds");
  vw = vmWords(isr14("21/3/0/3/6"));
  ok(/3 save rounds failed: tried again only at a new change$/.test(vw?.text), "VM with the retries used up");
  vw = vmWords(isr14("4/2"));
  ok(vw?.reads === 4 && vw.writes === 2 && vw.saves === undefined && /in RAM only/.test(vw.text), "builds 11 to 13's VM r/w: in RAM only");
  ok(vmWords("ISR 40:1/2/3 TM 1234 VM 21/2/1/0/") === null && vmWords("ISR 40:1/2/3 TM 1234 VM 21/2/1") === null
     && vmWords("ISR 40:1/2/3") === null && vmWords(undefined) === null, "a VM cut short or missing is not named");
  {
    const out = [], r = { time: "", level: "full", info, ...diagFields(["UP 30000 SAFE 0 STAGE 8 RST 1 GUARD 0", "LAST OK",
      "BT VIS IRQ 40:1234 41:567 EV 4 RDY SCAN 1", "LINK", "EV", "HCI", "FIFO 00000000 00000000 00000000 00000000", isr14("21/2/1/0/1"), `${c14} W 0`]) };
    show(r, null, (x) => out.push(x));
    const t = out.join("\n");
    ok(/^isr {8}ISR 40:12/m.test(t) && /^ {11}pairing records: the library read 21 and wrote 2; 1 saved to the flash/m.test(t), "the report names the isr line's VM");
  }
  {
    // test build 28's eleventh line, the LE side; up to build 27 there is none, and a line that is not it is not taken
    const le = "LE 2 L3 W134 P1 A4/0 C24 P12 IRQ 812/97 ED 12/340 N0/1 CEN LINK R310 M2 T2 K2 S2 F0 MIDI 41 @A1B2C3D4E5F6";
    const lines = (l11) => ["UP 30000 SAFE 0 STAGE 8 RST 1 GUARD 0", "LAST OK", "BT VIS IRQ 40:1234 41:567 EV 4 RDY SCAN 1", "LINK", "EV",
      "HCI", "FIFO 00000000 00000000 00000000 00000000", "ISR", `${c1} W 0`, "C1R 0", ...(l11 === undefined ? [] : [l11])];
    const out = [], r = { time: "", level: "full", info, ...diagFields(lines(le)) };
    show(r, null, (x) => out.push(x));
    ok(r.le === le && out.some((x) => x === `le         ${le}`), "build 28's LE line: kept and shown as the le row");
    ok(diagFields(lines()).le === undefined && diagFields(lines("TRACE 1 2 3")).le === undefined, "no LE line (up to build 27), or another line there: no le row");
  }
}
b = parseBoot("UP 12345 SAFE 1 STAGE 7 RST 44 GUARD 1");
ok(b.clockMs === 12345 && b.safe && b.stageName === "FIRST 6 S" && b.resetBy.join() === "WATCHDOG,SOFT" && b.failedBoots === 1, "boot line");
b = parseLast("LAST FAILED STAGE 7 BLOCKS 9 LOAD 42 PEAK 61 NAN 0 CPU1 1 LOOPS 0 CRASH VEC 3F PC 2000DC8 RETS 2001234 DBG 0 EMU 8 UP 4100");
ok(b.failed && b.load === 42 && b.crash.vec === 0x3F && b.crash.pc === 0x2000DC8 && b.crash.emu === 8 && b.crash.clockMs === 4100, "last-boot line with a crash");
ok(!parseLast("LAST OK").failed, "last boot fine");

const fw = readFileSync(join(ROOT, "firmware/openpocket.c"), "utf8");
const strings = (s) => [...s.matchAll(/"([^"]*)"/g)].map((m) => m[1]);
ok(strings(/ST_NAME\[ST_N\] = \{([^}]*)\}/.exec(fw)[1]).join() === BOOT_STAGES.join(), "boot stage names match firmware/openpocket.c");
ok(strings(/N\[7\] = \{([^}]*)\}/.exec(fw)[1]).join() === RESET_CAUSES.join(), "reset causes match firmware/openpocket.c");

// ---- the verdicts on the real firmware text ----
const fast = ["--interval", "0.01", "--json"];
const seen = { ok: true, seconds: 10, hits: 42, others: 9, name: "OpenPocket", rssi: -60, connectable: true };
const nothing = { ok: true, seconds: 10, hits: 0, others: 9, name: null };

let s = sim("edsim_diag");
await s.ready;
let r = await run(s.access, fast);
ok(r.code === 1 && /Bluetooth is off/.test(r.last.verdict.reason), "firmware text: OFF in a normal boot fails");
ok(r.last.level === "full" && r.last.boot.clockMs === 30000 && r.last.lastBoot.failed === false, "DIAG reaches the parser through the real editor code");
ok(/^LINK 1 n2 A 060504030201 I48 /.test(r.last.link), "the link line comes through");

s.ble("failed 5 3");
r = await run(s.access, fast);
ok(r.code === 1 && /still starting after 5 s/.test(r.last.verdict.reason) && /code 3/.test(r.last.verdict.reason), `FAILED in words: ${r.last.verdict.reason}`);

s.ble("adv 100 500");
r = await run(s.access, [...fast, "--count", "1"]);
ok(r.code === 3 && r.last.verdict.result === "pending", "one reading of counters decides nothing");
r = await run(s.access, fast);
ok(r.code === 1 && /no baseband events/i.test(r.last.verdict.reason), "counters that stand still fail");

let n = 100;
const rising = async (scan) => {
  const t = setInterval(() => s.ble(`adv ${n += 10} ${n * 5}`), 5);
  const out = await run(s.access, ["--interval", "0.1", "--json", ...(scan ? ["--scan", "5"] : [])], scan ? { scan: async () => scan } : {});
  clearInterval(t);
  return out;
};
r = await rising();
ok(r.code === 0 && r.last.verdict.result === "running" && /not proof/.test(r.last.verdict.reason), "rising counters: running, and not called proof");
let v = judge([{ level: "full", time: "2026-11-06T13:00:00Z", boot: { stage: 8, clockMs: 178000 }, ble: parseBle("ADV i10 e0 r0 t0 s2 V 0 0 0 CON 6001 IEN 101 IPND 0 STAT 0 IC45 5 IC29 5 BEN 1") },
  { level: "full", time: "2026-11-06T13:00:02Z", boot: { stage: 8, clockMs: 1000 }, ble: parseBle("ADV i30 e0 r0 t0 s2 V 0 0 0 CON 6001 IEN 101 IPND 0 STAT 0 IC45 5 IC29 5 BEN 1") }]);
ok(v.result === "running" && /10\.0 a second/.test(v.reason) && v.notes.length === 3, `events without timers still run, across a clock wrap, with notes: ${v.notes.length}`);
r = await rising(seen);
ok(r.code === 0 && r.last.verdict.result === "pass" && r.last.scan.hits === 42, "rising counters and a sighting: pass");
r = await rising(nothing);
ok(r.code === 1 && /saw no advertisement/.test(r.last.verdict.reason), "rising counters and no sighting: fail");
r = await rising({ ok: false, why: "the Bluetooth adapter is not usable" });
ok(r.code === 3 && r.last.verdict.result === "unknown", "a scanner that could not run decides nothing");

s.ble("safe");
r = await run(s.access, fast);
ok(r.code === 1 && /last boot failed/i.test(r.last.verdict.reason) && /FIRST 6 S/.test(r.last.verdict.reason) && /WATCHDOG/.test(r.last.verdict.reason) && /safe mode/.test(r.last.verdict.reason), `safe mode in words: ${r.last.verdict.reason}`);
r = await run(s.access, ["--interval", "0.01", "--count", "1"]);
ok(/last boot  FAILED/.test(r.text) && /verdict    FAIL/.test(r.text) && /SAFE MODE/.test(r.text), "the plain report says the same");

s.dump(); await sleep(50);
const before = s.states.at(-1);
await run(s.access, fast);
s.dump(); await sleep(50);
ok(s.states.at(-1) === before && before.split("|")[2] === "1", "reading changes nothing on the pedal");
await s.end();

// ---- what less can be read ----
s = sim("edsim");
await s.ready;
r = await run(s.access, fast);
ok(r.code === 3 && r.last.level === "info" && r.last.ping.load === 42 && /no log channel/.test(r.last.verdict.reason), "firmware without DIAG: version and load, and says what is missing");
await s.end();

const blank = join(mkdtempSync(join(tmpdir(), "oplog-")), "flash.bin");
writeFileSync(blank, Buffer.alloc(0x100000, 0xFF));
const dev = simAccess([blank, "--stock", "BlackBox_016"]);
await dev.ready;
r = await run(dev.access, fast);
ok(r.code === 3 && r.last.level === "identity" && /identity/.test(r.last.verdict.reason), `stock firmware: ${r.last.verdict.reason}`);
await dev.end();

const port = (name) => ({ name, state: "connected", onmidimessage: null, open: async () => {}, send: () => {} });
const mute = { inputs: new Map([["OpenPocket", port("OpenPocket")]]), outputs: new Map([["OpenPocket", port("OpenPocket")]]) };
r = await run(mute, fast);
ok(r.code === 3 && r.last.level === "silent" && /nothing answers/.test(r.last.verdict.reason), "a port that does not answer");
r = await run({ inputs: new Map(), outputs: new Map() }, fast);
ok(r.code === 3 && r.last.level === "none" && /CRASH screen/.test(r.last.verdict.reason), "no port: names the screens this cannot read");

// --wait: the pedal shows up during the wait
s = sim("edsim_diag");
await s.ready;
s.ble("failed 1 -1");
const late = { inputs: new Map(), outputs: new Map() };
setTimeout(() => { for (const k of ["inputs", "outputs"]) for (const [n, p] of s.access[k]) late[k].set(n, p); }, 300);
r = await run(late, ["--wait", "5", "--interval", "0.05", "--count", "1", "--json"]);
ok(r.code === 1 && r.last.ble.step === 1, "--wait reads the pedal once it appears");
await s.end();

r = await run({ inputs: new Map(), outputs: new Map() }, ["--nonsense"]);
ok(r.code === 2 && /usage/.test(r.text), "a bad argument prints the usage");
ok(judge([{ level: "none" }]).result === "unknown", "no reading is never a pass");

if (failed) { console.log(`log test: ${failed} FAILED`); process.exit(1); }
console.log("log test passed");
process.exit(0);
