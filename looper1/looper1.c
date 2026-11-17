/* LOOPER 01 - ARP / WAV / KIT / A09 looper for VIBE FM. v0.1: keys (WAV) + pads (KIT) + the looper. */
#include "vibe.h"
#include "vibe_dsp.h"
#include "vibe_gfx.h"

#define MAXEV  768
#define NVOICE 6
#define BARS   4

enum { ST_IDLE, ST_ARMED, ST_REC, ST_PLAY };

typedef struct { uint32_t t; uint16_t lap; uint8_t kind, id; float vel; } ev_t;   /* kind 0 note on, 1 note off, 2 pad */
typedef struct { int note; float ph, dt, vel; uint32_t age; vd_env env; } voice_t;
typedef struct { float f, drop, tone, noise, dec; int hp; } dtab_t;
typedef struct { float env, ph, vel, prev, c; } drum_t;

static float sr = 48000.0f;
static float bpm = 100.0f;
static uint32_t loop_len, pos;
static uint16_t lap;
static int st = ST_IDLE, ovd, help;

static ev_t evs[MAXEV];
static int nev;
static uint8_t held[128];

static voice_t V[NVOICE];
static uint32_t vage;
static vd_svf svf;
static vd_svf_c svfc;
static float cut_n = 0.6f, cut_last = -1.0f;
static vd_noise nz = { 12345 };

/* 16 pads: kick, snare, hats, clap, toms, then higher variations */
static const dtab_t DT[16] = {
  {  52, 130, 1.0f, 0.04f, 0.30f, 0 }, {  190,  60, 0.5f, 0.70f, 0.15f, 0 },
  {   0,   0, 0.0f, 1.00f, 0.04f, 1 }, {    0,   0, 0.0f, 1.00f, 0.25f, 1 },
  {   0,   0, 0.0f, 0.90f, 0.13f, 0 }, {   90,  40, 1.0f, 0.00f, 0.32f, 0 },
  { 130,  50, 1.0f, 0.00f, 0.26f, 0 }, {  190,  60, 1.0f, 0.00f, 0.22f, 0 },
  {  65, 150, 1.0f, 0.08f, 0.40f, 0 }, {  220,  70, 0.4f, 0.80f, 0.18f, 0 },
  {   0,   0, 0.0f, 1.00f, 0.07f, 1 }, {    0,   0, 0.0f, 1.00f, 0.45f, 1 },
  { 800, 100, 0.6f, 0.50f, 0.06f, 0 }, { 1200,  80, 0.7f, 0.30f, 0.05f, 0 },
  { 330,  90, 1.0f, 0.10f, 0.15f, 0 }, {  520, 120, 1.0f, 0.05f, 0.12f, 0 }
};
static drum_t D[16];
static float flash[16];

static void recompute_len(void) { loop_len = (uint32_t)((float)(BARS * 4) * 60.0f / bpm * sr); if (loop_len < 1000) loop_len = 1000; }

/* ---- sound ---- */
static void note_on(int note, float vel) {
  int best = 0; uint32_t oldest = 0xFFFFFFFFu;
  for (int i = 0; i < NVOICE; i++) {
    if (V[i].env.stage == 0) { best = i; oldest = 0; break; }
    if (V[i].age < oldest) { oldest = V[i].age; best = i; }
  }
  voice_t *v = &V[best];
  v->note = note; v->vel = vel; v->age = ++vage; v->ph = 0.0f;
  v->dt = vd_mtof((float)note) / sr;
  vd_env_set(&v->env, 0.004f, 0.18f, 0.65f, 0.22f, sr);
  vd_env_gate(&v->env, 1);
}
static void note_off(int note) {
  for (int i = 0; i < NVOICE; i++) if (V[i].note == note && V[i].env.stage != 0 && V[i].env.stage != 3) vd_env_gate(&V[i].env, 0);
}
static void drum_hit(int pad, float vel) {
  if (pad < 0 || pad > 15) return;
  D[pad].env = 1.0f; D[pad].vel = vel; D[pad].ph = 0.0f; D[pad].c = vd_coef(DT[pad].dec, sr);
}
static void all_off(void) {
  for (int i = 0; i < NVOICE; i++) if (V[i].env.stage != 0 && V[i].env.stage != 3) vd_env_gate(&V[i].env, 0);
}

