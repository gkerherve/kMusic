// kMusic: a beautiful "now playing" screen for Pebble.
// The Android companion (../android) reads the phone's media session and sends
// title / artist / position / album art (already dithered to the watch's colours).
//   Up = previous, Select = play/pause, Down = next, long Select = volume mode.
#include <pebble.h>

// #define KMUSIC_DEMO 1   // fake song + generated cover, for emulator screenshots

enum { CMD_PLAYPAUSE = 1, CMD_NEXT = 2, CMD_PREV = 3, CMD_VOL_UP = 4, CMD_VOL_DOWN = 5, CMD_HELLO = 6, CMD_LIKE = 7, CMD_DISLIKE = 8 };
enum { OV_NONE, OV_PLAY, OV_PAUSE, OV_NEXT, OV_PREV, OV_LIKE, OV_DISLIKE, OV_INFO, OV_PLUS, OV_MINUS };

static Window *s_win;
static Layer *s_canvas;
static AppTimer *s_ov_timer, *s_vol_timer, *s_hello_timer;

static char s_title[72], s_artist[56], s_player[24];
static bool s_has_song, s_playing, s_vol_mode, s_rate_mode;
static int s_liked;               // 1 liked, -1 disliked, 0 none
static AppTimer *s_rate_timer;
static char s_info[600];
static bool s_phone_ok, s_waited;   // companion answered? / waited long enough to say so
static void waited(void *d);          // album, year, genre, track n of m, player...
static Window *s_info_win;
static ScrollLayer *s_info_scroll;
static TextLayer *s_info_head, *s_info_text;
static int32_t s_pos, s_dur;      // ms
static int s_vol = -1;            // 0..100, -1 unknown
static int s_overlay;
static GColor s_accent;

static int s_S;                   // cover side in px
static GBitmap *s_art, *s_art_new;
static int s_art_id = -1, s_art_len, s_art_got;

// ------------------------------------------------------------------ helpers
static GColor bg_color(void) {
#if defined(PBL_COLOR)
  return s_has_song ? s_accent : GColorBlack;
#else
  return GColorBlack;
#endif
}
static void fmt_time(char *b, size_t n, int32_t ms) {
  int s = ms / 1000; if (s < 0) s = 0;
  if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(b, n, "%d:%02d", s / 60, s % 60);
}
static void send_cmd(int cmd) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_int32(it, MESSAGE_KEY_CMD, cmd);
  if (cmd == CMD_HELLO) {           // tell the phone what cover to make
    GBitmap *t = gbitmap_create_blank(GSize(s_S, s_S), PBL_IF_COLOR_ELSE(GBitmapFormat8Bit, GBitmapFormat1Bit));
    dict_write_int32(it, MESSAGE_KEY_ART_W, s_S);
    dict_write_int32(it, MESSAGE_KEY_ART_ROW, t ? gbitmap_get_bytes_per_row(t) : 0);
    dict_write_int32(it, MESSAGE_KEY_COLOR, PBL_IF_COLOR_ELSE(1, 0));
    if (t) gbitmap_destroy(t);
  }
  app_message_outbox_send();
}
static void hello_retry(void *d) { s_hello_timer = NULL; if (!s_has_song) { send_cmd(CMD_HELLO); s_hello_timer = app_timer_register(3000, hello_retry, NULL); } }

// ------------------------------------------------------------------ drawing
static void fill_tri(GContext *ctx, GPoint a, GPoint b, GPoint c) {
  GPoint pts[3] = { a, b, c };
  GPathInfo info = { 3, pts };
  GPath *p = gpath_create(&info);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}
