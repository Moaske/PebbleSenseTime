/**
 * HTC Sense-style Flip Clock for Pebble Time 2 (emery)
 *
 * - TWO flip tiles, matching the original HTC Sense widget: an "HH" tile
 *   (both hour digits together) and an "MM" tile (both minute digits
 *   together) - not one flap per digit. Only the tile whose value actually
 *   changed animates (normally just MM; HH only flips on the hour).
 * - Each tile is split by its horizontal crease into an upper flap and a
 *   lower flap, and animates in two sequential steps: the upper flap
 *   shrinks UP INTO the crease (collapsing to nothing), the value swaps at
 *   that hidden instant, then the lower flap grows back DOWN OUT OF the
 *   crease to reveal the new value - not a whole-tile squash. See the FLIP
 *   TILE DRAWING / FLIP ANIMATION DRIVER sections below for the mechanics.
 * - A flick of the wrist (accelerometer tap) re-plays the flip animation
 *   on both tiles, snapping back to the current time.
 * - A weather strip overlays the bottom of the tile row: location name
 *   (left of icon), weather icon (center), temperature (right of icon) -
 *   fed by a PebbleKit JS companion using the Open-Meteo API.
 * - A full-screen background wallpaper (one of 5 HTC_WALLPAPER0N bitmaps)
 *   sits behind everything else, including the housing panel - see the
 *   BACKGROUND WALLPAPER section below.
 * - A stats panel below the housing: three icon+text rows (today's
 *   high/low, steps + sleep, watch + phone battery) separated by two 1px
 *   divider lines - see the STATS PANEL section below.
 * - Units, location, and the background wallpaper are user-configurable via
 *   a Clay settings page.
 *
 * Battery-conscious: the clock only redraws on MINUTE_UNIT ticks or a
 * detected tap; the flip animation timer runs for well under a second
 * and then stops itself.
 *
 * IMPLEMENTATION NOTE on the animation technique: an earlier version of
 * this file tried to reveal just the top/bottom HALF of a digit glyph by
 * shrinking the `box` GRect passed to graphics_draw_text(). Pebble's text
 * renderer clips at whole-line granularity ("clipping may occur if the
 * vertical space cannot accommodate the first line of text" - Pebble SDK
 * docs), not pixel-by-pixel, so a half-height text box does not reliably
 * produce a half-visible glyph. This version never varies the SIZE of a
 * text box: text is always drawn with a box at least as tall as the font
 * needs (the same way any ordinary digital clock renders on Pebble). The
 * actual reveal comes entirely from resizing a surrounding Layer's own
 * GRect frame via layer_set_frame() - Pebble layers are guaranteed to clip
 * their contents to their current bounds at any size, a different and more
 * fundamental mechanism than graphics_draw_text()'s own box. Each tile now
 * uses TWO such layers (top_layer, bottom_layer), one per flap, instead of
 * a single whole-tile layer - see FLIP TILE DRAWING below.
 *
 * FONT: digits are drawn with graphics_draw_text() (real font rendering,
 * not hand-drawn segments). Currently trying the custom FONT_SEGOEUISL_70
 * font resource (Segoe UI Semilight at 70px) in place of the earlier
 * SonySketchEF - loaded via fonts_load_custom_font() in main_window_load()
 * and released via fonts_unload_custom_font() in main_window_unload(). The
 * SonySketchEF resource (SONY_SKETCH_70) is still declared in package.json
 * so switching back is a one-line change in main_window_load() if this new
 * font doesn't work out. Segoe UI's glyph metrics are unknown here (no way
 * to render/measure fonts in this sandbox), so the TILE_W/TILE_H/
 * DIGIT_Y_NUDGE tuning below is still whatever fit SonySketchEF - expect it
 * may need a fresh pass once this is seen on real hardware.
 *
 * HOUSING: a single panel sits behind the tiles + weather strip, echoing
 * the dark "widget housing" of the original HTC design. It's drawn from
 * the CLOCK_ISLAND bitmap resource (a real ~66%-opacity black PNG) via a
 * BitmapLayer with GCompOpSet compositing - unlike a plain shape fill,
 * bitmap compositing on Pebble genuinely respects per-pixel alpha, so this
 * is real translucency, not an approximation. See the HOUSING PANEL
 * section below for the exact size the PNG needs to be.
 */

#include <pebble.h>

// ============================================================================
// LAYOUT CONSTANTS (emery: 200x228)
// ============================================================================

#define TILE_Y 20        // reverted to 20 - the 5px move was meant for the digits within
                          // the tile, not the tile/weather module itself, see DIGIT_Y_NUDGE
#define TILE_W 83        // the max that fits two side-by-side tiles + TILE_GAP on Emery's
                          // 200px width, see note below (unchanged - width wasn't the issue)
#define TILE_H 75        // shrunk by 8px (was 83) - SonySketchEF renders flatter/wider than
                          // it is tall, so the tile no longer needs to be as tall to hold it
#define TILE_GAP 6       // the gap between the HH and MM tiles - brought in 4px (was 10) to
                          // pull the two tiles closer together; since content_w (and so x0)
                          // is centered off this value, both tiles shift 2px toward the
                          // center automatically, staying nicely centered as a pair

#define TILE_RADIUS 6     // corner rounding on each flap's outer edge
#define CREASE_GAP 2      // real background-colored gap between a tile's upper and lower
                           // flap halves. Settled on 2px (back down from an intermediate
                           // 4px) once DIGIT_Y_NUDGE below was pushed further - a tighter
                           // crease reads better with the digits sitting higher in the
                           // tile. Fixed/constant - it doesn't scale during animation,
                           // since each flap animates independently (see FLIP ANIMATION
                           // DRIVER).

#define DIGIT_Y_NUDGE -10  // shifts the drawn digits up within their tile, independent of
                            // the tile's own position/size - negative moves up. Re-tuned to
                            // -10 for FONT_SEGOEUISL_70's own metrics (was -14, tuned for
                            // SonySketchEF - a different font's glyphs sit differently within
                            // their line box, so this needed its own pass on hardware).

#define NUM_TILES 2
#define TILE_HOUR 0
#define TILE_MINUTE 1

#define AMPM_INSET 4    // AM/PM label sits this many px clear of the MM tile's own right and
                          // bottom edges - only shown in 12h mode (clock_is_24h_style())
#define AMPM_W 26
#define AMPM_H 16

// The tile's crease geometry, split around CREASE_GAP: the upper flap's
// rest height (TOP_FLAP_REST_H), the lower flap's rest top edge relative
// to the tile's own top (BOTTOM_FLAP_REST_TOP_OFFSET), and the lower
// flap's rest height (BOTTOM_FLAP_REST_H). All derived from TILE_H/
// CREASE_GAP so they stay correct if either changes.
#define TOP_FLAP_REST_H (TILE_H / 2 - CREASE_GAP / 2)
#define BOTTOM_FLAP_REST_TOP_OFFSET (TILE_H / 2 + (CREASE_GAP - CREASE_GAP / 2))
#define BOTTOM_FLAP_REST_H (TILE_H - BOTTOM_FLAP_REST_TOP_OFFSET)

