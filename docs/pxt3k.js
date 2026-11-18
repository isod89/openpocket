// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket web editor: TONE3000 (https://www.tone3000.com/api), the "Select" flow, in the browser.
// TONE3000 hosts the sign-in and the tone browser in a popup (OAuth 2.0 + PKCE, prompt=select_tone); the popup comes
// back to t3k.html, which hands the answer to this page. Then the tone's A2 models are listed and one is downloaded
// with the access token. Only the publishable key (client_id) is used here: it is meant for browser code.
// A browser that opens no second window (an in-app browser such as MIDIWeb, the Web MIDI browser iOS needs for Bluetooth
// MIDI) goes to TONE3000 in this tab instead (t3kSelectHere): the code verifier, the state and the address to come back to
// wait in this tab's sessionStorage, and t3k.html finishes there (t3kHere): it signs in, gets the chosen A2 captures ready
// for the pedal and comes back to the editor with them (t3kNote). The token lives in that page's memory only.
// A free (non-commercial) app may use the Select flow and the bounded lists (the signed-in user's favorites, downloads
// and own tones, trending, latest), not the search endpoint: the search is TONE3000's own browser, in the popup.

// The publishable key of OpenPocket's TONE3000 app (www.tone3000.com/settings): public by design (browser code).
export const T3K_CLIENT_ID = "t3k_pub_OvUHfN4NJJMeMM-5d8Px1XDFBMcTYJ_0";
// A site that publishes the editor under its own TONE3000 app: that app's key, its redirect being this site's t3k.html
// (steponme82's fork: https://steponme82.github.io/openpocket/t3k.html). Everywhere else, the project's key above. Only
// publishable keys: the Select flow runs with PKCE, no secret.
const T3K_SITE_KEYS = { "https://steponme82.github.io": "t3k_pub_dlDUT4gBuF5xAs0MpjZHnU7EeQ09DESn" };
export const t3kClientFor = (origin) => T3K_SITE_KEYS[origin] || T3K_CLIENT_ID;
const cid = () => globalThis.__t3kClientId || t3kClientFor(location.origin);   // (the page test sets its own)
const API = "https://www.tone3000.com/api/v1";
const REDIRECT = new URL("t3k.html", location.href).href.replace(/[?#].*$/, "");
const PENDING = "openpocket-t3k", NOTE = "openpocket-t3k-note";   // (sessionStorage: this tab's request; t3k.html's answer)

let token = null;           // { access, refresh, until (ms), client }: in this page only, never stored

export const t3kReady = () => !!cid();
export const t3kRedirect = () => REDIRECT;
export const t3kSignedIn = () => !!token;
export const t3kSignOut = () => { token = null; };

function b64url(buf) {
  let s = "";
  for (const b of new Uint8Array(buf)) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=/g, "");
}

const newVerifier = () => crypto.randomUUID().replace(/-/g, "") + crypto.randomUUID().replace(/-/g, "");
const challengeOf = async (v) => b64url(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(v)));
// TONE3000's Select page, the same for the popup and for this tab
function authorizeUrl(client, state, challenge) {
  const p = new URLSearchParams({
    client_id: client, redirect_uri: REDIRECT, response_type: "code",
    code_challenge: challenge, code_challenge_method: "S256", state,
    prompt: "select_tone", format: "nam", architecture: "2", gears: "amp_amp-cab_pedal",
    menubar: "true", preview: "true",
  });
  return `${API}/oauth/authorize?${p}`;
}