static void draw_icon(GContext *ctx, int kind, GPoint c, int r) {   // white glyph in a circle of radius r
  int u = r / 2;
  switch (kind) {
    case OV_PLAY: fill_tri(ctx, GPoint(c.x - u / 2 - 1, c.y - u), GPoint(c.x - u / 2 - 1, c.y + u), GPoint(c.x + u, c.y)); break;
    case OV_PAUSE:
      graphics_fill_rect(ctx, GRect(c.x - u + 1, c.y - u, u * 2 / 3 + 1, u * 2), 1, GCornersAll);
      graphics_fill_rect(ctx, GRect(c.x + u / 3, c.y - u, u * 2 / 3 + 1, u * 2), 1, GCornersAll);
      break;
    case OV_NEXT: case OV_PREV: {
      int d = kind == OV_NEXT ? 1 : -1;
      fill_tri(ctx, GPoint(c.x - d * u, c.y - u * 3 / 4), GPoint(c.x - d * u, c.y + u * 3 / 4), GPoint(c.x, c.y));
      fill_tri(ctx, GPoint(c.x, c.y - u * 3 / 4), GPoint(c.x, c.y + u * 3 / 4), GPoint(c.x + d * u, c.y));
      graphics_fill_rect(ctx, GRect(d > 0 ? c.x + u : c.x - u - 2, c.y - u * 3 / 4, 3, u * 3 / 2 + 1), 0, GCornerNone);
      break;
    }
    case OV_LIKE: case OV_DISLIKE: {          // thumb: palm + cuff + thumb (flipped for dislike)
      int f = kind == OV_LIKE ? 1 : -1, h = u * 5 / 4;
      GRect palm = GRect(c.x - u / 2, c.y - h / 2 + f * u / 4, u * 3 / 2, h);
      graphics_fill_rect(ctx, palm, 2, GCornersAll);
      graphics_fill_rect(ctx, GRect(c.x - u, palm.origin.y, u / 2, h), 1, GCornersAll);
      int th = u + 1;
      graphics_fill_rect(ctx, GRect(c.x - u / 2, f > 0 ? palm.origin.y - th + 2 : palm.origin.y + h - 2, u * 2 / 3 + 1, th), 2, GCornersAll);
      break;
    }
    case OV_PLUS:
      graphics_fill_rect(ctx, GRect(c.x - 1, c.y - u + 1, 3, u * 2 - 1), 0, GCornerNone);
      /* fallthrough */
    case OV_MINUS:
      graphics_fill_rect(ctx, GRect(c.x - u + 1, c.y - 1, u * 2 - 1, 3), 0, GCornerNone);
      break;
    case OV_INFO:
      graphics_fill_circle(ctx, GPoint(c.x, c.y - u * 3 / 4), u / 4 + 1);
      graphics_fill_rect(ctx, GRect(c.x - u / 4, c.y - u / 3, u / 2 + 1, u + u / 3), 1, GCornersAll);
      break;
  }
}
static void draw_note(GContext *ctx, GRect r) {   // placeholder cover: a music note on a soft tile
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
  graphics_fill_rect(ctx, r, 6, GCornersAll);
  GColor fg = PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack);
  graphics_context_set_fill_color(ctx, fg);
  int s = r.size.w, x = r.origin.x, y = r.origin.y;
  graphics_fill_circle(ctx, GPoint(x + s * 36 / 100, y + s * 68 / 100), s / 10);
  graphics_fill_circle(ctx, GPoint(x + s * 66 / 100, y + s * 60 / 100), s / 10);
  graphics_fill_rect(ctx, GRect(x + s * 36 / 100 + s / 10 - 3, y + s * 26 / 100, 3, s * 42 / 100), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(x + s * 66 / 100 + s / 10 - 3, y + s * 18 / 100, 3, s * 42 / 100), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(x + s * 36 / 100 + s / 10 - 3, y + s * 18 / 100, s * 30 / 100 + 3, s / 12 + 1), 0, GCornerNone);
}

// ---- action bar (like Pebble's own Music app): icons follow the current mode
static ActionBarLayer *s_bar;
static GBitmap *s_ic_play, *s_ic_pause, *s_ic_next, *s_ic_prev, *s_ic_vup, *s_ic_vdn, *s_ic_like, *s_ic_dislike, *s_ic_info;
static void update_icons(void) {
  if (!s_bar) return;
  GBitmap *up = s_ic_prev, *mid = s_playing ? s_ic_pause : s_ic_play, *dn = s_ic_next;
  if (s_vol_mode) { up = s_ic_vup; dn = s_ic_vdn; }
  if (s_rate_mode) { up = s_ic_like; mid = s_ic_info; dn = s_ic_dislike; }
  static GBitmap *last[3];                      // only touch the bar when an icon really changes
  GBitmap *now[3] = { up, mid, dn };
  ButtonId ids[3] = { BUTTON_ID_UP, BUTTON_ID_SELECT, BUTTON_ID_DOWN };
  for (int i = 0; i < 3; i++) if (now[i] != last[i]) { action_bar_layer_set_icon_animated(s_bar, ids[i], now[i], last[i] != NULL); last[i] = now[i]; }
}