// TILE_W/TILE_H/TILE_GAP math (emery, 200px wide):
//   2*TILE_W + TILE_GAP + 2*HOUSING_PAD <= screen width, with a few px to
//   spare for the housing's own margin from the screen edge.
//   2*83 + 6 + 2*8 = 188 <= 200 (6px margin either side of the housing,
//   now that TILE_GAP is 6 instead of the original 10).
// TILE_W is a hard ceiling (it's already the max that fits), so the font
// resource size is what's tuned against it instead: at ~1.1x the point
// size per two-digit pair (a rough estimate - no real glyph metrics for
// this font), 70px leaves ~3px of padding on each side of an 83px tile,
// and ~76px is about where it's estimated to start clipping. 70px was
// chosen as the safer of the two; see the FONT note at the top of this
// file. TILE_H isn't part of this width constraint at all - it was
// shrunk purely because SonySketchEF's actual proportions turned out
// flatter/wider than assumed, leaving unused vertical space in the tile.

#define WEATHER_OVERLAP 11   // how far the weather strip overlaps the tiles' bottom edge -
                              // reduced from 16 so the strip follows the tiles' shrink
                              // upward, but by a few px less than the full freed space
#define WEATHER_Y_NUDGE 8     // extra downward shift applied on top of WEATHER_OVERLAP - kept
                               // as its own constant (same pattern as DIGIT_Y_NUDGE) rather
                               // than folded into WEATHER_OVERLAP, so the two remain
                               // independently tunable: WEATHER_OVERLAP is "how much the
                               // strip tucks under the tiles", WEATHER_Y_NUDGE is "how much
                               // further down the whole strip sits".
#define WEATHER_H 40
#define WEATHER_ICON_SIZE 48   // matches the bundled 48x48 "dramatic" HTC Sense-style icon
                                // PNGs exactly (no auto-scaling on Pebble) - stepped up from
                                // 36px. Bigger than WEATHER_H now, so it overflows the
                                // strip's own band symmetrically (top and bottom) via the
                                // existing icon_y centering formula below - no other geometry
                                // needed to change for this.
#define WEATHER_TOP_LINE_NUDGE 2   // shifts ONLY the top line of each weather column (temp,
                                     // weekday) down a couple px, closer to the line below it
                                     // (location, date) - the bottom line doesn't move.
#define WEATHER_ICON_Y_NUDGE -14   // shifts ONLY the weather icon (not the surrounding text
                                     // rows) up by 14px - independent of WEATHER_Y_NUDGE, which
                                     // still governs the text strip's own position. The icon is
                                     // now the topmost layer in the whole window (added last in
                                     // main_window_load()), so it's free to overlap the tiles
                                     // and the widened location text box above/behind it - see
                                     // "Weather + date strip" in the README.

#define HOUSING_PAD 8        // gap between the housing panel edge and the tiles/weather it frames
#define HOUSING_H 129        // CLOCK_ISLAND's actual authored height (192x129px, re-exported to
                              // match the tile/weather row after the TILE_H shrink) - kept as an
                              // explicit fixed value rather than derived from tile/weather
                              // geometry, so tile-size tweaks don't silently break the
                              // housing/PNG alignment again. Any corner rounding is baked into
                              // that PNG's own alpha shape rather than drawn in code (no more
                              // HOUSING_RADIUS - the panel isn't a code-drawn shape anymore).

// Stats panel: three icon+text rows below the housing (weather high/low,
// steps + sleep, watch + phone battery), separated by two centered 1px
// divider lines. Built around a uniform 20px row height, matching both the
// bundled 20x20 icons (WEATHER/STEPS/SLEEP/WATCH/PHONE) and
// FONT_SEGOEUISB_20's own pixel size.
#define STATS_ICON_SIZE 20
#define STATS_ROW_H 20
#define STATS_ICON_GAP 4     // clearance between an icon and the text beside it - same value
                               // as the weather strip's own icon_gap above
#define STATS_ROW_GAP 3      // clearance above/below each divider line
#define STATS_DIVIDER_H 1
#define STATS_DIVIDER_W 180  // narrower than the row content itself (188px, matching the
                               // housing's own width) - centered on screen, giving a 4px
                               // margin either side for visual breathing room
#define STATS_PAD_TOP 6      // gap between the housing's bottom edge and the first row
#define STATS_PAD_BOTTOM 7   // gap between the third row and the screen's bottom edge
// Row math (emery, 228px tall): the housing's bottom edge sits at y=141
// (TILE_Y - HOUSING_PAD + HOUSING_H = 20-8+129), leaving 87px down to the
// screen's own bottom edge (228). Three 20px rows + two 1px dividers +
// STATS_PAD_TOP/BOTTOM + 4x STATS_ROW_GAP (one on each side of both
// dividers) exactly fill that 87px, with no remainder:
//   6 + 20 + 3 + 1 + 3 + 20 + 3 + 1 + 3 + 20 + 7 = 87

#define ANIM_STEP_MS 30
#define ANIM_STEP_DELTA 20
#define STAGGER_MS 90

// ============================================================================
// TYPES
// ============================================================================

typedef enum {
    FLIP_IDLE = 0,
    FLIP_TOP_SHRINK,    // upper flap collapsing up into the crease (old value, hidden at end)
    FLIP_BOTTOM_GROW    // lower flap growing back down out of the crease (new value)
} FlipPhase;

typedef struct {
    int value;              // 2-digit value currently displayed (0-99)
    int target_value;        // value to swap to once the upper flap is fully collapsed
    FlipPhase phase;
    int progress;             // 0-100 within the current phase
    int delay_ms;             // stagger delay remaining before animation starts
    Layer *top_layer;         // upper flap; only its frame's height/y ever animates
    Layer *bottom_layer;      // lower flap; only its frame's height ever animates
    int card_x;                // fixed x (absolute, window-layer coordinates)
} FlipTile;

typedef struct {
    bool use_fahrenheit;
    int wallpaper_index;   // which HTC_WALLPAPER0N is the active background (0-4)
} ClaySettings;

#define SETTINGS_KEY 1

// ============================================================================
// GLOBAL STATE
// ============================================================================

static Window *s_main_window;
static Layer *s_window_layer;
static BitmapLayer *s_background_layer; // full-screen wallpaper, drawn behind everything else
static GBitmap *s_background_bitmap;    // the currently-selected HTC_WALLPAPER0N - see
                                         // BACKGROUND WALLPAPER section below
static BitmapLayer *s_housing_layer; // big backing panel framing the tiles + weather strip
static GBitmap *s_housing_bitmap;    // CLOCK_ISLAND - see the HOUSING PANEL section below

static FlipTile s_tiles[NUM_TILES];
static AppTimer *s_anim_timer = NULL;
static GFont s_flip_font;           // see FONT note at the top of this file
static TextLayer *s_ampm_layer;     // AM/PM label, bottom-right corner of the MM tile -
                                     // only shown in 12h mode, see update_ampm_label()

static ClaySettings settings;

// Weather state
static bool s_has_weather = false;
static int s_temperature = 0;
static int s_weather_icon = 0;
static char s_location_name[32] = "----";
static GBitmap *s_weather_bitmaps[9];
static BitmapLayer *s_weather_icon_layer;
static TextLayer *s_temp_layer;
static TextLayer *s_location_layer;
static TextLayer *s_day_layer;    // weekday name, right column top line - see update_date_layout()
static TextLayer *s_date_layer;   // "DD Month", right column bottom line

static const uint32_t WEATHER_RESOURCE_IDS[9] = {
    RESOURCE_ID_IMAGE_WX_CLEAR_DAY,
    RESOURCE_ID_IMAGE_WX_CLEAR_NIGHT,
    RESOURCE_ID_IMAGE_WX_PARTLY_DAY,
    RESOURCE_ID_IMAGE_WX_PARTLY_NIGHT,
    RESOURCE_ID_IMAGE_WX_CLOUDY,
    RESOURCE_ID_IMAGE_WX_FOG,
    RESOURCE_ID_IMAGE_WX_RAIN,
    RESOURCE_ID_IMAGE_WX_SNOW,
    RESOURCE_ID_IMAGE_WX_THUNDER
};

