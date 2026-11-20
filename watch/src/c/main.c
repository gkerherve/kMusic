// kMusic: a beautiful "now playing" screen for Pebble.
// The Android companion (../android) reads the phone's media session and sends
// title / artist / position / album art (already dithered to the watch's colours).
//   Up = previous, Select = play/pause, Down = next, long Select = volume mode.
#include <pebble.h>

// #define KMUSIC_DEMO 1   // fake song + generated cover, for emulator screenshots

enum { CMD_PLAYPAUSE = 1, CMD_NEXT = 2, CMD_PREV = 3, CMD_VOL_UP = 4, CMD_VOL_DOWN = 5, CMD_HELLO = 6, CMD_LIKE = 7, CMD_DISLIKE = 8 };
enum { OV_NONE, OV_PLAY, OV_PAUSE, OV_NEXT, OV_PREV, OV_LIKE, OV_DISLIKE, OV_INFO };

static Window *s_win;
static Layer *s_canvas;
static AppTimer *s_ov_timer, *s_vol_timer, *s_hello_timer;

static char s_title[72], s_artist[56], s_player[24];
static bool s_has_song, s_playing, s_vol_mode, s_rate_mode;
static int s_liked;               // 1 liked, -1 disliked, 0 none
static AppTimer *s_rate_timer;
static char s_info[320];          // album, year, genre, track n of m, player...
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

static void canvas_draw(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);
  int W = b.size.w, H = b.size.h, S = s_S;
  bool big = W >= 200;
  GFont f_title = fonts_get_system_font(big ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD);
  GFont f_artist = fonts_get_system_font(big ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14);
  GFont f_small = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GColor bg = bg_color();
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  GColor sub = PBL_IF_COLOR_ELSE(gcolor_equal(bg, GColorBlack) ? GColorLightGray : GColorWhite, GColorWhite);

  // ---- cover
  int cy = PBL_IF_ROUND_ELSE(H * 10 / 100, big ? 12 : 8);
  GRect cr = GRect((W - S) / 2, cy, S, S);
#if defined(PBL_COLOR)
  graphics_context_set_fill_color(ctx, GColorBlack);           // drop shadow
  graphics_fill_rect(ctx, GRect(cr.origin.x + 3, cr.origin.y + 4, S, S), 4, GCornersAll);
#endif
  if (s_art) {
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
    graphics_draw_bitmap_in_rect(ctx, s_art, cr);
#if !defined(PBL_COLOR)
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_rect(ctx, grect_inset(cr, GEdgeInsets(-2)));
#endif
  } else draw_note(ctx, cr);

  // ---- overlay glyph on the cover after a button press
  if (s_overlay) {
    GPoint c = grect_center_point(&cr);
    int r = S / 5;
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, c, r + 2);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, c, r);
    graphics_context_set_fill_color(ctx, GColorWhite);
    draw_icon(ctx, s_overlay, c, r);
  }

  // ---- title / artist
  int pad = PBL_IF_ROUND_ELSE(W / 8, 6), y = cy + S + (big ? 10 : 6);
  int tw = W - 2 * pad - PBL_IF_ROUND_ELSE(W / 8, big ? 18 : 14);   // leave room for the button icons
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_has_song ? s_title : "Nothing playing", f_title, GRect(pad, y - 4, tw, big ? 30 : 24),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += big ? 26 : 20;
  graphics_context_set_text_color(ctx, sub);
  graphics_draw_text(ctx, s_rate_mode ? "Like  -  Info  -  Dislike" : s_has_song ? s_artist : "Play music on your phone", f_artist, GRect(pad, y - 2, tw, big ? 24 : 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // ---- progress (bar on rectangles, rim arc on round) or volume
  int32_t pos = s_pos > s_dur && s_dur > 0 ? s_dur : s_pos;
  int pct = s_vol_mode ? (s_vol < 0 ? 50 : s_vol) : (s_dur > 0 ? (int)((int64_t)pos * 1000 / s_dur) / 10 : 0);
  char tl[12], tr[12];
  fmt_time(tl, sizeof(tl), pos); fmt_time(tr, sizeof(tr), s_dur);
  if (s_vol_mode) { snprintf(tl, sizeof(tl), "VOL"); snprintf(tr, sizeof(tr), "%d%%", s_vol < 0 ? 0 : s_vol); }
#if defined(PBL_ROUND)
  GRect rim = grect_inset(b, GEdgeInsets(3));
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorBlack, GColorDarkGray));
  graphics_fill_radial(ctx, rim, GOvalScaleModeFitCircle, 6, DEG_TO_TRIGANGLE(0), DEG_TO_TRIGANGLE(360));
  graphics_context_set_fill_color(ctx, s_vol_mode ? PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite) : GColorWhite);
  if (pct > 0) graphics_fill_radial(ctx, rim, GOvalScaleModeFitCircle, 6, 0, DEG_TO_TRIGANGLE(360 * pct / 100));
  if (s_has_song) {
    char both[28]; snprintf(both, sizeof(both), "%s  %s %s", tl, s_vol_mode ? "" : "/", tr);
    graphics_context_set_text_color(ctx, sub);
    graphics_draw_text(ctx, both, f_small, GRect(0, H - (big ? 44 : 32), W, 18), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
#else
  if (s_has_song) {
    int by = H - (big ? 22 : 16), tx = big ? 46 : 36;
    graphics_context_set_text_color(ctx, sub);
    graphics_draw_text(ctx, tl, f_small, GRect(2, by - 9, tx - 4, 18), GTextOverflowModeFill, GTextAlignmentRight, NULL);
    graphics_draw_text(ctx, tr, f_small, GRect(W - tx + 2, by - 9, tx - 4, 18), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    GRect track = GRect(tx, by - 1, W - 2 * tx, 4);
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorBlack, GColorDarkGray));
    graphics_fill_rect(ctx, track, 2, GCornersAll);
    graphics_context_set_fill_color(ctx, s_vol_mode ? PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite) : GColorWhite);
    int fw = track.size.w * pct / 100;
    if (fw > 0) graphics_fill_rect(ctx, GRect(track.origin.x, track.origin.y, fw, 4), 2, GCornersAll);
    if (!s_vol_mode) graphics_fill_circle(ctx, GPoint(track.origin.x + fw, track.origin.y + 1), 3);
  }
