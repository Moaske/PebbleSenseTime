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
 * HOUSING: a panel-shaped region sits behind the tiles + weather strip,
 * echoing the dark "widget housing" of the original HTC design. It USED to
 * be its own always-resident CLOCK_ISLAND bitmap, alpha-composited live via
 * GCompOpSet over whichever wallpaper was loaded - genuine per-pixel
 * translucency, not a flat-fill approximation, but a whole separate bitmap
 * kept in memory for the app's entire lifetime on top of the wallpaper. It's
 * now pre-blended directly into each of the 6 wallpaper PNGs at build time
 * instead (same final on-screen pixels, one fewer resident bitmap) - see
 * "The housing panel is now baked into the wallpapers" in the HOUSING PANEL
 * section below.
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

#define SHADOW_OFFSET 2   // drop-shadow trick for the five stats-panel row texts below the
                            // clock island ONLY (weather H/L, steps, sleep, watch battery,
                            // phone battery) - a black copy of each layer sits SHADOW_OFFSET px
                            // down-and-right of the real (white) one, painted first so the real
                            // layer sits on top of it. Deliberately NOT applied to the temp/
                            // location/weekday/date strip, which sits ON the clock island
                            // itself, not below it. See create_shadow_text_layer()/
                            // set_shadowed_text() below.

#define HOUSING_PAD 8        // gap between the housing panel edge and the tiles/weather it frames
#define HOUSING_H 129         // Height of the housing panel region (housing_frame below) - no
                              // longer a bitmap's own size (there's no separate housing bitmap
                              // anymore, see the HOUSING PANEL section), just the fixed geometry
                              // the wallpapers were baked to match and that the stats panel below
                              // still aligns itself to. Kept as an explicit fixed value rather
                              // than derived from tile/weather geometry, so tile-size tweaks don't
                              // silently break the baked-wallpaper/stats-panel alignment - if
                              // TILE_H etc. ever change, the 6 wallpaper images need re-baking to
                              // match this constant, not just this constant updated to match them.

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
    int wallpaper_index;   // which HTC_WALLPAPER0N is the active background (0-5)
    bool bold_clock_font;  // false = Segoe UI Semilight (FONT_SEGOEUISL_70, default),
                            // true = Segoe UI Semibold (FONT_SEGOEUISB_70)
    bool raindrops_enabled;   // default true - gates the RAINDROPS overlay alongside the
                                // existing weathercode==6 check, see update_weather_layout()
    int weather_update_interval_min;   // how often tick_handler() requests a weather refresh,
                                         // in minutes - one of {15, 30, 60}, default 30. Added
                                         // at the END of the struct (see prv_load_settings()
                                         // comment) so upgrading users with an old persisted
                                         // blob still load fine; persist_read_data() only
                                         // overwrites as many bytes as the OLD saved blob had,
                                         // leaving this field at its prv_default_settings()
                                         // value (30) until they open the settings page again.
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

// There's no separate s_housing_layer/s_housing_bitmap anymore - the housing
// panel (formerly CLOCK_ISLAND, its own always-resident GBitmap) is now
// pre-blended directly into each of the 5 HTC_WALLPAPER0N images at build
// time, so s_background_bitmap alone covers both. See "The housing panel is
// now baked into the wallpapers" in the HOUSING PANEL section below for why
// and how - this removed an entire permanently-resident bitmap (~24.8KB
// decoded) from the app's steady-state memory footprint for free, since the
// wallpaper already has to be resident whenever anything is on screen
// anyway.

static FlipTile s_tiles[NUM_TILES];
static AppTimer *s_anim_timer = NULL;
static GFont s_flip_font;           // see FONT note at the top of this file
static TextLayer *s_ampm_layer;     // AM/PM label, bottom-right corner of the MM tile -
                                     // only shown in 12h mode, see update_ampm_label()

static ClaySettings settings;

// Weather state
static bool s_has_weather = false;
// Always stored in Celsius, regardless of settings.use_fahrenheit - the
// phone JS (src/pkjs/index.js) always fetches from Open-Meteo in Celsius
// and never re-fetches just because the unit toggle changed. Converting to
// Fahrenheit for display (celsius_to_fahrenheit() below) happens entirely
// on-watch, at format time, so flipping the Clay "Use Fahrenheit" toggle
// recalculates the displayed number instantly - it used to just swap the
// unit letter and leave the number as whatever was last fetched (in
// whichever unit that happened to be) until the next weather refresh,
// which could be minutes away or never arrive at all if the phone's
// GPS/network fetch failed.
static int s_temperature = 0;
static int s_weather_icon = 0;
static char s_location_name[32] = "----";
static GBitmap *s_weather_icon_bitmap;   // lazy-loaded, only the CURRENT icon stays resident -
                                           // see set_weather_icon_bitmap() below
static int s_weather_icon_bitmap_index = -1;   // which of the 9 is currently loaded (-1 = none yet)
static BitmapLayer *s_weather_icon_layer;
static TextLayer *s_temp_layer;
static TextLayer *s_location_layer;
static TextLayer *s_day_layer;    // weekday name, right column top line - see update_date_layout()
static TextLayer *s_date_layer;   // "DD Month", right column bottom line

#define WEATHER_ICON_RAIN 6   // index into WEATHER_RESOURCE_IDS[]/weatherCodeToIconIndex() -
                                // matches index.js's own comment ("6 rain"). Used to trigger the
                                // full-screen RAINDROPS overlay, see s_raindrops_layer below.

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

// Full-screen RAINDROPS overlay - shown only while BOTH the user's Clay
// toggle is on (settings.raindrops_enabled, default true) AND the current
// weather condition is rain (WEATHER_ICON_RAIN); hidden if either is
// false. Created LAST in main_window_load() (after every other layer,
// including the stats panel), so it's the topmost layer in the entire
// window and can draw over absolutely everything else - tiles, housing,
// weather strip, stats panel.
static BitmapLayer *s_raindrops_layer;
static GBitmap *s_raindrops_bitmap;

#define NUM_WALLPAPERS 6
static const uint32_t WALLPAPER_RESOURCE_IDS[NUM_WALLPAPERS] = {
    RESOURCE_ID_HTC_WALLPAPER01,
    RESOURCE_ID_HTC_WALLPAPER02,
    RESOURCE_ID_HTC_WALLPAPER03,
    RESOURCE_ID_HTC_WALLPAPER04,
    RESOURCE_ID_HTC_WALLPAPER05,
    RESOURCE_ID_HTC_WALLPAPER06   // "Arrows" - added along with the housing-crop fix, see
                                    // "The housing panel is now baked into the wallpapers" below
};