#define NUM_WALLPAPERS 5
static const uint32_t WALLPAPER_RESOURCE_IDS[NUM_WALLPAPERS] = {
    RESOURCE_ID_HTC_WALLPAPER01,
    RESOURCE_ID_HTC_WALLPAPER02,
    RESOURCE_ID_HTC_WALLPAPER03,
    RESOURCE_ID_HTC_WALLPAPER04,
    RESOURCE_ID_HTC_WALLPAPER05
};

// Stats panel state - see the STATS PANEL layout constants above and the
// STATS PANEL DATA section (update_stats_panel() etc.) below.
static GFont s_stats_font;   // FONT_SEGOEUISB_20 - loaded in main_window_load(), released in
                               // main_window_unload()

static BitmapLayer *s_stats_weather_icon_layer;
static GBitmap *s_stats_weather_icon_bitmap;
static TextLayer *s_stats_temp_hilo_layer;   // "H:24° L:16°C" - row 1

static BitmapLayer *s_stats_steps_icon_layer;
static GBitmap *s_stats_steps_icon_bitmap;
static TextLayer *s_stats_steps_layer;       // row 2, left half - HealthService, on-watch

static BitmapLayer *s_stats_sleep_icon_layer;
static GBitmap *s_stats_sleep_icon_bitmap;
static TextLayer *s_stats_sleep_layer;       // row 2, right half - HealthService, on-watch

static BitmapLayer *s_stats_watch_icon_layer;
static GBitmap *s_stats_watch_icon_bitmap;
static TextLayer *s_stats_watch_battery_layer;   // row 3, left half - BatteryStateService, on-watch

static BitmapLayer *s_stats_phone_icon_layer;
static GBitmap *s_stats_phone_icon_bitmap;
static TextLayer *s_stats_phone_battery_layer;   // row 3, right half - AppMessage from phone JS

static Layer *s_stats_divider1_layer;   // plain 1px white hairlines - see stats_divider_update_proc()
static Layer *s_stats_divider2_layer;

// Weather high/low (today) - arrive over AppMessage alongside the existing
// current-conditions fields, already converted to whatever unit (C/F) the
// forecast request was made in, same as s_temperature.
static int s_temp_high = 0;
static int s_temp_low = 0;

// Phone battery - unlike watch battery (read on-device via
// BatteryStateService), there's no native Pebble API for the connected
// phone's own battery level, so this arrives over AppMessage from the
// PebbleKit JS companion (see src/pkjs/index.js's initPhoneBattery()/
// sendPhoneBattery(), ported from MetroWP8 - event-driven, pushed
// whenever the phone's battery actually changes, not polled from here).
// s_has_phone_battery guards against showing a stale/default 0% before the
// first reading ever arrives.
static bool s_has_phone_battery = false;
static int s_phone_battery_percent = 0;
static bool s_phone_charging = false;   // MESSAGE_KEY_PHONE_CHARGING - shown as a "+" suffix
                                          // on the phone battery row, see update_stats_panel()

// ============================================================================
// SETTINGS
// ============================================================================

static void prv_default_settings(void) {
    settings.use_fahrenheit = false;
    settings.wallpaper_index = 0;   // HTC_WALLPAPER01
}

static void prv_save_settings(void) {
    persist_write_data(SETTINGS_KEY, &settings, sizeof(settings));
}

static void prv_load_settings(void) {
    prv_default_settings();
    persist_read_data(SETTINGS_KEY, &settings, sizeof(settings));
}

// ============================================================================
// FLIP TILE DRAWING
// ============================================================================

// Each tile is now two independent Layers - top_layer and bottom_layer -
// instead of one whole-tile layer. Only ONE of them ever animates at a
// time (see FLIP ANIMATION DRIVER): top_layer shrinks up into the crease
// while bottom_layer sits still at its full rest size, then bottom_layer
// grows back down out of the crease while top_layer sits still (already
// snapped back to its own full rest size, showing the new value). Both
// still draw a slice of the SAME full-tile-height 2-digit text, using the
// same "always draw the full-height text box, compensate with a y-offset"
// technique as before - just per-flap now instead of per-tile. Where a
// flap has shrunk away or hasn't grown in yet, nothing is drawn there at
// all (the housing panel underneath just shows through) - deliberately
// NOT pre-rendering the new value behind the animating flap, since that
// would let it peek through before the flip completes and undercut the
// effect.
static void draw_flap(GContext *ctx, FlipTile *t, GRect bounds, int y_offset, GCornerMask corners) {
    if (bounds.size.h <= 0) return;

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, TILE_RADIUS, corners);

    GRect text_box = GRect(0, y_offset, bounds.size.w, TILE_H);

    static char buf[3];
    snprintf(buf, sizeof(buf), "%02d", t->value);

    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, buf, s_flip_font, text_box,
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// Upper flap: its BOTTOM edge is always pinned at the crease (rest height
// TOP_FLAP_REST_H); only its TOP edge moves, down toward the crease as it
// shrinks. Its current top edge sits (TOP_FLAP_REST_H - bounds.size.h)
// pixels below the tile's nominal top - so the compensating text y-offset
// is the negative of that (bounds.size.h - TOP_FLAP_REST_H), same idea as
// the old whole-tile squash compensation, just against this flap's own
// rest height instead of TILE_H.
static void top_flap_update_proc(Layer *layer, GContext *ctx) {
    FlipTile *t = *(FlipTile **)layer_get_data(layer);
    GRect bounds = layer_get_bounds(layer);
    int y_offset = (bounds.size.h - TOP_FLAP_REST_H) + DIGIT_Y_NUDGE;
    draw_flap(ctx, t, bounds, y_offset, GCornersTop);
}

// Lower flap: its TOP edge is always pinned at the crease
// (TOP_FLAP_REST_H below the tile's nominal top... plus CREASE_GAP, i.e.
// BOTTOM_FLAP_REST_TOP_OFFSET); only its BOTTOM edge moves, growing down
// away from the crease. Because its top edge/origin never moves, the
// compensating text y-offset is just a constant, not something that needs
// recomputing from bounds.size.h.
static void bottom_flap_update_proc(Layer *layer, GContext *ctx) {
    FlipTile *t = *(FlipTile **)layer_get_data(layer);
    GRect bounds = layer_get_bounds(layer);
    int y_offset = -BOTTOM_FLAP_REST_TOP_OFFSET + DIGIT_Y_NUDGE;
    draw_flap(ctx, t, bounds, y_offset, GCornersBottom);
}

// ============================================================================
// BACKGROUND WALLPAPER (behind the housing panel, tiles, and weather strip)
// ============================================================================

// A full-screen (200x228 on emery) opaque background image, one of the five
// HTC_WALLPAPER0N resources, picked on the phone's settings page and stored
// in settings.wallpaper_index (0-4). Unlike the 9 small weather icons (which
// are cheap enough to keep all preloaded), a full-screen color PNG is much
// bigger, so only the currently-selected one is ever loaded into memory at a
// time - set_background_wallpaper() destroys the old GBitmap before loading
// the new one, both at startup and whenever the setting changes.
//
// This layer is added to s_window_layer FIRST, before the housing panel, so
// it sits at the very back - the housing's translucent CLOCK_ISLAND bitmap
// composites over it (GCompOpSet), and this is also what makes it possible
// to visually check the housing PNG's position/size against a real
// background instead of the plain black window fill used until now.
static void set_background_wallpaper(int index) {
    if (index < 0 || index >= NUM_WALLPAPERS) index = 0;

    if (s_background_bitmap) {
        gbitmap_destroy(s_background_bitmap);
        s_background_bitmap = NULL;
    }
    s_background_bitmap = gbitmap_create_with_resource(WALLPAPER_RESOURCE_IDS[index]);
    bitmap_layer_set_bitmap(s_background_layer, s_background_bitmap);
    layer_mark_dirty(bitmap_layer_get_layer(s_background_layer));
}