static void pill(GContext *ctx, const char *txt, GFont f, GRect r, GTextAlignment al) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, r, r.size.h / 2, GCornersAll);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, txt, f, GRect(r.origin.x + 2, r.origin.y - (r.size.h > 18 ? 3 : 2), r.size.w - 4, r.size.h), GTextOverflowModeFill, al, NULL);
}

static void canvas_draw(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);
  int W = b.size.w, H = b.size.h, S = s_S;
  bool big = W >= 200;
  GColor bg = bg_color();
  (void)H;
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // ---- cover: beside the action bar (rectangles) or the whole dial (round)
  GRect cr = GRect(0, 0, S, S);
  if (s_art) {
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
    graphics_draw_bitmap_in_rect(ctx, s_art, cr);
  } else draw_note(ctx, GRect(S / 4, S / 4 - PBL_IF_ROUND_ELSE(S / 10, 0), S / 2, S / 2));

  // ---- text
  int32_t pos = s_pos > s_dur && s_dur > 0 ? s_dur : s_pos;
  int pct = s_vol_mode ? (s_vol < 0 ? 0 : s_vol) : (s_dur > 0 ? (int)((int64_t)pos * 100 / s_dur) : 0);
  const char *l1 = s_has_song ? s_title : s_phone_ok ? "Nothing playing" : s_waited ? "Get the companion" : "Connecting...";
  const char *l2 = s_has_song ? s_artist : s_phone_ok ? "Play music on your phone" : s_waited ? "github.com/gkerherve/kMusic" : "";
  static char vb[24];
  if (s_vol_mode) { snprintf(vb, sizeof(vb), "Volume %d%%", s_vol < 0 ? 0 : s_vol); l2 = vb; }
  if (s_rate_mode) l2 = "Like, info or dislike";
  GColor barc = s_vol_mode ? PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite) : GColorWhite;
  GColor sub = PBL_IF_COLOR_ELSE(s_vol_mode || s_rate_mode ? GColorChromeYellow : GColorLightGray, GColorWhite);
  GFont f1 = fonts_get_system_font(big ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD);
  GFont f2 = fonts_get_system_font(big ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14);