static void render(float *l, float *r, int n) {
  if (cut_n != cut_last) { vd_svf_set(&svfc, 80.0f * vd_pow(60.0f, cut_n), 0.9f, sr); cut_last = cut_n; }
  for (int k = 0; k < n; k++) {
    float s = 0.0f, d = 0.0f;
    for (int i = 0; i < NVOICE; i++) {
      voice_t *v = &V[i];
      if (v->env.stage == 0) continue;
      float e = vd_env_next(&v->env);
      float x = vd_saw(v->ph, v->dt) * 0.6f + vd_sin(v->ph) * 0.5f;
      v->ph += v->dt; if (v->ph >= 1.0f) v->ph -= 1.0f;
      s += x * e * v->vel;
    }
    s = vd_svf_tick(&svf, &svfc, s).lp;
    for (int p = 0; p < 16; p++) {
      drum_t *m = &D[p];
      if (m->env < 0.0005f) { m->env = 0.0f; continue; }
      const dtab_t *t = &DT[p];
      m->ph += (t->f + t->drop * m->env) / sr; if (m->ph >= 1.0f) m->ph -= 1.0f;
      float nn = vd_noise_next(&nz);
      if (t->hp) { float h = nn - m->prev; m->prev = nn; nn = h * 0.5f; }
      d += (vd_sin(m->ph) * t->tone + nn * t->noise) * m->env * m->vel;
      m->env *= m->c;
    }
    float o = vd_tanh(s * 0.5f + d * 0.8f);
    l[k] = o; r[k] = o;
  }
}

/* ---- looper ---- */
static int recording(void) { return st == ST_REC || (st == ST_PLAY && ovd); }
static void rec_add(int kind, int id, float vel) {
  if (!recording() || nev >= MAXEV) return;
  ev_t *e = &evs[nev++]; e->t = pos; e->lap = lap; e->kind = (uint8_t)kind; e->id = (uint8_t)id; e->vel = vel;
}
static void maybe_start(void) { if (st == ST_ARMED) { st = ST_REC; pos = 0; lap++; } }
static void trigger(const ev_t *e) {
  if (e->kind == 0) note_on(e->id, e->vel);
  else if (e->kind == 1) note_off(e->id);
  else { drum_hit(e->id, e->vel); flash[e->id & 15] = 1.0f; }
}
static void wrap(void) {
  int was = recording();
  pos = 0; lap++;
  if (st == ST_REC) st = ST_PLAY;
  if (was) {
    for (int n = 0; n < 128; n++) if (held[n] && nev < MAXEV) {
      ev_t *e = &evs[nev++]; e->t = loop_len - 1; e->lap = lap; e->kind = 1; e->id = (uint8_t)n; e->vel = 0.0f;
    }
  }
  ovd = 0;
}

/* ---- the four functions ---- */
void vibe_on_init(float sample_rate) {
  sr = sample_rate;
  recompute_len();
  vibe_notes(VIBE_NOTES);
  for (int i = 0; i < NVOICE; i++) vd_env_set(&V[i].env, 0.004f, 0.18f, 0.65f, 0.22f, sr);
  vibe_label(VB_F1, "CLEAR");
}

void vibe_on_event(const vibe_event *e) {
  if (e->type == VIBE_EV_BUTTON) {
    if (!e->down) return;
    switch (e->id) {
      case VB_HOME: help = !help; break;
      case VB_RETURN: help = 0; break;
      case VB_F1: st = ST_IDLE; ovd = 0; pos = 0; nev = 0; all_off(); break;
      case VB_STOP: if (st != ST_IDLE) { st = ST_IDLE; ovd = 0; pos = 0; all_off(); } break;
      case VB_PLAY: if (st == ST_IDLE && nev > 0) { st = ST_PLAY; pos = 0; lap++; } break;
      case VB_REC:
        if (st == ST_IDLE) { nev = 0; st = ST_ARMED; }
        else if (st == ST_ARMED) st = ST_IDLE;
        else if (st == ST_PLAY) ovd = !ovd;
        break;
    }
  } else if (e->type == VIBE_EV_NOTE) {
    if (e->id > 127) return;
    if (e->down) { maybe_start(); note_on(e->id, e->value); held[e->id] = 1; rec_add(0, e->id, e->value); }
    else { note_off(e->id); held[e->id] = 0; rec_add(1, e->id, 0.0f); }
  } else if (e->type == VIBE_EV_PAD) {
    if (!e->down || e->id > 15) return;
    maybe_start(); drum_hit(e->id, e->value > 0.0f ? e->value : 0.8f); flash[e->id] = 1.0f; rec_add(2, e->id, e->value > 0.0f ? e->value : 0.8f);
  } else if (e->type == VIBE_EV_ENCODER) {
    if (e->id == VE_1) { cut_n += e->value * 0.02f; cut_n = vd_clamp(cut_n, 0.0f, 1.0f); }
    else if (e->id == VE_2 && nev == 0 && (st == ST_IDLE || st == ST_ARMED)) { bpm = vd_clamp(bpm + e->value, 60.0f, 180.0f); recompute_len(); }
  }
}