// ============================================================================
// HOUSING PANEL (behind the tiles + weather strip)
// ============================================================================

// The housing panel is the CLOCK_ISLAND bitmap resource (a real ~66%-
// opacity black PNG), drawn via a BitmapLayer with GCompOpSet compositing
// in main_window_load() below - there's no update_proc / custom drawing
// code for it, unlike the tiles.
//
// This replaces an earlier flat-fill version. Pebble's shape fills
// (graphics_fill_rect() etc.) do NOT alpha-blend against existing
// framebuffer content - the SDK docs are explicit that the alpha channel
// "only affects the bitmap drawing operations... it currently does not
// affect the filling or stroking operations" - so a translucent GColor
// fill rendered identically to an opaque one. Bitmap compositing
// (GCompOpSet) is different: it DOES respect per-pixel alpha, so as long
// as CLOCK_ISLAND's own alpha channel is genuinely translucent, this is
// real ~66% opacity, not an approximation.
//
// SIZE: for pixel-perfect alignment, CLOCK_ISLAND should exactly match the
// housing panel's rendered frame - 192x129px on emery. The frame's height
// comes directly from the fixed HOUSING_H constant (not derived from tile/
// weather geometry) specifically so it stays matched to the actual bitmap
// regardless of future tile-size tweaks - see the HOUSING_H comment above.
// Pebble's BitmapLayer does NOT auto-scale bitmaps to fit their frame - a
// mismatched PNG will just render at its own native size, centered in the
// frame, rather than stretching/shrinking to cover it.

// ============================================================================
// FLIP ANIMATION DRIVER
// ============================================================================

static void anim_timer_callback(void *data);

static bool any_tile_active(void) {
    for (int i = 0; i < NUM_TILES; i++) {
        if (s_tiles[i].phase != FLIP_IDLE || s_tiles[i].delay_ms > 0) return true;
    }
    return false;
}

static void start_anim_timer_if_needed(void) {
    if (!s_anim_timer && any_tile_active()) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "flip: starting animation timer");
        s_anim_timer = app_timer_register(ANIM_STEP_MS, anim_timer_callback, NULL);
    }
}

// Sets both flap layers' frames for the tile's CURRENT phase/progress.
// Exactly one of the two flaps is ever actually moving at a time - the
// other just sits at its rest frame:
//   FLIP_TOP_SHRINK:  top flap's height shrinks from TOP_FLAP_REST_H to 0
//                      (its bottom edge pinned at the crease, top edge
//                      moving down into it); bottom flap stays at rest,
//                      still showing the OLD value.
//   FLIP_BOTTOM_GROW:  top flap is back at its rest frame (already showing
//                      the NEW value, snapped in the instant the swap
//                      happened - see anim_timer_callback); bottom flap's
//                      height grows from 0 to BOTTOM_FLAP_REST_H (its top
//                      edge pinned at the crease, bottom edge moving down
//                      away from it), showing the NEW value.
static void update_flap_frames(FlipTile *t) {
    if (!t->top_layer || !t->bottom_layer) return;

    int top_h = TOP_FLAP_REST_H;
    int bottom_h = BOTTOM_FLAP_REST_H;

    if (t->phase == FLIP_TOP_SHRINK) {
        top_h = TOP_FLAP_REST_H - (TOP_FLAP_REST_H * t->progress) / 100;
    } else if (t->phase == FLIP_BOTTOM_GROW) {
        bottom_h = (BOTTOM_FLAP_REST_H * t->progress) / 100;
    }

    int top_top_y = TOP_FLAP_REST_H - top_h;  // top edge moves down as the flap shrinks

    layer_set_frame(t->top_layer, GRect(t->card_x, TILE_Y + top_top_y, TILE_W, top_h));
    layer_set_frame(t->bottom_layer, GRect(t->card_x, TILE_Y + BOTTOM_FLAP_REST_TOP_OFFSET, TILE_W, bottom_h));
    layer_mark_dirty(t->top_layer);
    layer_mark_dirty(t->bottom_layer);
}

static void anim_timer_callback(void *data) {
    bool still_active = false;

    for (int i = 0; i < NUM_TILES; i++) {
        FlipTile *t = &s_tiles[i];
        if (t->delay_ms > 0) {
            t->delay_ms -= ANIM_STEP_MS;
            still_active = true;
            continue;
        }
        if (t->phase == FLIP_IDLE) continue;

        t->progress += ANIM_STEP_DELTA;
        if (t->progress >= 100) {
            t->progress = 0;
            if (t->phase == FLIP_TOP_SHRINK) {
                // Top flap fully collapsed into the crease (hidden) - swap
                // to the new value now, then start growing the bottom flap.
                t->value = t->target_value;
                t->phase = FLIP_BOTTOM_GROW;
            } else {
                t->phase = FLIP_IDLE;
            }
        }
        still_active = true;
        update_flap_frames(t);
    }

    s_anim_timer = NULL;
    if (still_active) {
        s_anim_timer = app_timer_register(ANIM_STEP_MS, anim_timer_callback, NULL);
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "flip: animation timer stopped (idle)");
    }
}

// Kick off (or restart) the flip animation for a single tile.
static void flip_tile_to(int index, int new_value, int stagger_index) {
    FlipTile *t = &s_tiles[index];
    APP_LOG(APP_LOG_LEVEL_DEBUG, "flip: tile %d %02d -> %02d (stagger %d)",
            index, t->value, new_value, stagger_index);
    t->target_value = new_value;
    t->phase = FLIP_TOP_SHRINK;
    t->progress = 0;
    t->delay_ms = stagger_index * STAGGER_MS;
}

// ============================================================================
// TIME HANDLING
// ============================================================================

// Forward declaration: defined down in the STATS PANEL DATA section (after
// the WEATHER section, since it also reads s_temperature/s_has_weather),
// but update_tiles() below - defined earlier in the file - calls it on
// every minute tick.
static void update_stats_panel(void);

// AM/PM label - only visible in 12h mode (an empty string draws nothing, so
// no need to hide/show the layer itself). Uses the RAW 24h hour (before
// update_tiles() below converts its own local `hour` var to 12h), so this
// doesn't depend on call order relative to that conversion.
static void update_ampm_label(struct tm *tick_time) {
    if (!s_ampm_layer) return;
    if (clock_is_24h_style()) {
        text_layer_set_text(s_ampm_layer, "");
        return;
    }
    text_layer_set_text(s_ampm_layer, tick_time->tm_hour < 12 ? "AM" : "PM");
}

// Weekday name ("%A") and "DD Mmm" ("%d %b", abbreviated month) for the
// right-hand weather column. Pebble's own strftime() is locale-aware for
// %A/%b - it uses whatever language the watch's system locale is set to
// (via the Pebble mobile app), so this follows the user's language setting
// for free, with no phone-side involvement needed. Only refreshed at
// minute granularity like everything else here - the date only actually
// changes once a day, but re-formatting every tick is negligible cost.
static void update_date_layout(struct tm *tick_time) {
    if (!s_day_layer || !s_date_layer) return;

    static char day_buf[24];
    static char date_buf[24];
    strftime(day_buf, sizeof(day_buf), "%A", tick_time);
    strftime(date_buf, sizeof(date_buf), "%d %b", tick_time);

    text_layer_set_text(s_day_layer, day_buf);
    text_layer_set_text(s_date_layer, date_buf);
}