// Stats panel state - see the STATS PANEL layout constants above and the
// STATS PANEL DATA section (update_stats_panel() etc.) below.
static GFont s_stats_font;   // FONT_SEGOEUISB_20 - loaded in main_window_load(), released in
                               // main_window_unload()

static BitmapLayer *s_stats_weather_icon_layer;
static GBitmap *s_stats_weather_icon_bitmap;
static TextLayer *s_stats_temp_hilo_layer;   // "H:24° L:16°C" - row 1
static TextLayer *s_stats_temp_hilo_shadow_layer;   // drop shadow - see SHADOW_OFFSET

static BitmapLayer *s_stats_steps_icon_layer;
static GBitmap *s_stats_steps_icon_bitmap;
static TextLayer *s_stats_steps_layer;       // row 2, left half - HealthService, on-watch
static TextLayer *s_stats_steps_shadow_layer;       // drop shadow - see SHADOW_OFFSET

static BitmapLayer *s_stats_sleep_icon_layer;
static GBitmap *s_stats_sleep_icon_bitmap;
static TextLayer *s_stats_sleep_layer;       // row 2, right half - HealthService, on-watch
static TextLayer *s_stats_sleep_shadow_layer;       // drop shadow - see SHADOW_OFFSET

static BitmapLayer *s_stats_watch_icon_layer;
static GBitmap *s_stats_watch_icon_bitmap;
static TextLayer *s_stats_watch_battery_layer;   // row 3, left half - BatteryStateService, on-watch
static TextLayer *s_stats_watch_battery_shadow_layer;   // drop shadow - see SHADOW_OFFSET

static BitmapLayer *s_stats_phone_icon_layer;
static GBitmap *s_stats_phone_icon_bitmap;
static TextLayer *s_stats_phone_battery_layer;   // row 3, right half - AppMessage from phone JS
static TextLayer *s_stats_phone_battery_shadow_layer;   // drop shadow - see SHADOW_OFFSET

static Layer *s_stats_divider1_layer;   // plain 1px white hairlines - see stats_divider_update_proc()
static Layer *s_stats_divider2_layer;

// Weather high/low (today) - arrive over AppMessage alongside the existing
// current-conditions fields. Always Celsius, same as s_temperature (see the
// note there) - converted for display in celsius_to_fahrenheit() below.
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
    settings.bold_clock_font = false;   // Semilight
    settings.raindrops_enabled = true;
    settings.weather_update_interval_min = 30;
}

static void prv_save_settings(void) {
    persist_write_data(SETTINGS_KEY, &settings, sizeof(settings));
}

static void prv_load_settings(void) {
    prv_default_settings();
    persist_read_data(SETTINGS_KEY, &settings, sizeof(settings));
}

// ----------------------------------------------------------------------
// Weather now persists across watchface reloads
// ----------------------------------------------------------------------
// Pebble watchfaces aren't backgrounded like a phone app - leaving the
// watchface for the app menu (or any other app) and coming back re-runs
// init() from scratch, so every static global (including s_has_weather/
// s_temperature/s_weather_icon/s_location_name/s_temp_high/s_temp_low)
// used to reset to its zero-value default every single time. That made
// update_weather_layout() blank the weather strip (and update_stats_panel()
// show "--" for H/L) on every return to the watchface, until a brand new
// REQUEST_WEATHER round-trip (GPS + two HTTPS calls on the phone) came
// back - a multi-second blank/placeholder state on every single return,
// even though the watch already knew perfectly good, only-slightly-stale
// weather data moments earlier.
//
// Fixed by caching the last-known weather reading in persistent storage
// (a separate key from SETTINGS_KEY, so this can't ever collide with or
// disturb ClaySettings' own append-only layout) - prv_load_weather_cache()
// runs in init(), before window_stack_push() ever triggers
// main_window_load(), so by the time update_weather_layout()/
// update_stats_panel() run for the very first time, s_has_weather and
// friends already hold the last real reading and paint immediately -
// stale by however long the watchface was away, but never blank. The
// normal refresh path is untouched: inbox_received_callback() still
// updates the same statics and repaints the instant a fresh AppMessage
// arrives from the phone, exactly as before - this only changes what's
// shown before that first fresh update lands.
typedef struct {
    bool has_weather;
    int temperature;         // Celsius, same convention as s_temperature
    int weather_icon;
    char location_name[32];  // matches sizeof(s_location_name)
    int temp_high;            // Celsius
    int temp_low;              // Celsius
} WeatherCache;

#define WEATHER_CACHE_KEY 2   // deliberately != SETTINGS_KEY (1)

static void prv_save_weather_cache(void) {
    WeatherCache cache;
    cache.has_weather = s_has_weather;
    cache.temperature = s_temperature;
    cache.weather_icon = s_weather_icon;
    cache.temp_high = s_temp_high;
    cache.temp_low = s_temp_low;
    snprintf(cache.location_name, sizeof(cache.location_name), "%s", s_location_name);
    persist_write_data(WEATHER_CACHE_KEY, &cache, sizeof(cache));
}