void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames) {
  (void)in;
  int done = 0;
  while (done < frames) {
    int n = frames - done;
    int running = (st == ST_REC || st == ST_PLAY);
    if (running) {
      int remain = (int)(loop_len - pos);
      if (n > remain) n = remain;
      for (int i = 0; i < nev; i++)
        if (evs[i].lap != lap && evs[i].t >= pos && evs[i].t < pos + (uint32_t)n) trigger(&evs[i]);
    }
    render(out_l + done, out_r + done, n);
    if (running) { pos += (uint32_t)n; if (pos >= loop_len) wrap(); }
    done += n;
  }
}

static const char *const ROWS[] = {
  "KEYS", "bass notes (WAV)", "PADS", "drum kit (KIT)", "REC", "arm: starts at first note or pad",
  "PLAY / STOP", "loop on / off", "REC in loop", "overdub one lap", "F1 CLEAR", "erase the loop",
  "ENCODER 1", "bass filter", "ENCODER 2", "tempo (empty loop)"
};

void vibe_on_draw(uint16_t *fb) {
  vg_begin(fb);
  if (help) { vg_clear(0x101214); vg_controls("LOOPER", ROWS, (int)(sizeof ROWS / sizeof ROWS[0] / 2)); return; }
  vg_clear(0x101214);
  char b[24];
  const char *label = st == ST_REC ? "REC" : st == ST_ARMED ? "ARMED" : st == ST_PLAY ? (ovd ? "OVERDUB" : "PLAY") : "STOP";
  uint32_t col = st == ST_REC ? 0xFF3030 : st == ST_ARMED ? 0xFFB020 : st == ST_PLAY ? (ovd ? 0xFF8040 : 0x40E070) : 0x909090;
  vg_text(label, 24, 24, VG_HUGE, col, VG_LEFT);
  vg_text(vg_ftoa(bpm, 0, b), 456, 24, VG_HUGE, 0xE0E0E0, VG_RIGHT);
  vg_text("BPM", 456, 90, VG_BOLD, 0x808080, VG_RIGHT);
  /* loop bar: 16 beats */
  int beat = (int)(pos / (loop_len / (BARS * 4)));
  if (beat > BARS * 4 - 1) beat = BARS * 4 - 1;
  int running = (st == ST_REC || st == ST_PLAY);
  for (int i = 0; i < BARS * 4; i++) {
    uint32_t c = (running && i == beat) ? col : (i % 4 == 0 ? 0x3A3F46 : 0x24282D);
    vg_rect(24 + i * 27, 150, 23, 30, c);
  }
  if (running) { vg_text("BAR", 24, 196, VG_BOLD, 0x808080, VG_LEFT); vg_text(vg_itoa(beat / 4 + 1, b), 64, 196, VG_BIG, 0xE0E0E0, VG_LEFT); }
  vg_text("EVENTS", 24, 236, VG_BOLD, 0x808080, VG_LEFT); vg_text(vg_itoa(nev, b), 90, 232, VG_BIG, 0xE0E0E0, VG_LEFT);
  vg_text("FILTER", 24, 272, VG_BOLD, 0x808080, VG_LEFT);
  vg_rect(90, 278, 200, 8, 0x24282D); vg_rect(90, 278, (int)(200.0f * cut_n), 8, 0x40B0FF);
  /* lights */
  for (int i = 0; i < 16; i++) {
    flash[i] *= 0.85f;
    uint32_t pc = i < 8 ? 0x2060FF : 0xB040FF;
    vibe_led_pad(i, flash[i] > 0.05f ? 0xFFFFFF : pc, flash[i] > 0.05f ? 1.0f : 0.15f);
  }
  vibe_led_button(VB_REC, 0xFF2020, (st == ST_REC || ovd) ? 1.0f : (st == ST_ARMED ? 0.5f : 0.0f));
  vibe_led_button(VB_PLAY, 0x20FF60, st == ST_PLAY ? 1.0f : 0.0f);
}