static void update_tiles(bool force_all) {
    time_t temp = time(NULL);
    struct tm *tick_time = localtime(&temp);
    if (!tick_time) return;

    update_ampm_label(tick_time);
    update_date_layout(tick_time);
    // Steps/sleep/watch-battery drift slowly over the day but cost nothing
    // to re-check every minute tick, same cadence as everything else here.
    update_stats_panel();

    int hour = tick_time->tm_hour;
    if (!clock_is_24h_style()) {
        hour = hour % 12;
        if (hour == 0) hour = 12;
    }
    int minute = tick_time->tm_min;

    int new_values[NUM_TILES] = { hour, minute };

    int stagger = 0;
    for (int i = 0; i < NUM_TILES; i++) {
        if (force_all || s_tiles[i].value != new_values[i]) {
            flip_tile_to(i, new_values[i], stagger);
            stagger++;
        }
    }
    start_anim_timer_if_needed();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    // Only the tile(s) whose value actually changed will animate - most
    // ticks that's just the MM tile; HH only flips on the hour.
    update_tiles(false);

    // Refresh weather every 30 minutes.
    if (tick_time->tm_min % 30 == 0) {
        DictionaryIterator *iter;
        if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
            dict_write_uint8(iter, MESSAGE_KEY_REQUEST_WEATHER, 1);
            app_message_outbox_send();
        }
    }
}

// ============================================================================
// ACCELEROMETER TAP (flick of the wrist)
// ============================================================================

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "flip: accel tap detected (axis %d, dir %ld)",
            axis, (long)direction);
    // Re-play the flip animation on both tiles, snapping to the current
    // time even if nothing actually changed.
    update_tiles(true);
}

// ============================================================================
// WEATHER (AppMessage from PebbleKit JS)
// ============================================================================

static void update_weather_layout(void) {
    if (!s_has_weather) {
        text_layer_set_text(s_location_layer, "");
        text_layer_set_text(s_temp_layer, "");
        return;
    }

    static char temp_buf[8];
    snprintf(temp_buf, sizeof(temp_buf), "%d°%s", s_temperature,
              settings.use_fahrenheit ? "F" : "C");
    text_layer_set_text(s_temp_layer, temp_buf);
    text_layer_set_text(s_location_layer, s_location_name);

    int icon = s_weather_icon;
    if (icon < 0 || icon > 8) icon = 4;
    if (s_weather_bitmaps[icon]) {
        bitmap_layer_set_bitmap(s_weather_icon_layer, s_weather_bitmaps[icon]);
    }
}

// ============================================================================
// STATS PANEL DATA (health, battery, weather high/low)
// ============================================================================

// Steps and sleep are read straight from the watch's own HealthService - no
// phone/AppMessage round-trip needed for either. health_service_metric_
// accessible() is checked first since health data may genuinely not be
// available (a fresh watch with no history yet, or the user has health
// tracking disabled) - in that case we show "--" rather than a misleading 0.
static bool health_metric_today(HealthMetric metric, int *out_value) {
    time_t start = time_start_of_today();
    time_t end = time(NULL);
    HealthServiceAccessibilityMask mask = health_service_metric_accessible(metric, start, end);
    if (!(mask & HealthServiceAccessibilityMaskAvailable)) return false;
    *out_value = (int)health_service_sum_today(metric);
    return true;
}

static void format_sleep_duration(char *buf, size_t buf_size, int sleep_seconds) {
    int hours = sleep_seconds / 3600;
    int minutes = (sleep_seconds % 3600) / 60;
    snprintf(buf, buf_size, "%dh%02dm", hours, minutes);
}

// Refreshes all three stats-panel rows. Cheap enough to call on every
// minute tick (from update_tiles()) as well as whenever a relevant
// AppMessage arrives (weather high/low, phone battery, or a UseFahrenheit
// change affecting row 1's unit letter) - it just re-formats and
// re-assigns text, no layout/geometry work happens here.
static void update_stats_panel(void) {
    // Row 1: today's high/low - already in whatever unit (C/F) the
    // forecast was fetched in, same as s_temperature; see the WEATHER
    // section above and TEMP_HIGH/TEMP_LOW in inbox_received_callback().
    static char hilo_buf[24];
    if (s_has_weather) {
        snprintf(hilo_buf, sizeof(hilo_buf), "High:%d° Low:%d°%s",
                  s_temp_high, s_temp_low, settings.use_fahrenheit ? "F" : "C");
    } else {
        snprintf(hilo_buf, sizeof(hilo_buf), "--");
    }
    text_layer_set_text(s_stats_temp_hilo_layer, hilo_buf);

    // Row 2, left: steps.
    static char steps_buf[16];
    int steps;
    if (health_metric_today(HealthMetricStepCount, &steps)) {
        snprintf(steps_buf, sizeof(steps_buf), "%d", steps);
    } else {
        snprintf(steps_buf, sizeof(steps_buf), "--");
    }
    text_layer_set_text(s_stats_steps_layer, steps_buf);

    // Row 2, right: sleep total, formatted as "7h 32m".
    static char sleep_buf[16];
    int sleep_seconds;
    if (health_metric_today(HealthMetricSleepSeconds, &sleep_seconds)) {
        format_sleep_duration(sleep_buf, sizeof(sleep_buf), sleep_seconds);
    } else {
        snprintf(sleep_buf, sizeof(sleep_buf), "--");
    }
    text_layer_set_text(s_stats_sleep_layer, sleep_buf);

    // Row 3, left: watch battery - BatteryStateService, on-watch, always
    // available (no accessibility check needed, unlike Health metrics).
    static char watch_batt_buf[8];
    BatteryChargeState watch_batt = battery_state_service_peek();
    snprintf(watch_batt_buf, sizeof(watch_batt_buf), "%d%%", watch_batt.charge_percent);
    text_layer_set_text(s_stats_watch_battery_layer, watch_batt_buf);

    // Row 3, right: phone battery - no native Pebble API for this, arrives
    // over AppMessage from the phone's own JS (see PHONE_BATTERY/
    // PHONE_CHARGING in inbox_received_callback() and initPhoneBattery()/
    // sendPhoneBattery() in index.js). A trailing "+" flags charging - my
    // own call on how to surface it now that PHONE_CHARGING exists, easy
    // to change (a dedicated icon, a different glyph, etc.) if you'd
    // rather show it differently.
    static char phone_batt_buf[8];
    if (s_has_phone_battery) {
        snprintf(phone_batt_buf, sizeof(phone_batt_buf), "%d%%%s", s_phone_battery_percent,
                  s_phone_charging ? "+" : "");
    } else {
        snprintf(phone_batt_buf, sizeof(phone_batt_buf), "--");
    }
    text_layer_set_text(s_stats_phone_battery_layer, phone_batt_buf);
}

// Plain 1px white hairline - shared by both stats-panel divider Layers.
static void stats_divider_update_proc(Layer *layer, GContext *ctx) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