#if defined(PBL_ROUND)
  int band = H * 70 / 100, tx = W / 6, tw = W - ACTION_BAR_WIDTH - tx;
  if (!s_has_song && !s_phone_ok && s_waited) { l1 = "App on GitHub:"; l2 = "gkerherve/kMusic"; }
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, GRect(0, band, W, H - band), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, l1, f1, GRect(tx, band - 2, tw, big ? 30 : 24), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, sub);
  graphics_draw_text(ctx, l2, f2, GRect(tx, band + (big ? 24 : 18), tw, 20), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  GRect rim = grect_inset(b, GEdgeInsets(2));                                   // progress ring on the rim
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_radial(ctx, rim, GOvalScaleModeFitCircle, 6, 0, DEG_TO_TRIGANGLE(360));
  graphics_context_set_fill_color(ctx, barc);
  if (s_has_song && pct > 0) graphics_fill_radial(ctx, rim, GOvalScaleModeFitCircle, 6, 0, DEG_TO_TRIGANGLE(360 * pct / 100));
#else
  // progress bar under the cover, then title and artist
  int y = S + 3;
  if (s_has_song || s_vol_mode) {
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorBlack, GColorDarkGray));
    graphics_fill_rect(ctx, GRect(4, y, S - 8, 3), 1, GCornersAll);
    graphics_context_set_fill_color(ctx, barc);
    if (pct > 0) graphics_fill_rect(ctx, GRect(4, y, (S - 8) * pct / 100, 3), 1, GCornersAll);
  }
  y += big ? 2 : 1;
  if (!s_has_song && !s_phone_ok && s_waited) {          // no companion yet: say where to get it
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "Phone app on GitHub:\ngkerherve/kMusic", fonts_get_system_font(big ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14),
                       GRect(4, y - 2, S - 6, H - y), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    update_icons();
    return;
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, l1, f1, GRect(4, y, S - 6, big ? 30 : 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  y += big ? 26 : 20;
  graphics_context_set_text_color(ctx, sub);
  graphics_draw_text(ctx, l2, f2, GRect(4, y, S - 6, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
#endif

  // ---- time and volume pills on the bottom of the cover
  if (s_has_song) {
    static char tb[24], vb2[12];
    int ps = pos / 1000, ds = s_dur / 1000;
    if (s_dur > 0) snprintf(tb, sizeof(tb), "%d:%02d / %d:%02d", ps / 60, ps % 60, ds / 60, ds % 60);
    else snprintf(tb, sizeof(tb), "%d:%02d", ps / 60, ps % 60);
    if (s_vol >= 0) snprintf(vb2, sizeof(vb2), big || PBL_IF_ROUND_ELSE(true, false) ? "Vol %d%%" : "%d%%", s_vol); else vb2[0] = 0;
    GFont pf = fonts_get_system_font(big ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD);
    int ph = big ? 22 : 17;
#if defined(PBL_ROUND)
    int py = band - ph - 3;
    pill(ctx, tb, pf, GRect(W / 2 - 60, py, 120, ph), GTextAlignmentCenter);
    if (vb2[0]) pill(ctx, vb2, pf, GRect(W / 2 - 34, py - ph - 3, 68, ph), GTextAlignmentCenter);
#else
    int py = S - ph - 3;
    int tw2 = s_dur > 0 ? (big ? 104 : 72) : (big ? 48 : 36);
    pill(ctx, tb, pf, GRect(3, py, tw2, ph), GTextAlignmentCenter);
    int vw = big ? 70 : 36;
    if (vb2[0]) pill(ctx, vb2, pf, GRect(S - vw - 3, py, vw, ph), GTextAlignmentCenter);
#endif
  }

  // ---- liked / disliked badge, top-left of the cover
  if (s_has_song && s_liked && !s_overlay) {
    int rr = big ? 13 : 10;
    GPoint c = GPoint(PBL_IF_ROUND_ELSE(W / 4, rr + 5), PBL_IF_ROUND_ELSE(H / 4, rr + 5));
    graphics_context_set_fill_color(ctx, s_liked > 0 ? PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite) : PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorWhite));
    graphics_fill_circle(ctx, c, rr);
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
    draw_icon(ctx, s_liked > 0 ? OV_LIKE : OV_DISLIKE, c, rr - 1);
  }

  // ---- big glyph in the middle of the cover: after a press, or while paused
  int ov = s_overlay ? s_overlay : (s_has_song && !s_playing ? OV_PAUSE : OV_NONE);
  if (ov) {
    GPoint c = GPoint(PBL_IF_ROUND_ELSE((W - ACTION_BAR_WIDTH / 2) / 2, S / 2), PBL_IF_ROUND_ELSE(H * 38 / 100, S / 2));
    int r = s_overlay ? S / 6 : S / 9;
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, c, r + 2);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, c, r);
    graphics_context_set_fill_color(ctx, GColorWhite);
    draw_icon(ctx, ov, c, r);
  }
  update_icons();
}

