/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — the web editor's protocol (docs/EDITOR-PROTOCOL.md): SysEx F0 7D 4F 50 cmd args.. F7.
 *
 * The editor edits PRESET, the very sound the pedal's screen shows and the engine plays (audio_poll applies any change,
 * ui_render redraws what changed), and the 52 slots of the store (px_store.h). Frames come through usb.c's sx_frame
 * (ota_frame_get), replies leave through ota_wire_send, as the FM-1 editor (felucca's editor.c) does; ed_service runs
 * before ota_service, which would take any frame.
 * While watched, a change made on the pedal (knob, buttons, footswitches) is pushed as a STATE frame; a change the
 * editor made itself is not echoed back.
 * The including file provides: PRESET, cur_preset, load_preset, ui_blk / ui_mode / ui_item (ui.h), st_gen, the
 * store (px_store.h), BLK_COL (ui_colors.h), ota_frame_get / ota_frame_done / ota_wire_send, ms_now, OP_VERSION,
 * FELUCCA_ID_PTR, and ed_load / ed_flags (the DSP load for PING). An image with a log channel also defines ED_DIAG_LINES
 * and ed_diag_line(i, out), which writes line i (at most ED_DIAG_LEN - 1 characters); without them DIAG gets no reply.
 */
#pragma once

#define ED_H0 0x7Du
#define ED_H1 0x4Fu
#define ED_H2 0x50u
#define ED_PROTO 1u
#define ED_NAME_MAX 13u
#define ED_P_SIZE 110u
typedef char ed_size_check[sizeof(px_preset_t) == ED_P_SIZE ? 1 : -1];
enum { ED_INFO = 1, ED_CAT_BLOCK, ED_CAT_TYPE, ED_TEXTS, ED_STATE, ED_SET, ED_ORDER, ED_NAME, ED_PUT, ED_SELECT,
       ED_FOCUS, ED_LIST, ED_SLOT_GET, ED_SLOT_PUT, ED_COMMIT, ED_SAVE, ED_WATCH, ED_PING, ED_DIAG };

static uint8_t ed_out[1024];
static uint32_t ed_n;
static struct { uint8_t on; uint32_t last_ms, hash, push_ms; } ed_w;

static void ed_begin(uint32_t cmd)
{
    ed_out[0] = 0xF0; ed_out[1] = ED_H0; ed_out[2] = ED_H1; ed_out[3] = ED_H2; ed_out[4] = (uint8_t)cmd;
    ed_n = 5;
}
static void ed_b(uint32_t v) { if (ed_n < sizeof ed_out - 1u) ed_out[ed_n++] = (uint8_t)(v & 0x7Fu); }
static void ed_str(const char *s, uint32_t max)
{
    for (uint32_t i = 0; s && s[i] && i < max; i++) ed_b((uint8_t)s[i]);
    ed_b(0);
}
static void ed_send(void) { ed_out[ed_n++] = 0xF7; ota_wire_send(ed_out, ed_n); }
static void ed_preset(const px_preset_t *p)
{
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < ED_P_SIZE; i++) ed_b(b[i]);
}

/* a received preset: every field in range, the name cleaned; 0 = invalid */
static int ed_take_preset(const uint8_t *a, px_preset_t *q)
{
    uint32_t i, k;
    memcpy(q, a, ED_P_SIZE);
    for (i = 0; i < 14u; i++) {
        uint8_t c = (uint8_t)q->name[i];
        if (i >= ED_NAME_MAX || !c) { for (; i < 14u; i++) q->name[i] = 0; break; }
        if (c < 32u || c > 126u) q->name[i] = '?';
    }
    for (i = 0; i < PX_NBLK; i++) {
        if (q->on[i] > 1u || q->type[i] >= (uint8_t)px_ntypes((int)i)) return 0;
        for (k = 0; k < PX_NPAR; k++) if (q->p[i][k] > 100u) return 0;
    }
    return px_order_valid(q->order);
}

/* the sound differs from what its slot holds (the pedal's "*") */
static int ed_dirty(void)
{
    static px_preset_t ref;
    static uint32_t ref_slot = ~0u, ref_gen = ~0u;
    if (ref_slot != cur_preset || ref_gen != st_gen) { st_get(cur_preset, &ref); ref_slot = cur_preset; ref_gen = st_gen; }
    return memcmp(&ref, &PRESET, sizeof ref) != 0;
}
static uint32_t ed_state_hash(void)
{
    const uint8_t *b = (const uint8_t *)&PRESET;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < sizeof PRESET; i++) h = (h ^ b[i]) * 16777619u;
    h = (h ^ cur_preset) * 16777619u;
    h = (h ^ ui_blk) * 16777619u;
    return (h ^ st_gen) * 16777619u;
}
static void ed_state_frame(void)
{
    ed_begin(ED_STATE);
    ed_b(cur_preset);
    ed_b((ed_dirty() ? 1u : 0u) | (st_saved(cur_preset) ? 2u : 0u));
    ed_b(ui_blk);
    ed_preset(&PRESET);
}
static void ed_ui_fix(void)                       /* the block page after a type change, a new preset... */
{
    if (ui_mode == M_EDIT && ui_item >= 2u + (uint32_t)px_nparams((int)ui_blk, PRESET.type[ui_blk])) ui_mode = M_BLOCK;
    if (ui_item >= 2u + (uint32_t)px_nparams((int)ui_blk, PRESET.type[ui_blk])) ui_item = 0;
}