// Clay's stock "select" control sends its value as a CSTRING tuple (HTML
// <select> elements always yield string values in the DOM, regardless of
// the "value" being numeric in config.js), while a "toggle" sends a real
// INT32. Reading straight from ->value->int32 on a tuple that's actually
// CSTRING silently reads garbage bytes reinterpreted as an int - which is
// exactly what made the Wallpaper setting appear to do nothing: the
// garbage value landed outside 0-4, got clamped back to 0 every time, so
// nothing ever visibly changed. This helper handles either tuple type.
static int tuple_get_int(Tuple *t, int fallback) {
    if (!t) return fallback;
    if (t->type == TUPLE_CSTRING) return atoi(t->value->cstring);
    return (int)t->value->int32;
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    bool weather_changed = false;

    Tuple *temp_tuple = dict_find(iterator, MESSAGE_KEY_TEMPERATURE);
    Tuple *icon_tuple = dict_find(iterator, MESSAGE_KEY_WEATHER_ICON);
    Tuple *loc_tuple = dict_find(iterator, MESSAGE_KEY_LOCATION_NAME);
    Tuple *high_tuple = dict_find(iterator, MESSAGE_KEY_TEMP_HIGH);
    Tuple *low_tuple = dict_find(iterator, MESSAGE_KEY_TEMP_LOW);

    if (temp_tuple) {
        s_temperature = (int)temp_tuple->value->int32;
        s_has_weather = true;
        weather_changed = true;
    }
    if (icon_tuple) {
        s_weather_icon = (int)icon_tuple->value->int32;
        weather_changed = true;
    }
    if (loc_tuple) {
        snprintf(s_location_name, sizeof(s_location_name), "%s", loc_tuple->value->cstring);
        weather_changed = true;
    }
    if (high_tuple) {
        s_temp_high = (int)high_tuple->value->int32;
        weather_changed = true;
    }
    if (low_tuple) {
        s_temp_low = (int)low_tuple->value->int32;
        weather_changed = true;
    }
    if (weather_changed) {
        update_weather_layout();
    }

    // Phone battery - see s_has_phone_battery/initPhoneBattery() notes above.
    // Both tuples normally arrive together in one message (sendPhoneBattery()
    // in index.js always sets both keys at once), but each is still read
    // independently in case that ever changes.
    Tuple *phone_batt_tuple = dict_find(iterator, MESSAGE_KEY_PHONE_BATTERY);
    if (phone_batt_tuple) {
        s_phone_battery_percent = (int)phone_batt_tuple->value->int32;
        s_has_phone_battery = true;
    }
    Tuple *phone_charging_tuple = dict_find(iterator, MESSAGE_KEY_PHONE_CHARGING);
    if (phone_charging_tuple) {
        s_phone_charging = (int)phone_charging_tuple->value->int32 == 1;
    }

    Tuple *fahrenheit_tuple = dict_find(iterator, MESSAGE_KEY_UseFahrenheit);
    if (fahrenheit_tuple) {
        bool new_val = tuple_get_int(fahrenheit_tuple, settings.use_fahrenheit ? 1 : 0) == 1;
        APP_LOG(APP_LOG_LEVEL_DEBUG, "settings: UseFahrenheit received = %d", new_val);
        if (new_val != settings.use_fahrenheit) {
            settings.use_fahrenheit = new_val;
            prv_save_settings();
            update_weather_layout();
        }
    }

    Tuple *wallpaper_tuple = dict_find(iterator, MESSAGE_KEY_Wallpaper);
    if (wallpaper_tuple) {
        int new_val = tuple_get_int(wallpaper_tuple, settings.wallpaper_index);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "settings: Wallpaper received = %d", new_val);
        if (new_val != settings.wallpaper_index) {
            settings.wallpaper_index = new_val;
            prv_save_settings();
            if (s_background_layer) set_background_wallpaper(new_val);
        }
    }

    // Unconditional, and deliberately last: cheap to call, and this way it
    // always reflects the final state of everything above in one message -
    // weather high/low, phone battery, and a UseFahrenheit change (which
    // affects row 1's unit letter) all land correctly however many of them
    // arrived together in this same dictionary.
    update_stats_panel();
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped, reason %d", reason);
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed, reason %d", reason);
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
}

// ============================================================================
// WINDOW LOAD / UNLOAD
// ============================================================================

static void create_tile(int index, int x) {
    FlipTile *t = &s_tiles[index];
    t->card_x = x;

    GRect top_frame = GRect(x, TILE_Y, TILE_W, TOP_FLAP_REST_H);
    t->top_layer = layer_create_with_data(top_frame, sizeof(FlipTile *));
    *(FlipTile **)layer_get_data(t->top_layer) = t;
    layer_set_update_proc(t->top_layer, top_flap_update_proc);
    layer_add_child(s_window_layer, t->top_layer);

    GRect bottom_frame = GRect(x, TILE_Y + BOTTOM_FLAP_REST_TOP_OFFSET, TILE_W, BOTTOM_FLAP_REST_H);
    t->bottom_layer = layer_create_with_data(bottom_frame, sizeof(FlipTile *));
    *(FlipTile **)layer_get_data(t->bottom_layer) = t;
    layer_set_update_proc(t->bottom_layer, bottom_flap_update_proc);
    layer_add_child(s_window_layer, t->bottom_layer);
}

// Builds one stats-panel entry: a STATS_ICON_SIZE-square icon at (x, y),
// with a text layer immediately to its right filling the rest of
// total_w (used for all five icon+text pairs - row 1's full-width weather
// entry, and the two half-width entries on each of rows 2 and 3).
static void create_stat_entry(BitmapLayer **icon_layer_out, GBitmap **icon_bitmap_out,
                               uint32_t icon_resource_id, TextLayer **text_layer_out,
                               int x, int y, int total_w) {
    *icon_layer_out = bitmap_layer_create(GRect(x, y, STATS_ICON_SIZE, STATS_ICON_SIZE));
    bitmap_layer_set_compositing_mode(*icon_layer_out, GCompOpSet);
    *icon_bitmap_out = gbitmap_create_with_resource(icon_resource_id);
    bitmap_layer_set_bitmap(*icon_layer_out, *icon_bitmap_out);
    layer_add_child(s_window_layer, bitmap_layer_get_layer(*icon_layer_out));

    int text_x = x + STATS_ICON_SIZE + STATS_ICON_GAP;
    int text_w = total_w - STATS_ICON_SIZE - STATS_ICON_GAP;
    *text_layer_out = text_layer_create(GRect(text_x, y, text_w, STATS_ROW_H));
    text_layer_set_background_color(*text_layer_out, GColorClear);
    text_layer_set_text_color(*text_layer_out, GColorWhite);
    text_layer_set_font(*text_layer_out, s_stats_font);
    text_layer_set_text_alignment(*text_layer_out, GTextAlignmentLeft);
    text_layer_set_overflow_mode(*text_layer_out, GTextOverflowModeTrailingEllipsis);
    layer_add_child(s_window_layer, text_layer_get_layer(*text_layer_out));
}