// ------------------------------------------------------------------ buttons
static void ov_clear(void *d) { s_ov_timer = NULL; s_overlay = OV_NONE; layer_mark_dirty(s_canvas); }
static void show_overlay(int k) {
  s_overlay = k;
  if (s_ov_timer) app_timer_reschedule(s_ov_timer, 900); else s_ov_timer = app_timer_register(900, ov_clear, NULL);
  layer_mark_dirty(s_canvas);
}
static void vol_end(void *d) { s_vol_timer = NULL; s_vol_mode = false; layer_mark_dirty(s_canvas); }
static void vol_touch(void) {
  s_vol_mode = true;
  if (s_vol_timer) app_timer_reschedule(s_vol_timer, 2000); else s_vol_timer = app_timer_register(2000, vol_end, NULL);
  layer_mark_dirty(s_canvas);
}
// Up = previous, Select = play/pause, Down = next.
// Hold Up / Down = volume up / down (keeps going while held).
static AppTimer *s_hold_timer;
static int s_hold_cmd;
static void vol_step(void) {
  send_cmd(s_hold_cmd);     // the phone answers with the real volume
  vol_touch();
}
static void hold_tick(void *d) { vol_step(); s_hold_timer = app_timer_register(350, hold_tick, NULL); }
static void hold_start(ClickRecognizerRef r, void *c) {
  if (s_rate_mode) return;
  s_hold_cmd = click_recognizer_get_button_id(r) == BUTTON_ID_UP ? CMD_VOL_UP : CMD_VOL_DOWN;
  hold_tick(NULL);
}
static void hold_end(ClickRecognizerRef r, void *c) { if (s_hold_timer) { app_timer_cancel(s_hold_timer); s_hold_timer = NULL; } }
// ---- song info page
static void info_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  window_set_background_color(w, bg_color());
  int pad = PBL_IF_ROUND_ELSE(b.size.w / 8, 6), tw = b.size.w - 2 * pad, y = PBL_IF_ROUND_ELSE(24, 4);
  s_info_scroll = scroll_layer_create(b);
  scroll_layer_set_click_config_onto_window(s_info_scroll, w);
  GFont fh = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), ft = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GSize hs = graphics_text_layout_get_content_size(s_title, fh, GRect(0, 0, tw, 400), GTextOverflowModeWordWrap, GTextAlignmentLeft);
  s_info_head = text_layer_create(GRect(pad, y, tw, hs.h + 6)); y += hs.h + 8;
  GSize ts = graphics_text_layout_get_content_size(s_info, ft, GRect(0, 0, tw, 2000), GTextOverflowModeWordWrap, GTextAlignmentLeft);
  s_info_text = text_layer_create(GRect(pad, y, tw, ts.h + 6)); y += ts.h + PBL_IF_ROUND_ELSE(50, 12);
  TextLayer *ls[2] = { s_info_head, s_info_text };
  for (int i = 0; i < 2; i++) {
    text_layer_set_background_color(ls[i], GColorClear);
    text_layer_set_text_color(ls[i], GColorWhite);
    text_layer_set_text_alignment(ls[i], PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
    scroll_layer_add_child(s_info_scroll, text_layer_get_layer(ls[i]));
  }
  text_layer_set_font(s_info_head, fh); text_layer_set_text(s_info_head, s_has_song ? s_title : "Nothing playing");
  text_layer_set_font(s_info_text, ft); text_layer_set_text(s_info_text, s_info[0] ? s_info : s_phone_ok ? "No details from the player." :
    "kMusic needs its free Android companion app. Download it from:\n\ngithub.com/gkerherve/kMusic\n\nInstall it, follow its setup steps, then play some music.");
  scroll_layer_set_content_size(s_info_scroll, GSize(b.size.w, y));
  layer_add_child(root, scroll_layer_get_layer(s_info_scroll));
}
static void info_unload(Window *w) {
  text_layer_destroy(s_info_head); text_layer_destroy(s_info_text); scroll_layer_destroy(s_info_scroll);
}

// Hold Select = menu: Up = like, Select = song info, Down = dislike.
static void rate_end(void *d) { s_rate_timer = NULL; s_rate_mode = false; layer_mark_dirty(s_canvas); }
static void rate_stop(void) { if (s_rate_timer) app_timer_cancel(s_rate_timer); rate_end(NULL); }
static void select_long(ClickRecognizerRef r, void *c) {
  s_rate_mode = true; vibes_short_pulse();
  if (s_rate_timer) app_timer_reschedule(s_rate_timer, 4000); else s_rate_timer = app_timer_register(4000, rate_end, NULL);
  layer_mark_dirty(s_canvas);
}
static void rate(int v) {
  rate_stop();
  s_liked = s_liked == v ? 0 : v;           // pressing again removes the like / dislike
  send_cmd(v > 0 ? CMD_LIKE : CMD_DISLIKE);
  show_overlay(v > 0 ? OV_LIKE : OV_DISLIKE);
  vibes_short_pulse();
}
static void up_click(ClickRecognizerRef r, void *c) { if (s_rate_mode) { rate(1); return; } send_cmd(CMD_PREV); show_overlay(OV_PREV); }
static void down_click(ClickRecognizerRef r, void *c) { if (s_rate_mode) { rate(-1); return; } send_cmd(CMD_NEXT); show_overlay(OV_NEXT); }
static void select_click(ClickRecognizerRef r, void *c) {
  if (s_rate_mode) { rate_stop(); window_stack_push(s_info_win, true); return; }
  s_playing = !s_playing;
  send_cmd(CMD_PLAYPAUSE); show_overlay(s_playing ? OV_PLAY : OV_PAUSE);
}
static void click_config(void *c) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_UP, 400, hold_start, hold_end);
  window_long_click_subscribe(BUTTON_ID_DOWN, 400, hold_start, hold_end);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
}