#endif
  // button hints at the right edge, next to the physical buttons
  {
    int hx = W - PBL_IF_ROUND_ELSE(W / 7, big ? 12 : 10), r = big ? 6 : 5;
    int ys[3] = { H * 30 / 100, H / 2, H * 70 / 100 };
    int kinds[3] = { OV_PREV, s_playing ? OV_PAUSE : OV_PLAY, OV_NEXT };
    if (s_rate_mode) { kinds[0] = OV_LIKE; kinds[1] = OV_INFO; kinds[2] = OV_DISLIKE; }
    graphics_context_set_fill_color(ctx, s_rate_mode ? GColorWhite : sub);
    if (s_rate_mode) r += 2;
    for (int i = 0; i < 3; i++) draw_icon(ctx, kinds[i], GPoint(hx, ys[i]), r * 2);
  }
  if (s_has_song && s_liked && !s_overlay) {        // liked / disliked badge, top-left of the cover
    GPoint c = GPoint(cr.origin.x + 12, cr.origin.y + 12);
    graphics_context_set_fill_color(ctx, s_liked > 0 ? PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite) : PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorWhite));
    graphics_fill_circle(ctx, c, 9);
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
    draw_icon(ctx, s_liked > 0 ? OV_LIKE : OV_DISLIKE, c, 8);
  }
  // little play-state dot under the cover when paused
  if (s_has_song && !s_playing && !s_overlay) {
    GPoint c = GPoint(cr.origin.x + S - 12, cr.origin.y + S - 12);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, c, 9);
    graphics_context_set_fill_color(ctx, GColorWhite);
    draw_icon(ctx, OV_PAUSE, c, 7);
  }
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
  if (s_vol_timer) app_timer_reschedule(s_vol_timer, 1500); else s_vol_timer = app_timer_register(1500, vol_end, NULL);
  layer_mark_dirty(s_canvas);
}
// Up = previous, Select = play/pause, Down = next.
// Hold Up / Down = volume up / down (keeps going while held).
static AppTimer *s_hold_timer;
static int s_hold_cmd;
static void vol_step(void) {
  send_cmd(s_hold_cmd);
  if (s_vol >= 0) { s_vol += s_hold_cmd == CMD_VOL_UP ? 7 : -7; if (s_vol > 100) s_vol = 100; if (s_vol < 0) s_vol = 0; }
  vol_touch();
}
static void hold_tick(void *d) { vol_step(); s_hold_timer = app_timer_register(250, hold_tick, NULL); }
static void hold_start(ClickRecognizerRef r, void *c) {
  if (s_rate_mode) return;
  s_hold_cmd = click_recognizer_get_button_id(r) == BUTTON_ID_UP ? CMD_VOL_UP : CMD_VOL_DOWN;
  vibes_short_pulse();
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
  text_layer_set_font(s_info_text, ft); text_layer_set_text(s_info_text, s_info[0] ? s_info : "No details from the player.");
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
  if (s_playing && s_has_song && s_dur > 0) { s_pos += 1000; if (s_pos > s_dur) s_pos = s_dur; layer_mark_dirty(s_canvas); }
}

#ifdef KMUSIC_DEMO
static void demo(void) {
  s_has_song = true; s_playing = true; s_pos = 83000; s_dur = 214000; s_vol = 60;
  s_liked = 1; strcpy(s_title, "Midnight Orchard");
  strcpy(s_info, "The Paper Lanterns\nAlbum: Lantern Light (2024)\nGenre: Indie folk\nTrack 3 of 11\n\nPlaying on YouTube Music\nVolume 60%"); strcpy(s_artist, "The Paper Lanterns");
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
  s_S = PBL_IF_ROUND_ELSE(b.size.w * 45 / 100, b.size.w * 62 / 100) & ~3;   // 88 / 124 / 88 / 128
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_draw);
  layer_add_child(root, s_canvas);
}
static void win_unload(Window *w) { layer_destroy(s_canvas); }

int main(void) {
  s_accent = PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorBlack);
  s_info_win = window_create();
  window_set_window_handlers(s_info_win, (WindowHandlers){ .load = info_load, .unload = info_unload });
  s_win = window_create();
  window_set_click_config_provider(s_win, click_config);
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
#endif
  app_event_loop();
  if (s_art) gbitmap_destroy(s_art);
  if (s_art_new) gbitmap_destroy(s_art_new);
  window_destroy(s_win);
  window_destroy(s_info_win);
}