static void main_window_load(Window *window) {
    s_window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(s_window_layer);

    window_set_background_color(window, GColorBlack);

    // Custom font: trying Segoe UI Semilight at 70px (FONT_SEGOEUISL_70) in
    // place of SonySketchEF - see the FONT note at the top of this file.
    // To switch back, swap this for RESOURCE_ID_SONY_SKETCH_70 (that
    // resource is still declared in package.json).
    s_flip_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUISL_70));

    // Custom font for the stats panel below the housing - Segoe UI
    // Semibold at 20px, matching the panel's own 20px row height and its
    // bundled 20x20 icons.
    s_stats_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUISB_20));

    // Geometry: two flip tiles - HH and MM - centered as a group near the
    // top, and a weather strip whose top edge overlaps the last few pixels
    // of the tiles.
    int content_w = TILE_W * 2 + TILE_GAP;
    int x0 = (bounds.size.w - content_w) / 2;
    int x1 = x0 + TILE_W + TILE_GAP;

    int weather_y = TILE_Y + TILE_H - WEATHER_OVERLAP + WEATHER_Y_NUDGE;
    int tile_row_right = x1 + TILE_W;   // right edge of the MM tile, for right-aligning the temp

    // Background wallpaper - added FIRST, before even the housing panel, so
    // it's the very bottom-most layer (see BACKGROUND WALLPAPER section
    // above). Covers the full screen; the actual bitmap is loaded by
    // set_background_wallpaper() based on the persisted setting.
    s_background_layer = bitmap_layer_create(GRect(0, 0, bounds.size.w, bounds.size.h));
    layer_add_child(s_window_layer, bitmap_layer_get_layer(s_background_layer));
    set_background_wallpaper(settings.wallpaper_index);

    // Housing panel - added next so it renders behind the tiles/weather but
    // in front of the wallpaper, framing the tiles + weather strip as one
    // unit like the original HTC widget (see the HOUSING PANEL section
    // above for how CLOCK_ISLAND is composited). SIZE NOTE: the height
    // comes from the fixed HOUSING_H constant (matching CLOCK_ISLAND's
    // actual 192x129px size) rather than being derived from the tile/
    // weather geometry above, so it stays correctly matched to the bitmap
    // even as TILE_H etc. get tuned.
    GRect housing_frame = GRect(
        x0 - HOUSING_PAD,
        TILE_Y - HOUSING_PAD,
        content_w + HOUSING_PAD * 2,
        HOUSING_H
    );
    s_housing_bitmap = gbitmap_create_with_resource(RESOURCE_ID_CLOCK_ISLAND);
    s_housing_layer = bitmap_layer_create(housing_frame);
    bitmap_layer_set_compositing_mode(s_housing_layer, GCompOpSet);
    bitmap_layer_set_bitmap(s_housing_layer, s_housing_bitmap);
    layer_add_child(s_window_layer, bitmap_layer_get_layer(s_housing_layer));

    create_tile(TILE_HOUR, x0);
    create_tile(TILE_MINUTE, x1);

    // AM/PM label - added right after the tiles so it draws on top of the
    // MM tile's bottom-right corner, inset AMPM_INSET px from that tile's
    // own right and bottom edges. Text is set by update_ampm_label() (only
    // non-empty in 12h mode), called from update_tiles() below.
    GRect ampm_frame = GRect(
        x1 + TILE_W - AMPM_INSET - AMPM_W,
        TILE_Y + TILE_H - AMPM_INSET - AMPM_H,
        AMPM_W,
        AMPM_H
    );
    s_ampm_layer = text_layer_create(ampm_frame);
    text_layer_set_background_color(s_ampm_layer, GColorClear);
    text_layer_set_text_color(s_ampm_layer, GColorBlack);
    text_layer_set_font(s_ampm_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
    text_layer_set_text_alignment(s_ampm_layer, GTextAlignmentRight);
    layer_add_child(s_window_layer, text_layer_get_layer(s_ampm_layer));

    // Icon position is computed here (needed below for the text columns'
    // left_w/right_x math) but the layer itself isn't created/added until
    // the very end of this function - see the comment down there for why.
    int icon_x = bounds.size.w / 2 - WEATHER_ICON_SIZE / 2;
    int icon_y = weather_y + (WEATHER_H - WEATHER_ICON_SIZE) / 2 + WEATHER_ICON_Y_NUDGE;

    // Two text rows exactly split WEATHER_H (20px each here) instead of one
    // centered row - same per-row height the single line used to get, just
    // stacked now instead of vertically centered with padding above/below.
    int line_h = WEATHER_H / 2;
    int icon_gap = 4;   // clearance between the icon and either text block

    // LEFT block (temp on top, location below) - flush with the tile row's
    // own LEFT outer edge (no inset - more room, per your request), left-
    // aligned so both lines start at that edge. left_w (temp's own width)
    // stops short of the icon, same as before.
    int left_x = x0;
    int left_w = (icon_x - icon_gap) - left_x;

    // RIGHT block x/width - computed here (ahead of where it's used to
    // build the RIGHT block layers below) because the LOCATION box also
    // needs right_x as its own right edge now (see below).
    int right_x = icon_x + WEATHER_ICON_SIZE + icon_gap;
    int right_w = tile_row_right - right_x;

    s_temp_layer = text_layer_create(GRect(left_x, weather_y + WEATHER_TOP_LINE_NUDGE, left_w, line_h));
    text_layer_set_background_color(s_temp_layer, GColorClear);
    text_layer_set_text_color(s_temp_layer, GColorWhite);
    text_layer_set_font(s_temp_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_temp_layer, GTextAlignmentLeft);
    layer_add_child(s_window_layer, text_layer_get_layer(s_temp_layer));

    // Location now uses the SAME font as temp (both GOTHIC_18_BOLD), per
    // your instruction - previously it was the plain (non-bold) weight.
    // WIDTH: unlike temp above (which stops short of the icon), location's
    // box now runs the full width from the left edge all the way to
    // right_x (where the day/date column starts) - i.e. straight underneath/
    // behind the icon - so a long city name has much more room before
    // hitting the trailing-ellipsis cutoff. This only works visually
    // because the weather icon is now the topmost layer in the window (see
    // the icon layer creation at the end of this function) - it paints over
    // whatever part of the location text happens to sit behind it, instead
    // of the icon being hidden behind a same-z-order text layer's blank
    // background (GColorClear means there's no literal occlusion either
    // way, but keeping the icon last/topmost is what makes the stacking
    // intent explicit and correct if either layer's rendering ever changes).
    s_location_layer = text_layer_create(GRect(left_x, weather_y + line_h, right_x - left_x, line_h));
    text_layer_set_background_color(s_location_layer, GColorClear);
    text_layer_set_text_color(s_location_layer, GColorWhite);
    text_layer_set_font(s_location_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_location_layer, GTextAlignmentLeft);
    text_layer_set_overflow_mode(s_location_layer, GTextOverflowModeTrailingEllipsis);
    layer_add_child(s_window_layer, text_layer_get_layer(s_location_layer));

    // RIGHT block (weekday on top, "DD Mmm" below) - flush with the tile
    // row's own RIGHT outer edge, right-aligned. Date now uses the same
    // GOTHIC_18_BOLD as the other three fields (was the plain GOTHIC_18) -
    // the abbreviated "DD Mmm" format below frees up the width the plain
    // weight used to buy. Text is set by update_date_layout() (called from
    // update_tiles()).
    s_day_layer = text_layer_create(GRect(right_x, weather_y + WEATHER_TOP_LINE_NUDGE, right_w, line_h));
    text_layer_set_background_color(s_day_layer, GColorClear);
    text_layer_set_text_color(s_day_layer, GColorWhite);
    text_layer_set_font(s_day_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_day_layer, GTextAlignmentRight);
    text_layer_set_overflow_mode(s_day_layer, GTextOverflowModeTrailingEllipsis);
    layer_add_child(s_window_layer, text_layer_get_layer(s_day_layer));

    s_date_layer = text_layer_create(GRect(right_x, weather_y + line_h, right_w, line_h));
    text_layer_set_background_color(s_date_layer, GColorClear);
    text_layer_set_text_color(s_date_layer, GColorWhite);
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_date_layer, GTextAlignmentRight);
    text_layer_set_overflow_mode(s_date_layer, GTextOverflowModeTrailingEllipsis);
    layer_add_child(s_window_layer, text_layer_get_layer(s_date_layer));

    // Weather icon layer - created and added LAST, after every other layer
    // in this function (tiles, AM/PM label, all four weather/date text
    // layers), so it's the topmost layer in the whole window. Per your
    // request it's now allowed to overlap other elements (it already
    // overlaps the tiles' bottom edge now that WEATHER_ICON_Y_NUDGE moves
    // it up, and it overlaps the widened location text box above/behind
    // it) - being topmost is what makes that overlap actually render on
    // top instead of getting hidden behind them.
    s_weather_icon_layer = bitmap_layer_create(GRect(icon_x, icon_y, WEATHER_ICON_SIZE, WEATHER_ICON_SIZE));
    bitmap_layer_set_compositing_mode(s_weather_icon_layer, GCompOpSet);
    layer_add_child(s_window_layer, bitmap_layer_get_layer(s_weather_icon_layer));

    // Load weather icon bitmaps.
    for (int i = 0; i < 9; i++) {
        s_weather_bitmaps[i] = gbitmap_create_with_resource(WEATHER_RESOURCE_IDS[i]);
    }

    // STATS PANEL - three icon+text rows below the housing (weather
    // high/low, steps + sleep, watch + phone battery), separated by two
    // centered 1px divider lines. See the STATS PANEL layout constants
    // near the top of this file for the exact vertical math.
    //
    // Horizontally, the panel is aligned with the housing's own left/right
    // edges (not the tile row's) - housing_frame is wider than the tile
    // row by HOUSING_PAD on each side, and that's the edge these rows
    // should line up with, per your instruction.
    int stats_left = housing_frame.origin.x;
    int stats_w = housing_frame.size.w;
    int stats_half_w = stats_w / 2;

    int housing_bottom = housing_frame.origin.y + HOUSING_H;
    int stats_row1_y = housing_bottom + STATS_PAD_TOP;
    int stats_divider1_y = stats_row1_y + STATS_ROW_H + STATS_ROW_GAP;
    int stats_row2_y = stats_divider1_y + STATS_DIVIDER_H + STATS_ROW_GAP;
    int stats_divider2_y = stats_row2_y + STATS_ROW_H + STATS_ROW_GAP;
    int stats_row3_y = stats_divider2_y + STATS_DIVIDER_H + STATS_ROW_GAP;
    int stats_divider_x = (bounds.size.w - STATS_DIVIDER_W) / 2;

    // Row 1: WEATHER icon + today's high/low, full row width.
    create_stat_entry(&s_stats_weather_icon_layer, &s_stats_weather_icon_bitmap, RESOURCE_ID_WEATHER,
                       &s_stats_temp_hilo_layer, stats_left, stats_row1_y, stats_w);

    s_stats_divider1_layer = layer_create(GRect(stats_divider_x, stats_divider1_y, STATS_DIVIDER_W, STATS_DIVIDER_H));
    layer_set_update_proc(s_stats_divider1_layer, stats_divider_update_proc);
    layer_add_child(s_window_layer, s_stats_divider1_layer);

    // Row 2: STEPS (left half) + SLEEP (right half) - both read straight
    // off the watch via HealthService, see update_stats_panel().
    create_stat_entry(&s_stats_steps_icon_layer, &s_stats_steps_icon_bitmap, RESOURCE_ID_STEPS,
                       &s_stats_steps_layer, stats_left, stats_row2_y, stats_half_w);
    create_stat_entry(&s_stats_sleep_icon_layer, &s_stats_sleep_icon_bitmap, RESOURCE_ID_SLEEP,
                       &s_stats_sleep_layer, stats_left + stats_half_w, stats_row2_y, stats_half_w);

    s_stats_divider2_layer = layer_create(GRect(stats_divider_x, stats_divider2_y, STATS_DIVIDER_W, STATS_DIVIDER_H));
    layer_set_update_proc(s_stats_divider2_layer, stats_divider_update_proc);
    layer_add_child(s_window_layer, s_stats_divider2_layer);

    // Row 3: WATCH battery (left half, BatteryStateService) + PHONE
    // battery (right half, AppMessage from the phone's own JS).
    create_stat_entry(&s_stats_watch_icon_layer, &s_stats_watch_icon_bitmap, RESOURCE_ID_WATCH,
                       &s_stats_watch_battery_layer, stats_left, stats_row3_y, stats_half_w);
    create_stat_entry(&s_stats_phone_icon_layer, &s_stats_phone_icon_bitmap, RESOURCE_ID_PHONE,
                       &s_stats_phone_battery_layer, stats_left + stats_half_w, stats_row3_y, stats_half_w);

    update_tiles(true);
    update_weather_layout();
    // Note: update_tiles(true) above already calls update_stats_panel()
    // internally (see TIME HANDLING section), so the panel is populated
    // as soon as this function returns without a separate call here.
}