// ------------------------------------------------------------------ phone
static void copy_str(DictionaryIterator *it, uint32_t key, char *dst, size_t n) {
  Tuple *t = dict_find(it, key);
  if (t) { strncpy(dst, t->value->cstring, n - 1); dst[n - 1] = 0; }
}
static void inbox(DictionaryIterator *it, void *ctx) {
  s_phone_ok = true;
  Tuple *t;
  if (dict_find(it, MESSAGE_KEY_TITLE)) {
    copy_str(it, MESSAGE_KEY_TITLE, s_title, sizeof(s_title));
    s_has_song = s_title[0] != 0;
    copy_str(it, MESSAGE_KEY_ARTIST, s_artist, sizeof(s_artist));
    copy_str(it, MESSAGE_KEY_PLAYER, s_player, sizeof(s_player));
  }
  if ((t = dict_find(it, MESSAGE_KEY_LIKED))) s_liked = t->value->int32;
  copy_str(it, MESSAGE_KEY_INFO, s_info, sizeof(s_info));
  if ((t = dict_find(it, MESSAGE_KEY_STATE))) s_playing = t->value->int32 != 0;
  if ((t = dict_find(it, MESSAGE_KEY_POS))) s_pos = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_DUR))) s_dur = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_VOL))) s_vol = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_ACCENT))) s_accent = (GColor){ .argb = (uint8_t)t->value->int32 };

  Tuple *id = dict_find(it, MESSAGE_KEY_ART_ID);
  if (id) {
    Tuple *len = dict_find(it, MESSAGE_KEY_ART_LEN), *off = dict_find(it, MESSAGE_KEY_ART_OFF), *dat = dict_find(it, MESSAGE_KEY_ART_DATA);
    if (len) {                                   // header: start a new cover (0 = no cover)
      if (s_art_new) { gbitmap_destroy(s_art_new); s_art_new = NULL; }
      if (s_art) { gbitmap_destroy(s_art); s_art = NULL; }
      s_art_id = id->value->int32; s_art_len = len->value->int32; s_art_got = 0;
      if (s_art_len == 0) { if (s_art) { gbitmap_destroy(s_art); s_art = NULL; } }
      else {
        s_art_new = gbitmap_create_blank(GSize(s_S, s_S), PBL_IF_COLOR_ELSE(GBitmapFormat8Bit, GBitmapFormat1Bit));
        if (s_art_new && gbitmap_get_bytes_per_row(s_art_new) * s_S != s_art_len) { gbitmap_destroy(s_art_new); s_art_new = NULL; }
      }
    } else if (off && dat && s_art_new && id->value->int32 == s_art_id) {
      int o = off->value->int32, n = dat->length;
      if (o >= 0 && o + n <= s_art_len) {
        memcpy(gbitmap_get_data(s_art_new) + o, dat->value->data, n);
        s_art_got += n;
        if (s_art_got >= s_art_len) {             // complete: swap in
          if (s_art) gbitmap_destroy(s_art);
          s_art = s_art_new; s_art_new = NULL;
        }
      }
    }
  }
  layer_mark_dirty(s_canvas);
}
static void tick(struct tm *t, TimeUnits u) {
  if (s_playing && s_has_song && s_dur > 0) { s_pos += 1000; if (s_pos > s_dur) s_pos = s_dur; }
  if (s_has_song) layer_mark_dirty(s_canvas);
}