static void ed_handle(const uint8_t *a, uint32_t n)   /* a: the args, after the command byte */
{
    uint32_t cmd = a[-1], i, k;
    int rc;
    px_preset_t q;
    ed_begin(cmd);
    switch (cmd) {
    case ED_INFO:
        ed_b(ED_PROTO); ed_str(OP_VERSION, 15); ed_str(FELUCCA_ID_PTR, 27);
        ed_b(PX_NBLK); ed_b(PX_NPAR); ed_b(ST_NSLOT); ed_b(ED_NAME_MAX);
        break;
    case ED_CAT_BLOCK:
        if (n < 1u || a[0] >= PX_NBLK) return;
        ed_b(a[0]); ed_str(px_block_label(a[0]), 15); ed_str(px_block_name(a[0]), 31); ed_b((uint32_t)px_ntypes(a[0]));
        { uint32_t c = BLK_COL[a[0]]; ed_b(c); ed_b(c >> 7); ed_b(c >> 14); }
        break;
    case ED_CAT_TYPE: {
        if (n < 2u || a[0] >= PX_NBLK || a[1] >= (uint32_t)px_ntypes(a[0])) return;
        int np = px_nparams(a[0], a[1]);
        uint32_t mask = 0;
        char t[12];
        for (k = 0; k < (uint32_t)np; k++)
            for (i = 0; i <= 100u && !(mask >> k & 1u); i++)
                if (px_param_text(a[0], a[1], (int)k, (int)i, t)) mask |= 1u << k;
        ed_b(a[0]); ed_b(a[1]); ed_str(px_type_name(a[0], a[1]), 31); ed_b((uint32_t)np); ed_b(mask);
        for (k = 0; k < (uint32_t)np; k++) ed_str(px_param_name(a[0], a[1], (int)k), 15);
        break;
    }
    case ED_TEXTS: {
        if (n < 3u || a[0] >= PX_NBLK || a[1] >= (uint32_t)px_ntypes(a[0]) || a[2] >= PX_NPAR) return;
        char t[12];
        ed_b(a[0]); ed_b(a[1]); ed_b(a[2]);
        for (i = 0; i <= 100u; i++) {
            t[8] = 0;
            ed_str(px_param_text(a[0], a[1], a[2], (int)i, t) ? t : "", 8);
        }
        break;
    }
    case ED_STATE:
        ed_state_frame();
        break;
    case ED_SET: {
        if (n < 4u || a[0] >= PX_NBLK) return;
        uint32_t b = a[0], v = a[3];
        if (a[1] == 0u) {
            if (a[2] >= PX_NPAR) return;
            PRESET.p[b][a[2]] = (uint8_t)(v > 100u ? 100u : v);
            v = PRESET.p[b][a[2]];
        } else if (a[1] == 1u) {
            PRESET.on[b] = (uint8_t)(v ? 1u : 0u);
            v = PRESET.on[b];
        } else if (a[1] == 2u) {
            uint32_t nt = (uint32_t)px_ntypes((int)b);
            PRESET.type[b] = (uint8_t)(v < nt ? v : nt - 1u);
            v = PRESET.type[b];
            ed_ui_fix();
        } else return;
        ed_b(b); ed_b(a[1]); ed_b(a[2]); ed_b(v);
        break;
    }
    case ED_ORDER:
        if (n < PX_NBLK) return;
        rc = px_order_valid(a) ? 0 : 1;
        if (!rc) memcpy(PRESET.order, a, PX_NBLK);
        ed_b((uint32_t)rc);
        break;
    case ED_NAME:
        for (i = 0; i < 14u; i++) q.name[i] = 0;
        for (i = 0; i < n && a[i] && i < ED_NAME_MAX; i++) q.name[i] = (char)(a[i] >= 32u && a[i] <= 126u ? a[i] : '?');
        memcpy(PRESET.name, q.name, 14);
        ed_str(PRESET.name, 14);
        break;
    case ED_PUT:
        if (n < ED_P_SIZE) return;
        rc = ed_take_preset(a, &q) ? 0 : 1;
        if (!rc) { PRESET = q; ed_ui_fix(); }
        ed_b((uint32_t)rc);
        break;
    case ED_SELECT:
        if (n < 1u) return;
        rc = a[0] < ST_NSLOT ? 0 : 1;
        if (!rc) { cur_preset = a[0]; load_preset(cur_preset); ed_ui_fix(); }
        ed_b(a[0]); ed_b((uint32_t)rc);
        break;
    case ED_FOCUS:
        if (n < 1u || a[0] >= PX_NBLK) return;
        if (ui_mode == M_CHAIN || ui_mode == M_BLOCK || ui_mode == M_EDIT) {
            if (ui_blk != a[0]) { ui_blk = a[0]; ui_item = 0; }
            ui_mode = M_BLOCK;
        }
        ed_b(a[0]);
        break;
    case ED_LIST: {
        if (n < 2u) return;
        uint32_t s = a[0], c = a[1];
        if (s >= ST_NSLOT) s = ST_NSLOT;
        if (c > 26u) c = 26u;
        if (s + c > ST_NSLOT) c = ST_NSLOT - s;
        ed_b(s); ed_b(c);
        for (k = s; k < s + c; k++) { st_get(k, &q); ed_b(st_saved(k) ? 1u : 0u); ed_str(q.name, 14); }
        break;
    }
    case ED_SLOT_GET:
        if (n < 1u || a[0] >= ST_NSLOT) return;
        st_get(a[0], &q);
        ed_b(a[0]); ed_b(st_saved(a[0]) ? 1u : 0u); ed_preset(&q);
        break;
    case ED_SLOT_PUT:
        if (n < 2u || a[0] >= ST_NSLOT) return;
        rc = 0;
        if (a[1] & 1u) {
            if (n < 2u + ED_P_SIZE || !ed_take_preset(a + 2, &q)) rc = 1;
            else { memset(st_slot[a[0]], 0xFF, ST_SLOT); memcpy(st_slot[a[0]], &q, sizeof q); st_slot[a[0]][sizeof q] = ST_MARK; }
        } else memset(st_slot[a[0]], 0xFF, ST_SLOT);
        if (!rc && (a[1] & 2u)) rc = st_commit() ? 2 : 0;
        st_gen++;
        ed_b(a[0]); ed_b((uint32_t)rc);
        break;
    case ED_COMMIT:
        rc = st_commit() ? 2 : 0;
        st_gen++;
        ed_b((uint32_t)rc);
        break;
    case ED_SAVE:
        if (n < 1u || a[0] >= ST_NSLOT) return;
        rc = st_save(a[0], &PRESET) ? 2 : 0;
        if (!rc) cur_preset = a[0];
        st_gen++;
        if (!rc) load_preset(cur_preset);
        ed_b(a[0]); ed_b((uint32_t)rc);
        break;
    case ED_WATCH:
        ed_w.on = n >= 1u && a[0] ? 1u : 0u;
        ed_b(ed_w.on);
        break;
    case ED_PING:
        ed_b(ed_load() > 127u ? 127u : ed_load()); ed_b(ed_flags());
        break;
#ifdef ED_DIAG_LINES
    case ED_DIAG: {                                    /* read only: nothing here changes the pedal */
        static char l[ED_DIAG_LEN];
        ed_b(ED_DIAG_LINES);
        for (i = 0; i < ED_DIAG_LINES; i++) { ed_diag_line(i, l); ed_str(l, sizeof l - 1u); }
        break;
    }
#endif
    default:
        return;
    }
    ed_w.hash = ed_state_hash();                       /* the editor's own change: no push back */
    ed_send();
}

/* main loop, before ota_service: editor frames; anything else stays for ota_service */
static void ed_service(uint32_t now)
{
    const uint8_t *p;
    uint32_t n;
    if (ed_w.on && now - ed_w.last_ms > 5000u) ed_w.on = 0;    /* the editor went away */
    if (ed_w.on && now - ed_w.push_ms >= 40u) {
        uint32_t h = ed_state_hash();
        if (h != ed_w.hash) { ed_w.hash = h; ed_w.push_ms = now; ed_state_frame(); ed_send(); }
    }
    if (!ota_frame_get(&p, &n) || n < 4u || p[0] != ED_H0 || p[1] != ED_H1 || p[2] != ED_H2)
        return;
    {
        static uint8_t f[160];                         /* the longest request: SLOT_PUT, 4 + 2 + 110 bytes */
        uint32_t k = n > sizeof f ? sizeof f : n, i;
        for (i = 0; i < k; i++) f[i] = p[i];
        ota_frame_done();                              /* free the frame buffer before replying */
        ed_w.last_ms = now;
        ed_handle(f + 4, k - 4u);
    }
}