// opens TONE3000's tone browser (call from a click: the popup opens at once); resolves with
// { code, tone_id, verifier } (tone_id null: the user signed in, then closed the browser without a tone) or rejects
// (closed before signing in: "cancelled"; refused; an error from TONE3000). No window at all (an in-app browser, or
// pop-ups blocked): rejects with .noWindow set, and the caller can go there in this tab (t3kSelectHere)
export function t3kSelect() {
  const win = window.open("about:blank", "t3k", "width=1000,height=760");
  if (!win) return Promise.reject(Object.assign(new Error("the browser opened no TONE3000 window: allow pop-ups for this page"), { noWindow: true }));
  const verifier = newVerifier();
  const state = crypto.randomUUID();
  return new Promise((resolve, reject) => {
    let done = false;
    const finish = (f, v) => { if (done) return; done = true; window.removeEventListener("message", onMsg); clearInterval(poll); f(v); };
    const onMsg = (ev) => {
      if (ev.origin !== location.origin || !ev.data || ev.data.t3k !== 1) return;
      const q = ev.data;
      try { win.close(); } catch (_) {}
      if (q.state !== state) return finish(reject, new Error("TONE3000 answered with the wrong state (try again)"));
      if (q.error) return finish(reject, new Error(q.error === "access_denied" ? "cancelled" : `TONE3000: ${q.error}`));
      if (!q.code) return finish(reject, new Error(q.canceled ? "cancelled" : "no tone was chosen"));
      if (!q.tone_id && !q.canceled) return finish(reject, new Error("no tone was chosen"));
      finish(resolve, { code: q.code, tone_id: q.tone_id || null, verifier });
    };
    window.addEventListener("message", onMsg);
    const poll = setInterval(() => { if (win.closed) finish(reject, new Error("cancelled")); }, 500);
    challengeOf(verifier).then((c) => { win.location.href = authorizeUrl(cid(), state, c); }, (e) => finish(reject, e));
  });
}

// TONE3000 in this tab (no second window): what t3k.html needs to finish waits in this tab's sessionStorage, then the page
// leaves for TONE3000. `back`: the editor's address to come back to. Throws when the tab can keep nothing.
export async function t3kSelectHere(back) {
  const verifier = newVerifier(), state = crypto.randomUUID(), client = cid();
  const challenge = await challengeOf(verifier);
  try { sessionStorage.setItem(PENDING, JSON.stringify({ state, verifier, client, back, at: Date.now() })); }
  catch (_) { throw new Error("this browser keeps nothing for the page while it is away, so TONE3000 could not come back to it"); }
  location.assign(authorizeUrl(client, state, challenge));
}
const sameSite = (u) => { try { return new URL(u, location.href).origin === location.origin; } catch (_) { return false; } };
// t3k.html with no window that opened it: TONE3000's answer `q` ({ code, state, tone_id, canceled, error }) against the
// request this tab sent, used once -> null (this tab sent none: the page was opened some other way), { error, back }, or
// { code, tone_id, verifier, client, back }
export function t3kHere(q) {
  let p = null;
  try { p = JSON.parse(sessionStorage.getItem(PENDING) || "null"); sessionStorage.removeItem(PENDING); } catch (_) { p = null; }
  if (!p || typeof p.state !== "string" || typeof p.verifier !== "string") return null;
  const back = typeof p.back === "string" && sameSite(p.back) ? new URL(p.back, location.href).href : new URL("editor.html#models", location.href).href;
  if (q.state !== p.state || !(Date.now() - p.at < 30 * 60000)) return { error: "TONE3000 answered a request this page did not send, or an old one: try again", back };
  if (q.error) return { error: q.error === "access_denied" ? "cancelled" : `TONE3000: ${q.error}`, back };
  if (!q.code || !q.tone_id) return { error: q.code || q.canceled ? "cancelled" : "no tone was chosen", back };
  return { code: q.code, tone_id: q.tone_id, verifier: p.verifier, client: typeof p.client === "string" ? p.client : null, back };
}
// what t3k.html brings back to the editor in this tab: { captures: [{ name, bytes (base64), detail, source }] } or { error };
// read once by the editor
export function t3kNote(n) { try { sessionStorage.setItem(NOTE, JSON.stringify(n)); return true; } catch (_) { return false; } }
export function t3kNoteRead() {
  try { const n = JSON.parse(sessionStorage.getItem(NOTE) || "null"); sessionStorage.removeItem(NOTE); return n && typeof n === "object" ? n : null; } catch (_) { return null; }
}