#ifdef KMUSIC_DEMO
static void demo(void) {
  s_has_song = true; s_playing = true; s_pos = 83000; s_dur = 214000; s_vol = 60;
  s_liked = 1; strcpy(s_title, "Midnight Orchard");
  strcpy(s_info, "The Paper Lanterns\nAlbum: Lantern Light (2024)\nLength: 3:34\n\nUp next:\n1. Harbour Lights - Ada Vale\n2. Slow Comet - The Paper Lanterns\n3. Northbound - Kite Season\n\nPlaying on YouTube Music\nVolume 60%"); strcpy(s_artist, "The Paper Lanterns");
  s_accent = PBL_IF_COLOR_ELSE(GColorImperialPurple, GColorBlack);
  s_art = gbitmap_create_blank(GSize(s_S, s_S), PBL_IF_COLOR_ELSE(GBitmapFormat8Bit, GBitmapFormat1Bit));
  uint8_t *d = gbitmap_get_data(s_art); int row = gbitmap_get_bytes_per_row(s_art);
  for (int y = 0; y < s_S; y++) for (int x = 0; x < s_S; x++) {
    int dx = x - s_S * 2 / 3, dy = y - s_S / 3, sun = dx * dx + dy * dy < s_S * s_S / 30;
    int hill = y > s_S * 6 / 10 + (x * x) / (s_S * 4) - x / 6;
#if defined(PBL_COLOR)
    int t = y * 4 / s_S;                               // sky gradient: blue -> purple -> pink
    uint8_t sky[4] = { 0xC6, 0xD6, 0xE6, 0xF6 };
    uint8_t c = sun ? 0xFC : hill ? ((x + y) % 5 == 0 ? 0xC4 : 0xC8) : sky[t];
    if (!sun && !hill && ((x * 7 + y * 13) % 97 == 0)) c = 0xFF;   // stars
    d[y * row + x] = c;
#else
    int on = sun ? 1 : hill ? 0 : ((x + y) % 2 == 0 && y > s_S / 3);
    if (on) d[y * row + x / 8] |= 1 << (x % 8);
#endif
  }
}
#endif

// ------------------------------------------------------------------ app
static void win_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_S = PBL_IF_ROUND_ELSE(b.size.w, b.size.w - ACTION_BAR_WIDTH);   // cover fills the space beside the action bar
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_draw);
  layer_add_child(root, s_canvas);
  s_ic_play = gbitmap_create_with_resource(RESOURCE_ID_IC_PLAY);
  s_ic_pause = gbitmap_create_with_resource(RESOURCE_ID_IC_PAUSE);
  s_ic_next = gbitmap_create_with_resource(RESOURCE_ID_IC_NEXT);
  s_ic_prev = gbitmap_create_with_resource(RESOURCE_ID_IC_PREV);
  s_ic_vup = gbitmap_create_with_resource(RESOURCE_ID_IC_VOL_UP);
  s_ic_vdn = gbitmap_create_with_resource(RESOURCE_ID_IC_VOL_DOWN);
  s_ic_like = gbitmap_create_with_resource(RESOURCE_ID_IC_LIKE);
  s_ic_dislike = gbitmap_create_with_resource(RESOURCE_ID_IC_DISLIKE);
  s_ic_info = gbitmap_create_with_resource(RESOURCE_ID_IC_INFO);
  s_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_bar, GColorBlack);
  action_bar_layer_set_click_config_provider(s_bar, click_config);
  action_bar_layer_add_to_window(s_bar, w);
  update_icons();
}
static void win_unload(Window *w) {
  action_bar_layer_destroy(s_bar); s_bar = NULL;   // icons cached in update_icons are re-set on next load
  GBitmap *ics[9] = { s_ic_play, s_ic_pause, s_ic_next, s_ic_prev, s_ic_vup, s_ic_vdn, s_ic_like, s_ic_dislike, s_ic_info };
  for (int i = 0; i < 9; i++) gbitmap_destroy(ics[i]);
  layer_destroy(s_canvas);
}

static void waited(void *d) { s_waited = true; if (s_canvas) layer_mark_dirty(s_canvas); }

int main(void) {
  s_accent = PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorBlack);
  s_info_win = window_create();
  window_set_window_handlers(s_info_win, (WindowHandlers){ .load = info_load, .unload = info_unload });
  s_win = window_create();
  window_set_window_handlers(s_win, (WindowHandlers){ .load = win_load, .unload = win_unload });
  window_stack_push(s_win, true);
  app_message_register_inbox_received(inbox);
  uint32_t in = app_message_inbox_size_maximum();
  app_message_open(in > 4096 ? 4096 : in, 256);
  tick_timer_service_subscribe(SECOND_UNIT, tick);
#ifdef KMUSIC_DEMO
  demo();
#else
  s_hello_timer = app_timer_register(300, hello_retry, NULL);
  app_timer_register(6000, waited, NULL);
#endif
  app_event_loop();
  if (s_art) gbitmap_destroy(s_art);
  if (s_art_new) gbitmap_destroy(s_art_new);
  window_destroy(s_win);
  window_destroy(s_info_win);
}