static void main_window_unload(Window *window) {
    for (int i = 0; i < NUM_TILES; i++) {
        layer_destroy(s_tiles[i].top_layer);
        layer_destroy(s_tiles[i].bottom_layer);
        s_tiles[i].top_layer = NULL;
        s_tiles[i].bottom_layer = NULL;
    }
    bitmap_layer_destroy(s_housing_layer);
    gbitmap_destroy(s_housing_bitmap);

    bitmap_layer_destroy(s_background_layer);
    if (s_background_bitmap) gbitmap_destroy(s_background_bitmap);

    text_layer_destroy(s_ampm_layer);

    text_layer_destroy(s_location_layer);
    text_layer_destroy(s_temp_layer);
    text_layer_destroy(s_day_layer);
    text_layer_destroy(s_date_layer);
    bitmap_layer_destroy(s_weather_icon_layer);

    for (int i = 0; i < 9; i++) {
        if (s_weather_bitmaps[i]) gbitmap_destroy(s_weather_bitmaps[i]);
    }

    // Stats panel
    text_layer_destroy(s_stats_temp_hilo_layer);
    bitmap_layer_destroy(s_stats_weather_icon_layer);
    gbitmap_destroy(s_stats_weather_icon_bitmap);

    text_layer_destroy(s_stats_steps_layer);
    bitmap_layer_destroy(s_stats_steps_icon_layer);
    gbitmap_destroy(s_stats_steps_icon_bitmap);

    text_layer_destroy(s_stats_sleep_layer);
    bitmap_layer_destroy(s_stats_sleep_icon_layer);
    gbitmap_destroy(s_stats_sleep_icon_bitmap);

    text_layer_destroy(s_stats_watch_battery_layer);
    bitmap_layer_destroy(s_stats_watch_icon_layer);
    gbitmap_destroy(s_stats_watch_icon_bitmap);

    text_layer_destroy(s_stats_phone_battery_layer);
    bitmap_layer_destroy(s_stats_phone_icon_layer);
    gbitmap_destroy(s_stats_phone_icon_bitmap);

    layer_destroy(s_stats_divider1_layer);
    layer_destroy(s_stats_divider2_layer);

    fonts_unload_custom_font(s_flip_font);
    fonts_unload_custom_font(s_stats_font);
}

// ============================================================================
// APPLICATION LIFECYCLE
// ============================================================================

static void init(void) {
    prv_load_settings();

    s_main_window = window_create();
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload
    });
    window_stack_push(s_main_window, true);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    accel_tap_service_subscribe(accel_tap_handler);

    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_register_outbox_sent(outbox_sent_callback);
    app_message_open(256, 256);
}

static void deinit(void) {
    if (s_anim_timer) {
        app_timer_cancel(s_anim_timer);
        s_anim_timer = NULL;
    }
    tick_timer_service_unsubscribe();
    accel_tap_service_unsubscribe();
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