async function tokenCall(body, client) {
  const r = await fetch(`${API}/oauth/token`, {
    method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({ client_id: client, ...body }),
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok || !j.access_token) { token = null; throw new Error(`TONE3000 sign-in failed (${j.error || r.status})`); }
  token = { access: j.access_token, refresh: j.refresh_token || null, until: Date.now() + (j.expires_in || 3600) * 1000 - 30000, client };
}
// sel: { code, verifier, client? } (client: the key the request was made with, when it was another page's)
export async function t3kSignIn(sel) {
  await tokenCall({ grant_type: "authorization_code", code: sel.code, code_verifier: sel.verifier, redirect_uri: REDIRECT }, sel.client || cid());
}
async function auth() {
  if (!token) throw new Error("not signed in to TONE3000");
  if (Date.now() > token.until && token.refresh) await tokenCall({ grant_type: "refresh_token", refresh_token: token.refresh }, token.client);
  return { Authorization: `Bearer ${token.access}` };
}
async function get(path) {
  const r = await fetch(`${API}${path}`, { headers: await auth() });
  if (r.status === 429) throw new Error("TONE3000 asks to slow down (too many requests): try again in a minute");
  if (r.status === 401) { token = null; throw new Error("the TONE3000 sign-in ended: browse TONE3000 again to sign in"); }
  if (!r.ok) throw new Error(`TONE3000 answered ${r.status}`);
  return r.json();
}
// the tone (title, gear, format, images, user: { username, display_name, avatar_url, url }, url)
export const t3kTone = (id) => get(`/tones/${encodeURIComponent(id)}`);
// its A2 models: [{ id, name, size, model_url, architecture_version }]
export async function t3kModels(toneId) {
  const j = await get(`/models?tone_id=${encodeURIComponent(toneId)}&architecture=2&page_size=100`);
  return (j.data || []).filter((m) => String(m.architecture_version) === "2" && m.model_url);
}
// a model's .nam file (text)
export async function t3kDownload(m) {
  const r = await fetch(m.model_url, { headers: await auth() });
  if (!r.ok) throw new Error(`the download failed (${r.status})`);
  return r.text();
}
// who is signed in: { username, display_name, avatar_url, url }
export const t3kUser = () => get("/user");
// the lists a free app may read: the signed-in user's, and TONE3000's front page
export const T3K_LISTS = [
  { id: "favorited", label: "Favorites", empty: "No favorites yet. Mark tones with the heart on TONE3000 and they show up here." },
  { id: "downloaded", label: "Downloaded", empty: "Nothing downloaded with this account yet." },
  { id: "trending", label: "Trending", empty: "TONE3000 lists nothing as trending right now." },
  { id: "latest", label: "Latest", empty: "TONE3000 lists nothing new right now." },
];
export async function t3kList(id) {
  const paged = id === "favorited" || id === "downloaded" || id === "created";
  const j = await get(`/tones/${id}${paged ? "?page_size=50" : ""}`);
  return (j.data || []).filter((t) => !t.format || t.format === "nam");
}
// a capture's origin, as the library keeps it (docs/pxlib.js)
export function t3kSource(tone, m) {
  const u = tone.user || {};
  return { from: "tone3000", toneId: tone.id, title: tone.title || "Tone", creator: u.display_name || u.username || "", creatorUrl: u.url || "",
    url: tone.url || "", gear: tone.gear || "", format: tone.format || "", image: (tone.images || [])[0] || "", model: m.name || "" };
}
// how many of a tone's models the pedal can play (A2), or null when the list does not say
export const t3kA2Count = (tone) => (Number.isInteger(tone.a2_models_count) ? tone.a2_models_count : null);
// what TONE3000 sends is shown as it comes, but an address from it is used only when it is a plain web address
// (a link) or a picture (https, or inline image data)
export const t3kLink = (u) => (typeof u === "string" && /^https:\/\//i.test(u) ? u : null);
export const t3kPicture = (u) => (typeof u === "string" && /^(https:\/\/|data:image\/)/i.test(u) ? u : null);
