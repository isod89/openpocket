// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket web editor: TONE3000 (https://www.tone3000.com/api), the "Select" flow, in the browser.
// TONE3000 hosts the sign-in and the tone browser in a popup (OAuth 2.0 + PKCE, prompt=select_tone); the popup comes
// back to t3k.html, which hands the answer to this page. Then the tone's A2 models are listed and one is downloaded
// with the access token. Only the publishable key (client_id) is used here: it is meant for browser code.
// A free (non-commercial) app may use the Select flow and the bounded lists (the signed-in user's favorites, downloads
// and own tones, trending, latest), not the search endpoint: the search is TONE3000's own browser, in the popup.

// The publishable key of OpenPocket's TONE3000 app (www.tone3000.com/settings): public by design (browser code).
export const T3K_CLIENT_ID = "t3k_pub_OvUHfN4NJJMeMM-5d8Px1XDFBMcTYJ_0";
const cid = () => globalThis.__t3kClientId || T3K_CLIENT_ID;   // (the page test sets its own)
const API = "https://www.tone3000.com/api/v1";
const REDIRECT = new URL("t3k.html", location.href).href.replace(/[?#].*$/, "");

let token = null;           // { access, refresh, until (ms) }: in this page only, never stored

export const t3kReady = () => !!cid();
export const t3kRedirect = () => REDIRECT;
export const t3kSignedIn = () => !!token;
export const t3kSignOut = () => { token = null; };

function b64url(buf) {
  let s = "";
  for (const b of new Uint8Array(buf)) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=/g, "");
}

// opens TONE3000's tone browser (call from a click: the popup opens at once); resolves with
// { code, tone_id, verifier } (tone_id null: the user signed in, then closed the browser without a tone) or rejects
// (closed before signing in: "cancelled"; refused; an error from TONE3000)
export function t3kSelect() {
  const win = window.open("about:blank", "t3k", "width=1000,height=760");
  if (!win) return Promise.reject(new Error("the browser blocked the TONE3000 window: allow pop-ups for this page"));
  const verifier = crypto.randomUUID().replace(/-/g, "") + crypto.randomUUID().replace(/-/g, "");
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
    crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier)).then((h) => {
      const p = new URLSearchParams({
        client_id: cid(), redirect_uri: REDIRECT, response_type: "code",
        code_challenge: b64url(h), code_challenge_method: "S256", state,
        prompt: "select_tone", format: "nam", architecture: "2", gears: "amp_amp-cab_pedal",
        menubar: "true", preview: "true",
      });
      win.location.href = `${API}/oauth/authorize?${p}`;
    }, (e) => finish(reject, e));
  });
}

async function tokenCall(body) {
  const r = await fetch(`${API}/oauth/token`, {
    method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({ client_id: cid(), ...body }),
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok || !j.access_token) { token = null; throw new Error(`TONE3000 sign-in failed (${j.error || r.status})`); }
  token = { access: j.access_token, refresh: j.refresh_token || null, until: Date.now() + (j.expires_in || 3600) * 1000 - 30000 };
}
export async function t3kSignIn(sel) {
  await tokenCall({ grant_type: "authorization_code", code: sel.code, code_verifier: sel.verifier, redirect_uri: REDIRECT });
}
async function auth() {
  if (!token) throw new Error("not signed in to TONE3000");
  if (Date.now() > token.until && token.refresh) await tokenCall({ grant_type: "refresh_token", refresh_token: token.refresh });
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
// how many of a tone's models the pedal can play (A2), or null when the list does not say
export const t3kA2Count = (tone) => (Number.isInteger(tone.a2_models_count) ? tone.a2_models_count : null);
// what TONE3000 sends is shown as it comes, but an address from it is used only when it is a plain web address
// (a link) or a picture (https, or inline image data)
export const t3kLink = (u) => (typeof u === "string" && /^https:\/\//i.test(u) ? u : null);
export const t3kPicture = (u) => (typeof u === "string" && /^(https:\/\/|data:image\/)/i.test(u) ? u : null);