static void prv_load_weather_cache(void) {
    // persist_exists() guards this explicitly (rather than the
    // default-then-unconditional-overlay pattern prv_load_settings() uses)
    // because there's no "default" WeatherCache worth overlaying blindly -
    // on a brand new watch, or before the very first weather fetch ever
    // completes, s_has_weather's own static initializer (false) is already
    // exactly the right fallback, and skipping the read entirely avoids
    // ever copying a zeroed/garbage location_name over the top of it.
    if (!persist_exists(WEATHER_CACHE_KEY)) return;

    WeatherCache cache;
    if (persist_read_data(WEATHER_CACHE_KEY, &cache, sizeof(cache)) != (int)sizeof(cache)) return;

    s_has_weather = cache.has_weather;
    s_temperature = cache.temperature;
    s_weather_icon = cache.weather_icon;
    s_temp_high = cache.temp_high;
    s_temp_low = cache.temp_low;
    snprintf(s_location_name, sizeof(s_location_name), "%s", cache.location_name);
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

// A full-screen (200x228 on emery) opaque background image, one of the six
// HTC_WALLPAPER0N resources, picked on the phone's settings page and stored
// in settings.wallpaper_index (0-5). Unlike the 9 small weather icons (which
// are cheap enough to keep all preloaded), a full-screen color PNG is much
// bigger, so only the currently-selected one is ever loaded into memory at a
// time - set_background_wallpaper() destroys the old GBitmap before loading
// the new one, both at startup and whenever the setting changes.
//
// This layer is added to s_window_layer FIRST, so it sits at the very back.
// It no longer needs a separate housing panel compositing over it at
// runtime - the housing is baked directly into each wallpaper PNG now (see
// "The housing panel is now baked into the wallpapers" below) - but the
// wallpaper still has to render behind the tiles/weather/stats panel, so it
// stays the first (bottom-most) layer added regardless.
static void set_background_wallpaper(int index) {
    if (index < 0 || index >= NUM_WALLPAPERS) index = 0;

    if (s_background_bitmap) {
        // Clear the layer's own reference FIRST, before freeing the bitmap
        // it points to - otherwise, if the new bitmap below fails to load,
        // s_background_layer is left holding a dangling pointer to memory
        // that was already freed (a real bug this project had all along,
        // just never hit until a decode failure actually happened on real
        // hardware - see the note below).
        bitmap_layer_set_bitmap(s_background_layer, NULL);
        gbitmap_destroy(s_background_bitmap);
        s_background_bitmap = NULL;
    }
    GBitmap *new_bitmap = gbitmap_create_with_resource(WALLPAPER_RESOURCE_IDS[index]);
    if (!new_bitmap) {
        // Decode failed (out of memory, or a corrupt/oversized resource) -
        // this happened on real hardware once with a 24-bit truecolor PNG
        // here (firmware logged "gbitmap_png.c: PNG memory allocation
        // failed"); the wallpaper PNGs are now re-encoded to Pebble's own
        // native 64-color palette, which should avoid it going forward.
        // The layer's bitmap was already cleared to NULL above, so this
        // just leaves the wallpaper blank (falling through to the plain
        // black window background) rather than a dangling pointer - and
        // logs loudly so it's obvious why, instead of failing silently.
        APP_LOG(APP_LOG_LEVEL_ERROR,
                "set_background_wallpaper: failed to load wallpaper index %d (decode/OOM)", index);
        return;
    }
    s_background_bitmap = new_bitmap;
    bitmap_layer_set_bitmap(s_background_layer, s_background_bitmap);
    layer_mark_dirty(bitmap_layer_get_layer(s_background_layer));
}

// ============================================================================
// HOUSING PANEL (behind the tiles + weather strip)
// ============================================================================

// There is no housing bitmap, BitmapLayer, or update_proc here anymore -
// this section is now just geometry (housing_frame, computed in
// main_window_load() and used to align the stats panel below it) plus this
// comment explaining where the actual pixels went.
//
// ORIGINALLY: the housing panel was its own CLOCK_ISLAND bitmap resource (a
// real ~66%-opacity black PNG), drawn via a BitmapLayer with GCompOpSet
// compositing over whichever wallpaper was loaded. That compositing mode
// choice mattered: Pebble's shape fills (graphics_fill_rect() etc.) do NOT
// alpha-blend against existing framebuffer content - the SDK docs are
// explicit that the alpha channel "only affects the bitmap drawing
// operations... it currently does not affect the filling or stroking
// operations" - so a translucent GColor fill rendered identically to an
// opaque one. Bitmap compositing (GCompOpSet) is different: it DOES respect
// per-pixel alpha, so as long as CLOCK_ISLAND's own alpha channel was
// genuinely translucent, this was real ~66% opacity, not an approximation.
//
// THE HOUSING PANEL IS NOW BAKED INTO THE WALLPAPERS: since the housing
// always sits directly on top of the wallpaper and nothing else - the
// tiles/weather/text all draw ABOVE both of them, and the housing's own
// position/size never changes at runtime - alpha-compositing CLOCK_ISLAND
// over each wallpaper is fully reproducible offline, pixel for pixel. All 6
// `resources/images/htc_wallpaper0N.png` files now have the housing panel
// pre-blended directly into them (at the same 64-color/4-level-alpha
// treatment described in "Fixed: PNG decode out-of-memory" above), so
// loading the current wallpaper is now the ONLY bitmap load needed to show
// both - CLOCK_ISLAND itself is gone from resources/images/ and
// package.json entirely, and with it an entire GBitmap (~24.8KB decoded)
// that used to sit permanently resident for the app's whole lifetime on top
// of whichever wallpaper was also loaded. Net effect: same pixels on
// screen, one fewer bitmap in memory at all times, and (since HTC_WALLPAPER
// files still went through the same lossless `optipng` pass as everything
// else) most of the wallpaper files came out the same size or SMALLER
// than the un-blended versions - the housing panel's flat ~66%-dark
// overlay tends to reduce local color variance in that region of an
// already-busy wallpaper, which compresses better, not worse.
//
// SIZE / ALIGNMENT, FIXED (was 188px wide vs. CLOCK_ISLAND's original
// 192px): figuring out the exact bake position for the FIRST version of
// this baking pass surfaced a real pre-existing quirk, worth recording
// since it's not obvious from the Pebble docs alone. housing_frame is
// 188x129 (content_w + HOUSING_PAD*2 x HOUSING_H = 172+16 x 129), but the
// original CLOCK_ISLAND art was authored at 192x129 - 4px wider than the
// frame it was ever actually drawn into. Pebble's BitmapLayer does NOT
// auto-scale a mismatched bitmap to its frame, and (checked against
// Pebble's own bitmap_layer.c source, since the hosted docs don't spell
// this out) defaults to GAlignTopLeft, not GAlignCenter, when
// bitmap_layer_set_alignment() is never called - which this project never
// did. Combined with graphics_draw_bitmap_in_rect()'s documented behavior
// of clipping (not scaling) a bitmap larger than its target rect, the real
// on-device result was CLOCK_ISLAND anchored at the frame's top-left
// corner with its rightmost 4 columns silently clipped off - not centered,
// and not resampled. The first bake reproduced that exact crop (matching
// what was already rendering rather than accidentally changing it), and I
// called the resulting 4px asymmetry "invisible in practice" - wrong: you
// noticed it right away once a real background was behind it, showing up
// as a visibly uneven margin between the panel's right edge and the
// screen edge versus the left. Fixed properly this time by re-authoring
// CLOCK_ISLAND at the correct 188x129 straight away (no crop needed at
// all now - the source art matches housing_frame exactly), then re-baking
// all 6 wallpapers against it.

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

// Forward declaration: defined down near create_stat_entry(), but
// update_stats_panel() below needs it to keep each of the five stats-row
// shadow layers' black copy in sync with its real (white) layer's text -
// see SHADOW_OFFSET.
static void set_shadowed_text(TextLayer *shadow, TextLayer *real, const char *text);

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

// Weekday name (full) and "DD Mmm" (abbreviated month) for the right-hand
// weather column.
//
// CORRECTION: this used to be built with strftime("%A")/strftime("%b"),
// on the assumption that Pebble's strftime() is locale-aware. You caught
// on real hardware that it isn't - changing the watch's language did
// nothing to the day/date text. That claim turned out to be wrong; per
// our earlier PebbleMetroWP8 watchface (which solved this exact problem),
// strftime()'s weekday/month names are NOT tied to the watch's live
// system language at all - they're always English, regardless of locale.
// PebbleMetroWP8's fix was a small hand-built name table matched against
// i18n_get_system_locale() (the watch's OWN current locale, e.g. "en_US",
// "nl_NL" - a real, live Pebble API, unrelated to strftime), and this is
// the same fix applied here. Unlike that project, GOTHIC_18_BOLD is a
// Pebble SYSTEM font here (not a custom-baked one with an ASCII-only
// glyph subset), so there's no need to strip accents or abbreviate
// weekday names down to 3 letters - full, properly-accented names are
// used directly.
typedef struct {
    const char *lang_prefix;   // matched against the start of i18n_get_system_locale()
    const char *weekdays[7];   // indexed by tm_wday: Sun..Sat, full names
    const char *months[12];    // indexed by tm_mon: Jan..Dec, abbreviated
} LocaleDateNames;

static const LocaleDateNames LOCALE_DATE_TABLE[] = {
    { "en", { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
            { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" } },
    { "fr", { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" },
            { "Jan", "Fév", "Mar", "Avr", "Mai", "Juin", "Juil", "Aoû", "Sep", "Oct", "Nov", "Déc" } },
    { "de", { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag" },
            { "Jan", "Feb", "Mär", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez" } },
    { "es", { "Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado" },
            { "Ene", "Feb", "Mar", "Abr", "May", "Jun", "Jul", "Ago", "Sep", "Oct", "Nov", "Dic" } },
    { "it", { "Domenica", "Lunedì", "Martedì", "Mercoledì", "Giovedì", "Venerdì", "Sabato" },
            { "Gen", "Feb", "Mar", "Apr", "Mag", "Giu", "Lug", "Ago", "Set", "Ott", "Nov", "Dic" } },
    { "nl", { "Zondag", "Maandag", "Dinsdag", "Woensdag", "Donderdag", "Vrijdag", "Zaterdag" },
            { "Jan", "Feb", "Mrt", "Apr", "Mei", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dec" } },
    { "pt", { "Domingo", "Segunda", "Terça", "Quarta", "Quinta", "Sexta", "Sábado" },
            { "Jan", "Fev", "Mar", "Abr", "Mai", "Jun", "Jul", "Ago", "Set", "Out", "Nov", "Dez" } },
};
#define LOCALE_DATE_TABLE_COUNT (sizeof(LOCALE_DATE_TABLE) / sizeof(LOCALE_DATE_TABLE[0]))

// Returns the name table to use for the watch's current locale, matched
// by language prefix (e.g. "nl" matches both "nl_NL" and "nl_BE").
// Falls back to English for any language not in the table above.
static const LocaleDateNames *locale_date_names(void) {
    const char *locale = i18n_get_system_locale();
    for (size_t i = 0; i < LOCALE_DATE_TABLE_COUNT; i++) {
        size_t prefix_len = strlen(LOCALE_DATE_TABLE[i].lang_prefix);
        if (strncmp(locale, LOCALE_DATE_TABLE[i].lang_prefix, prefix_len) == 0) {
            return &LOCALE_DATE_TABLE[i];
        }
    }
    return &LOCALE_DATE_TABLE[0]; // fallback: English
}

// Only refreshed at minute granularity like everything else here - the
// date only actually changes once a day, but re-checking every tick is
// negligible cost and means a language change takes effect on the very
// next minute tick rather than needing a relaunch.
static void update_date_layout(struct tm *tick_time) {
    if (!s_day_layer || !s_date_layer) return;

    const LocaleDateNames *names = locale_date_names();

    static char date_buf[24];
    snprintf(date_buf, sizeof(date_buf), "%02d %s", tick_time->tm_mday, names->months[tick_time->tm_mon]);

    // Weekday name points directly into the static const table above - no
    // buffer copy needed, that string literal's storage lives for the
    // entire program, same as any other string literal.
    text_layer_set_text(s_day_layer, names->weekdays[tick_time->tm_wday]);
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

    // Refresh weather every settings.weather_update_interval_min minutes
    // (Clay "Weather update interval" select - 15/30/60, default 30). Guard
    // against a corrupt/unset value (e.g. 0, which would make % a divide-by-
    // zero) by falling back to 30 - this mirrors the same defensive pattern
    // inbox_received_callback() uses when validating the incoming tuple.
    int interval_min = settings.weather_update_interval_min;
    if (interval_min != 15 && interval_min != 30 && interval_min != 60) {
        interval_min = 30;
    }
    if (tick_time->tm_min % interval_min == 0) {
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

// RAINDROPS overlay visibility - loads/frees the (full-screen-sized) bitmap
// on demand rather than keeping it resident for the whole app lifetime,
// same memory-conscious pattern set_background_wallpaper() already uses
// for the wallpaper bitmaps (only what's currently needed stays in
// memory). Safe to call repeatedly with the same value.
static void set_raindrops_visible(bool visible) {
    if (!s_raindrops_layer) return;
    Layer *layer = bitmap_layer_get_layer(s_raindrops_layer);
    if (visible) {
        if (!s_raindrops_bitmap) {
            s_raindrops_bitmap = gbitmap_create_with_resource(RESOURCE_ID_RAINDROPS);
            if (!s_raindrops_bitmap) {
                APP_LOG(APP_LOG_LEVEL_ERROR, "set_raindrops_visible: failed to load RAINDROPS (decode/OOM)");
                return;   // leave the layer hidden rather than showing a blank bitmap
            }
            bitmap_layer_set_bitmap(s_raindrops_layer, s_raindrops_bitmap);
        }
        layer_set_hidden(layer, false);
    } else {
        layer_set_hidden(layer, true);
        if (s_raindrops_bitmap) {
            bitmap_layer_set_bitmap(s_raindrops_layer, NULL);
            gbitmap_destroy(s_raindrops_bitmap);
            s_raindrops_bitmap = NULL;
        }
    }
}

// Weather icon - lazy-loaded/freed the same way, and for the same reason:
// this used to preload all 9 icons at once and keep them ALL resident for
// the app's entire lifetime ("cheap enough" was the assumption when there
// were only 9 small 48x48 icons and nothing else competing for heap).
// That assumption didn't hold up once the wallpaper-OOM investigation
// showed real hardware genuinely running out of heap decoding a single
// full-screen bitmap - 9 permanently-resident icons is exactly the kind
// of fixed baseline memory that eats into the headroom a wallpaper/font
// swap needs. Only the CURRENTLY-shown icon is kept loaded now, same
// pattern as set_background_wallpaper()/set_raindrops_visible(). Skips
// the reload entirely if the requested icon is already the one loaded
// (the common case - weather rarely changes between refreshes).
static void set_weather_icon_bitmap(int icon) {
    if (icon < 0 || icon > 8) icon = 4;
    if (!s_weather_icon_layer) return;
    if (icon == s_weather_icon_bitmap_index && s_weather_icon_bitmap) return;

    if (s_weather_icon_bitmap) {
        // Clear the layer's reference before freeing, same dangling-pointer
        // fix applied to set_background_wallpaper() - matters here even
        // more now, since a failed decode below is no longer a rare event.
        bitmap_layer_set_bitmap(s_weather_icon_layer, NULL);
        gbitmap_destroy(s_weather_icon_bitmap);
        s_weather_icon_bitmap = NULL;
        s_weather_icon_bitmap_index = -1;
    }

    GBitmap *new_bitmap = gbitmap_create_with_resource(WEATHER_RESOURCE_IDS[icon]);
    if (!new_bitmap) {
        APP_LOG(APP_LOG_LEVEL_ERROR,
                "set_weather_icon_bitmap: failed to load weather icon %d (decode/OOM)", icon);
        return;   // layer's bitmap is already NULL from above - leaves it blank, not dangling
    }
    s_weather_icon_bitmap = new_bitmap;
    s_weather_icon_bitmap_index = icon;
    bitmap_layer_set_bitmap(s_weather_icon_layer, s_weather_icon_bitmap);
}

// s_temperature/s_temp_high/s_temp_low are always Celsius (see the note on
// s_temperature above) - this is the one place that converts for display.
// Integer-only rounding (avoiding round()/roundf(), which Pebble's libc
// doesn't reliably provide): F = C*9/5 + 32, rounded to the nearest
// integer rather than truncated toward zero. Plain C integer division
// (tenths / 5) always truncates toward zero, which under-rounds every
// negative, non-multiple-of-5 case (e.g. -38/5 would truncate to -7 when
// the true value -7.6 should round to -8) - nudging the numerator by ±2
// before dividing, with the sign matched to tenths, corrects for that.
static int celsius_to_fahrenheit(int c) {
    int tenths = c * 9;
    int rounded = (tenths >= 0) ? (tenths + 2) / 5 : (tenths - 2) / 5;
    return rounded + 32;
}

static int display_temp(int celsius) {
    return settings.use_fahrenheit ? celsius_to_fahrenheit(celsius) : celsius;
}

static void update_weather_layout(void) {
    if (!s_has_weather) {
        text_layer_set_text(s_location_layer, "");
        text_layer_set_text(s_temp_layer, "");
        // No weather data yet - definitely not raining as far as we know.
        set_raindrops_visible(false);
        return;
    }

    static char temp_buf[8];
    snprintf(temp_buf, sizeof(temp_buf), "%d°%s", display_temp(s_temperature),
              settings.use_fahrenheit ? "F" : "C");
    text_layer_set_text(s_temp_layer, temp_buf);
    text_layer_set_text(s_location_layer, s_location_name);

    int icon = s_weather_icon;
    if (icon < 0 || icon > 8) icon = 4;
    set_weather_icon_bitmap(icon);

    // RAINDROPS overlay: visible only while BOTH the user's toggle is on
    // (settings.raindrops_enabled, Clay "Show raindrops in rain" - default
    // on) AND the (clamped, same as what's actually drawn above) icon
    // index is WEATHER_ICON_RAIN. Hidden for every other condition, or
    // whenever the toggle is off regardless of weathercode.
    set_raindrops_visible(settings.raindrops_enabled && icon == WEATHER_ICON_RAIN);
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
// change recalculating row 1's H/L values) - it just re-formats and
// re-assigns text, no layout/geometry work happens here.
static void update_stats_panel(void) {
    // Row 1: today's high/low - s_temp_high/s_temp_low are always Celsius
    // (see the note on s_temperature above); display_temp() converts for
    // display the same way the current-conditions temp does, so this stays
    // in sync with the Clay "Use Fahrenheit" toggle immediately, not just
    // the unit letter.
    static char hilo_buf[24];
    if (s_has_weather) {
        snprintf(hilo_buf, sizeof(hilo_buf), "H:%d° L:%d°%s",
                  display_temp(s_temp_high), display_temp(s_temp_low),
                  settings.use_fahrenheit ? "F" : "C");
    } else {
        snprintf(hilo_buf, sizeof(hilo_buf), "--");
    }
    set_shadowed_text(s_stats_temp_hilo_shadow_layer, s_stats_temp_hilo_layer, hilo_buf);

    // Row 2, left: steps.
    static char steps_buf[16];
    int steps;
    if (health_metric_today(HealthMetricStepCount, &steps)) {
        snprintf(steps_buf, sizeof(steps_buf), "%d", steps);
    } else {
        snprintf(steps_buf, sizeof(steps_buf), "--");
    }
    set_shadowed_text(s_stats_steps_shadow_layer, s_stats_steps_layer, steps_buf);

    // Row 2, right: sleep total, formatted as "7h 32m".
    static char sleep_buf[16];
    int sleep_seconds;
    if (health_metric_today(HealthMetricSleepSeconds, &sleep_seconds)) {
        format_sleep_duration(sleep_buf, sizeof(sleep_buf), sleep_seconds);
    } else {
        snprintf(sleep_buf, sizeof(sleep_buf), "--");
    }
    set_shadowed_text(s_stats_sleep_shadow_layer, s_stats_sleep_layer, sleep_buf);

    // Row 3, left: watch battery - BatteryStateService, on-watch, always
    // available (no accessibility check needed, unlike Health metrics).
    static char watch_batt_buf[8];
    BatteryChargeState watch_batt = battery_state_service_peek();
    snprintf(watch_batt_buf, sizeof(watch_batt_buf), "%d%%", watch_batt.charge_percent);
    set_shadowed_text(s_stats_watch_battery_shadow_layer, s_stats_watch_battery_layer, watch_batt_buf);

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
                  s_phone_charging ? "↑" : "");
    } else {
        snprintf(phone_batt_buf, sizeof(phone_batt_buf), "--");
    }
    set_shadowed_text(s_stats_phone_battery_shadow_layer, s_stats_phone_battery_layer, phone_batt_buf);
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

// Forward declaration: defined down near create_tile()/main_window_load(),
// but inbox_received_callback() below needs to call it live when the
// BoldClockFont Clay toggle changes.
static void set_flip_font(bool bold);

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
        // Cache the fresh reading so the NEXT time this watchface loads
        // (e.g. after a trip to the app menu) it can paint immediately
        // from this instead of blanking until a new AppMessage arrives -
        // see "Weather now persists across watchface reloads" above.
        prv_save_weather_cache();
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

    Tuple *bold_font_tuple = dict_find(iterator, MESSAGE_KEY_BoldClockFont);
    if (bold_font_tuple) {
        bool new_val = tuple_get_int(bold_font_tuple, settings.bold_clock_font ? 1 : 0) == 1;
        APP_LOG(APP_LOG_LEVEL_DEBUG, "settings: BoldClockFont received = %d", new_val);
        if (new_val != settings.bold_clock_font) {
            settings.bold_clock_font = new_val;
            prv_save_settings();
            set_flip_font(new_val);
        }
    }

    Tuple *raindrops_tuple = dict_find(iterator, MESSAGE_KEY_RaindropsEnabled);
    if (raindrops_tuple) {
        bool new_val = tuple_get_int(raindrops_tuple, settings.raindrops_enabled ? 1 : 0) == 1;
        APP_LOG(APP_LOG_LEVEL_DEBUG, "settings: RaindropsEnabled received = %d", new_val);
        if (new_val != settings.raindrops_enabled) {
            settings.raindrops_enabled = new_val;
            prv_save_settings();
            // Re-evaluate immediately against whatever weathercode is
            // already known - update_weather_layout() re-runs the same
            // settings.raindrops_enabled && icon == WEATHER_ICON_RAIN
            // check this toggle just changed, so switching it off hides
            // an active overlay right away rather than waiting for the
            // next weather refresh.
            update_weather_layout();
        }
    }

    Tuple *interval_tuple = dict_find(iterator, MESSAGE_KEY_WeatherUpdateInterval);
    if (interval_tuple) {
        int new_val = tuple_get_int(interval_tuple, settings.weather_update_interval_min);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "settings: WeatherUpdateInterval received = %d", new_val);
        // Only 15/30/60 are ever offered by the Clay select - reject
        // anything else rather than persist a value that would make
        // tick_handler()'s modulo check behave oddly.
        if ((new_val == 15 || new_val == 30 || new_val == 60) &&
            new_val != settings.weather_update_interval_min) {
            settings.weather_update_interval_min = new_val;
            prv_save_settings();
        }
    }

    // Unconditional, and deliberately last: cheap to call, and this way it
    // always reflects the final state of everything above in one message -
    // weather high/low, phone battery, and a UseFahrenheit change (which
    // now recalculates row 1's H/L values via display_temp(), not just the
    // unit letter) all land correctly however many of them arrived together
    // in this same dictionary.
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

// Loads the clock digit font matching settings.bold_clock_font - Semilight
// (FONT_SEGOEUISL_70, default) or Semibold (FONT_SEGOEUISB_70, same
// underlying segoeuisb.ttf file already used at 20px for the stats panel,
// just declared a second time in package.json at 70px). Unloads whatever
// s_flip_font currently points at first (fine to call this at startup too,
// since fonts_unload_custom_font() on a NULL/not-yet-set font is only ever
// reached here after the very first load - see main_window_load()). Marks
// both tiles' flap layers dirty afterward so a live toggle change redraws
// immediately with the new font, without needing a flip animation.
static void set_flip_font(bool bold) {
    if (s_flip_font) {
        fonts_unload_custom_font(s_flip_font);
    }
    s_flip_font = fonts_load_custom_font(resource_get_handle(
        bold ? RESOURCE_ID_FONT_SEGOEUISB_70 : RESOURCE_ID_FONT_SEGOEUISL_70));

    for (int i = 0; i < NUM_TILES; i++) {
        if (s_tiles[i].top_layer) layer_mark_dirty(s_tiles[i].top_layer);
        if (s_tiles[i].bottom_layer) layer_mark_dirty(s_tiles[i].bottom_layer);
    }
}

// Drop-shadow trick for the five stats-panel row texts below the clock
// island ONLY (weather H/L, steps, sleep, watch battery, phone battery) -
// builds a black copy of `frame`, offset SHADOW_OFFSET px down-and-right,
// matching the real layer's font/alignment so its text lines up exactly
// except for the offset. Added to the window BEFORE the caller adds the
// real (white) layer at the same frame, so the shadow paints first and
// the real text sits on top of it, 2px up-and-left. Called from
// create_stat_entry() below, once per stats-panel entry. Does not set any
// text itself - see set_shadowed_text() below, called from
// update_stats_panel() alongside the existing text_layer_set_text() calls
// on the real layers.
static TextLayer *create_shadow_text_layer(GRect frame, GFont font, GTextAlignment alignment) {
    GRect shadow_frame = GRect(frame.origin.x + SHADOW_OFFSET, frame.origin.y + SHADOW_OFFSET,
                                frame.size.w, frame.size.h);
    TextLayer *shadow = text_layer_create(shadow_frame);
    text_layer_set_background_color(shadow, GColorClear);
    text_layer_set_text_color(shadow, GColorBlack);
    text_layer_set_font(shadow, font);
    text_layer_set_text_alignment(shadow, alignment);
    layer_add_child(s_window_layer, text_layer_get_layer(shadow));
    return shadow;
}

// Sets the same text on a shadow layer and its real layer together - used
// everywhere one of the five shadowed stats-row layers' text is set, so
// the two never drift out of sync. Either pointer may be NULL, though in
// practice both are always set together by create_stat_entry().
static void set_shadowed_text(TextLayer *shadow, TextLayer *real, const char *text) {
    if (shadow) text_layer_set_text(shadow, text);
    if (real) text_layer_set_text(real, text);
}

// Builds one stats-panel entry: a STATS_ICON_SIZE-square icon at (x, y),
// with a text layer immediately to its right filling the rest of
// total_w (used for all five icon+text pairs - row 1's full-width weather
// entry, and the two half-width entries on each of rows 2 and 3). Also
// builds that text layer's drop-shadow companion (see SHADOW_OFFSET) -
// an identical black copy added to the window FIRST, so the real (white)
// text layer added right after it paints on top. The icon itself is not
// shadowed (you're handling those with a border yourself).
static void create_stat_entry(BitmapLayer **icon_layer_out, GBitmap **icon_bitmap_out,
                               uint32_t icon_resource_id,
                               TextLayer **shadow_text_layer_out, TextLayer **text_layer_out,
                               int x, int y, int total_w) {
    *icon_layer_out = bitmap_layer_create(GRect(x, y, STATS_ICON_SIZE, STATS_ICON_SIZE));
    bitmap_layer_set_compositing_mode(*icon_layer_out, GCompOpSet);
    *icon_bitmap_out = gbitmap_create_with_resource(icon_resource_id);
    bitmap_layer_set_bitmap(*icon_layer_out, *icon_bitmap_out);
    layer_add_child(s_window_layer, bitmap_layer_get_layer(*icon_layer_out));

    int text_x = x + STATS_ICON_SIZE + STATS_ICON_GAP;
    int text_w = total_w - STATS_ICON_SIZE - STATS_ICON_GAP;
    GRect text_frame = GRect(text_x, y, text_w, STATS_ROW_H);

    *shadow_text_layer_out = create_shadow_text_layer(text_frame, s_stats_font, GTextAlignmentLeft);
    text_layer_set_overflow_mode(*shadow_text_layer_out, GTextOverflowModeTrailingEllipsis);

    *text_layer_out = text_layer_create(text_frame);
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

    // Custom font: Segoe UI Semilight or Semibold at 70px, in place of
    // SonySketchEF - see the FONT note at the top of this file. Which one
    // loads is user-configurable (Clay "Bold clock digits" toggle,
    // settings.bold_clock_font) - see set_flip_font() above. To go back to
    // the very first font tried here, swap that for RESOURCE_ID_SONY_SKETCH_70
    // (that resource is still declared in package.json).
    set_flip_font(settings.bold_clock_font);

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

    // Background wallpaper - added FIRST, so it's the very bottom-most
    // layer (see BACKGROUND WALLPAPER section above). Covers the full
    // screen; the actual bitmap is loaded by set_background_wallpaper()
    // based on the persisted setting. This single bitmap now ALSO carries
    // the housing panel - see "The housing panel is now baked into the
    // wallpapers" below - so there's no separate housing layer added after
    // this one the way there used to be.
    s_background_layer = bitmap_layer_create(GRect(0, 0, bounds.size.w, bounds.size.h));
    layer_add_child(s_window_layer, bitmap_layer_get_layer(s_background_layer));
    set_background_wallpaper(settings.wallpaper_index);

    // housing_frame: NOT a bitmap/layer anymore (see the HOUSING PANEL
    // section above) - kept purely as a geometry reference. The stats panel
    // below still aligns itself to the housing's own left/right edges and
    // bottom edge (housing_frame.origin.x/size.w and
    // housing_frame.origin.y + HOUSING_H, used further down this function),
    // which is exactly what this GRect describes - just without ever
    // creating a bitmap for it anymore, since that same box of pixels is
    // now already part of whichever wallpaper is loaded above. SIZE NOTE:
    // the height still comes from the fixed HOUSING_H constant (matching
    // the housing source art's 188x129px size exactly - see "The housing
    // panel is now baked into the wallpapers" for the earlier 192-vs-188
    // mismatch this replaced) rather than being derived from the tile/
    // weather geometry above, so it stays correctly matched to how the
    // wallpapers were actually baked even as TILE_H etc. get tuned - if
    // that ever changes, all 6 wallpaper images need re-baking to match,
    // not just this constant.
    GRect housing_frame = GRect(
        x0 - HOUSING_PAD,
        TILE_Y - HOUSING_PAD,
        content_w + HOUSING_PAD * 2,
        HOUSING_H
    );

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
    //
    // WEEKDAY WIDTH: unlike DATE below it (which keeps right_x/right_w,
    // stopping short of the icon), the weekday box's LEFT edge now starts
    // at screen center - i.e. it runs from behind the weather icon all the
    // way to the tile row's right edge - mirroring what LOCATION already
    // does on the other side (see the comment on s_location_layer above).
    // Text stays right-aligned (GTextAlignmentRight, unchanged), so normal-
    // length weekday names still land exactly where they used to; only a
    // long localized name grows leftward into the extra room, behind the
    // icon. That only reads correctly because the weather icon layer is
    // created last/topmost below, so it paints over whatever part of this
    // text happens to sit underneath it - same reasoning as LOCATION's
    // overlap, just mirrored to the opposite side and line.
    int day_x = bounds.size.w / 2;
    int day_w = tile_row_right - day_x;
    s_day_layer = text_layer_create(GRect(day_x, weather_y + WEATHER_TOP_LINE_NUDGE, day_w, line_h));
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

    // Weather icon bitmap itself is lazy-loaded by set_weather_icon_bitmap()
    // (called from update_weather_layout() once real weather data arrives)
    // rather than preloaded here - see the comment on that function.

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
                       &s_stats_temp_hilo_shadow_layer, &s_stats_temp_hilo_layer,
                       stats_left, stats_row1_y, stats_w);

    s_stats_divider1_layer = layer_create(GRect(stats_divider_x, stats_divider1_y, STATS_DIVIDER_W, STATS_DIVIDER_H));
    layer_set_update_proc(s_stats_divider1_layer, stats_divider_update_proc);
    layer_add_child(s_window_layer, s_stats_divider1_layer);

    // Row 2: STEPS (left half) + SLEEP (right half) - both read straight
    // off the watch via HealthService, see update_stats_panel().
    create_stat_entry(&s_stats_steps_icon_layer, &s_stats_steps_icon_bitmap, RESOURCE_ID_STEPS,
                       &s_stats_steps_shadow_layer, &s_stats_steps_layer,
                       stats_left, stats_row2_y, stats_half_w);
    create_stat_entry(&s_stats_sleep_icon_layer, &s_stats_sleep_icon_bitmap, RESOURCE_ID_SLEEP,
                       &s_stats_sleep_shadow_layer, &s_stats_sleep_layer,
                       stats_left + stats_half_w, stats_row2_y, stats_half_w);

    s_stats_divider2_layer = layer_create(GRect(stats_divider_x, stats_divider2_y, STATS_DIVIDER_W, STATS_DIVIDER_H));
    layer_set_update_proc(s_stats_divider2_layer, stats_divider_update_proc);
    layer_add_child(s_window_layer, s_stats_divider2_layer);

    // Row 3: WATCH battery (left half, BatteryStateService) + PHONE
    // battery (right half, AppMessage from the phone's own JS).
    create_stat_entry(&s_stats_watch_icon_layer, &s_stats_watch_icon_bitmap, RESOURCE_ID_WATCH,
                       &s_stats_watch_battery_shadow_layer, &s_stats_watch_battery_layer,
                       stats_left, stats_row3_y, stats_half_w);
    create_stat_entry(&s_stats_phone_icon_layer, &s_stats_phone_icon_bitmap, RESOURCE_ID_PHONE,
                       &s_stats_phone_battery_shadow_layer, &s_stats_phone_battery_layer,
                       stats_left + stats_half_w, stats_row3_y, stats_half_w);

    // RAINDROPS overlay - created and added dead LAST, after every other
    // layer in this entire function (background, housing, tiles, AM/PM
    // label, weather strip, weather icon, stats panel - all of it), so
    // it's the topmost layer in the whole window and can draw over
    // absolutely everything else. Full-screen, same GRect as the
    // background wallpaper. The layer itself is created empty/hidden here;
    // its (full-screen-sized) bitmap is only actually loaded on demand by
    // set_raindrops_visible() - see that function for why - which
    // update_weather_layout() below calls to set the real initial state.
    s_raindrops_layer = bitmap_layer_create(GRect(0, 0, bounds.size.w, bounds.size.h));
    bitmap_layer_set_compositing_mode(s_raindrops_layer, GCompOpSet);
    layer_set_hidden(bitmap_layer_get_layer(s_raindrops_layer), true);
    layer_add_child(s_window_layer, bitmap_layer_get_layer(s_raindrops_layer));

    update_tiles(true);
    update_weather_layout();
    // Note: update_tiles(true) above already calls update_stats_panel()
    // internally (see TIME HANDLING section), so the panel is populated
    // as soon as this function returns without a separate call here.
    // update_weather_layout() (called explicitly right above) also sets
    // the raindrops overlay's real initial visibility, replacing the
    // "start hidden" default set when it was created just above.
}

static void main_window_unload(Window *window) {
    for (int i = 0; i < NUM_TILES; i++) {
        layer_destroy(s_tiles[i].top_layer);
        layer_destroy(s_tiles[i].bottom_layer);
        s_tiles[i].top_layer = NULL;
        s_tiles[i].bottom_layer = NULL;
    }
    // No separate housing bitmap/layer to destroy here anymore - baked into
    // s_background_bitmap now, cleaned up by the destroy call right below.
    bitmap_layer_destroy(s_background_layer);
    if (s_background_bitmap) gbitmap_destroy(s_background_bitmap);

    text_layer_destroy(s_ampm_layer);

    text_layer_destroy(s_location_layer);
    text_layer_destroy(s_temp_layer);
    text_layer_destroy(s_day_layer);
    text_layer_destroy(s_date_layer);
    bitmap_layer_destroy(s_weather_icon_layer);
    if (s_weather_icon_bitmap) gbitmap_destroy(s_weather_icon_bitmap);

    // Stats panel
    text_layer_destroy(s_stats_temp_hilo_layer);
    text_layer_destroy(s_stats_temp_hilo_shadow_layer);
    bitmap_layer_destroy(s_stats_weather_icon_layer);
    gbitmap_destroy(s_stats_weather_icon_bitmap);

    text_layer_destroy(s_stats_steps_layer);
    text_layer_destroy(s_stats_steps_shadow_layer);
    bitmap_layer_destroy(s_stats_steps_icon_layer);
    gbitmap_destroy(s_stats_steps_icon_bitmap);

    text_layer_destroy(s_stats_sleep_layer);
    text_layer_destroy(s_stats_sleep_shadow_layer);
    bitmap_layer_destroy(s_stats_sleep_icon_layer);
    gbitmap_destroy(s_stats_sleep_icon_bitmap);

    text_layer_destroy(s_stats_watch_battery_layer);
    text_layer_destroy(s_stats_watch_battery_shadow_layer);
    bitmap_layer_destroy(s_stats_watch_icon_layer);
    gbitmap_destroy(s_stats_watch_icon_bitmap);

    text_layer_destroy(s_stats_phone_battery_layer);
    text_layer_destroy(s_stats_phone_battery_shadow_layer);
    bitmap_layer_destroy(s_stats_phone_icon_layer);
    gbitmap_destroy(s_stats_phone_icon_bitmap);

    layer_destroy(s_stats_divider1_layer);
    layer_destroy(s_stats_divider2_layer);

    bitmap_layer_destroy(s_raindrops_layer);
    if (s_raindrops_bitmap) gbitmap_destroy(s_raindrops_bitmap);

    fonts_unload_custom_font(s_flip_font);
    fonts_unload_custom_font(s_stats_font);
}

// ============================================================================
// APPLICATION LIFECYCLE
// ============================================================================

static void init(void) {
    prv_load_settings();
    // Must happen before window_stack_push() below - main_window_load()
    // calls update_weather_layout()/update_stats_panel() at the very end
    // of itself, and this is what makes those calls have real (if
    // possibly stale) data to show on the very first paint instead of
    // blanking. See "Weather now persists across watchface reloads",
    // just above prv_load_settings()/prv_load_weather_cache() themselves.
    prv_load_weather_cache();

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
