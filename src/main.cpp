//  _   _ ____ _____ _____        _   _   _
// | | | |  _ \_   _|_   _|__ __ | \ | | | |
// | | | | |_) || |   | |/ _ \ V /|  \| | | |
// | |_| |  __/ | |   | | (_) V / | |\  | |_|
//  \___/|_|    |_|   |_|\___/_/  |_| \_| (_)
//
// uptown-f-esp32  (+ plane spotter, reunited)
// Andy Maxwell | github.com/andyhomecode/ads-b-esp32
// 2025 12 25, repurposed 2026 09 07, recombined 2026 09 09
//
// Two feeds on one pair of displays:
//   1. Countdown to the next uptown (northbound) F trains at East Broadway, M14A and M9 busses, and weather alerts.
//   2. If an airliner is low over Brooklyn on final into LGA, the northern-most
//      one in the bounding area -- flight, airline, origin, aircraft type --
//      shown between subway passes.
//
// Same hardware throughout: ESP32-S3 Wroom 1 Dev Board, two Adafruit 14-segment
// LED backpacks (0x70 / 0x71), mode switch on GPIO 13.
//
// for the ESP32-S3 Wroom 1 Dev Board,
// use the COM port, not USB
//
// built on PlatformIO on linux
//
// File layout (top to bottom): Config -> Hardware & globals -> Display / LED
// rendering -> Time parsing -> Trains -> Lookups -> Planes -> Buses ->
// Citi Bike -> Weather -> Quake -> ISS -> the small feeds (airports, aurora,
// Tokyo, air, launches, sports, ...) -> WiFi & config portal -> Setup & loop. Each feed section carries a fetchX() (pulls data into a cache) and a
// showX() (renders the cache); the Display section is the only place that
// touches the LED hardware directly.

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>    // SSDP, to find the Sonos speakers
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <time.h>
#include <string.h>  // strchr, for isoToEpoch
#include <math.h>    // sinf/cosf/atan2f, for the ISS distance check

// LED output
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"

#include <ArduinoJson.h>
#include <map>

// API keys live in include/secrets.h -- gitignored, compiled in, never on
// GitHub (see include/secrets.h.example). Build still works without it; any
// feature that needs a key just stays dark.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef BUSTIME_API_KEY
  #define BUSTIME_API_KEY ""
#endif

//  ____             __ _
// / ___|___  _ __  / _(_) __ _
//| |   / _ \| '_ \| |_| |/ _` |
//| |__| (_) | | | |  _| | (_| |
// \____\___/|_| |_|_| |_|\__, |
//                        |___/

#define SWITCH_PIN 13  // GPIO pin for mode switch

#define SDA 9
#define SCL 18

// What we're watching.
// Stop IDs come from the MTA GTFS feed; East Broadway on the F is "F16".
// In the response, "N" is northbound (uptown / Manhattan- & Queens-bound) and
// "S" is southbound (downtown / Brooklyn-bound). We show both.
#define API_URL_BASE   "https://api.wheresthefuckingtrain.com/by-id/"
#define STOP_ID        "F16"
#define NUM_TRAINS     3      // how many upcoming trains to show, per direction
#define REFETCH_MS     30000  // re-hit the server about this often; the display loops faster

// --- ADS-B: airliners on final into LGA, low over Brooklyn -------------------
// adsb.lol's free API. It now 403s any request with a blank or generic
// User-Agent ("User-Agent too generic; include valid contact info.") -- that is
// exactly what killed the original plane-spotter build on the device -- so every
// request below sends a real UA with contact info.
#define USER_AGENT      "ads-b-esp32/5.8 (+https://github.com/andyhomecode/ads-b-esp32)"

// We fetch a disc (adsb.lol has no free bbox endpoint), then keep only aircraft
// inside a lat/lon box over Brooklyn -- the disc alone reaches the Hudson
// corridor west of Manhattan, so LGA arrivals coming up the river were sneaking
// in. Box: roughly Greenpoint down to Green-Wood, East River across to East NY.
#define ADSB_URL_BASE   "https://api.adsb.lol/v2/point/"
#define ADSB_LAT        "40.6875"     // disc centre (central Brooklyn)
#define ADSB_LON        "-73.9845"
#define ADSB_RADIUS_NM  "6"           // wide enough to cover the box + margin
#define BBOX_LAT_MIN    40.61f        // over-Brooklyn box
#define BBOX_LAT_MAX    40.74f
#define BBOX_LON_MIN   -73.99f        // East River / Brooklyn waterfront -- cuts Manhattan & the Hudson
#define BBOX_LON_MAX   -73.85f
#define ADSB_CATEGORY   "A3"          // A3 == large aircraft (75k-300k lb): airliners
#define ADSB_ALT_MIN    800           // ft -- on final, low over Brooklyn
#define ADSB_ALT_MAX    5000          // ft
#define ADSB_REFETCH_MS 20000         // planes move fast; poll sooner than the trains

// Emergency squawks anywhere near NYC: adsb.lol's per-squawk endpoint returns
// every aircraft worldwide currently squawking that code (usually none, so a
// ~100-byte answer). Checked most-serious first; the first code with a plane
// inside SQK_RADIUS_MI of home wins.
#define SQK_URL_BASE    "https://api.adsb.lol/v2/sqk/"
#define SQK_RADIUS_MI   150.0f
#define SQK_REFETCH_MS  120000        // 2 min

// adsb.lol's route lookup -- the same one the web GUI uses. POST a callsign +
// the plane's current lat/lng; it returns the airports and a `plausible` flag,
// and (being position-aware) resolves the right leg of multi-stop routes. It
// answers an empty 201 unless the request carries a Referer from an adsb.lol
// origin -- a soft anti-abuse gate, hence ROUTE_REFERER below. (adsbdb.com was
// tried here first but its callsign->route table is often stale/wrong -- e.g.
// it had RPA5753 as JFK->CLE when it was really PIT->LGA.)
#define ROUTE_URL      "https://api.adsb.lol/api/0/routeset"
#define ROUTE_REFERER  "https://adsb.lol/"

// --- NWS weather alerts ----------------------------------------------------
// Active watches / warnings / advisories for our point. If the feed carries any
// feature, we show its "event" string (e.g. "Winter Weather Advisory") and
// nothing else. The alert list is at /alerts/active?point=, NOT /points/ (that
// one is just grid metadata and has no alerts).
#define WX_URL          "https://api.weather.gov/alerts/active?point=40.7168,-73.9861"
#define WX_REFETCH_MS   300000        // 5 min -- alerts don't churn

// --- NWS current conditions + short forecast --------------------------------
// Two more api.weather.gov endpoints, same host as the alert feed above, but
// each needs a one-time /points/{lat},{lon} lookup to resolve -- already done
// by hand for our point: KNYC (Central Park) is the nearest observation
// station, and OKX/34,42 is the forecast gridpoint -- so both URLs below are
// hardcoded rather than re-resolved on every boot, same as EQ_URL's Tokyo
// coordinates or the Citi Bike station IDs.
#define WXNOW_URL        "https://api.weather.gov/stations/KNYC/observations/latest"
#define WXNOW_REFETCH_MS 600000       // 10 min -- NWS observations update ~hourly
#define WXFC_URL         "https://api.weather.gov/gridpoints/OKX/34,42/forecast"
#define WXFC_REFETCH_MS  1800000      // 30 min -- forecast text doesn't change that often
#define WXFC_PERIODS     2            // how many forecast periods to show

// --- NYC OEM emergency alerts (Notify NYC / Everbridge CAP feed) -----------
// The live feed linked from the Notify NYC homepage footer -- a genuine CAP
// (Common Alerting Protocol) feed, same vocabulary as NWS. Unlike NWS's single
// JSON GET, this is two hops: the RSS list gives recent messages, and each
// item points at a separate full CAP XML doc where severity/urgency actually
// live. NYC OEM covers everything from a subway delay to a building collapse,
// so unlike NWS we only surface it when capIsHighUrgency() says so. See
// tools/nyc_oem_test.py for how this was proven out, including the full
// source citations (OASIS CAP-feeds v1.0 wrapping spec, CAP v1.2 field spec)
// and the caveat that the CAP-document leg was verified against a synthetic
// doc, not a real one, since NYC had no active alert at test time.
#define OEM_RSS_URL     "https://feeds.everbridge.net/feeds/453003085617722/rss/rss.xml"
#define OEM_REFETCH_MS  300000        // 5 min, same cadence as NWS
// OEM re-sends every alert once per language, and translations are NOT
// reliably older/newer than the English copy in feed order -- a single alert
// seen live had 12 translations sorted ahead of the English item (13
// items total). OEM_MAX_RSS_ITEMS must comfortably cover that so the walk
// below doesn't give up before reaching English. It's cheap to scan that
// many (no HTTP per item -- see oemTitleLooksEnglish()); OEM_MAX_CAP_FETCHES
// separately bounds the expensive part (each candidate needs its own CAP doc
// HTTPS fetch), which normally fires at most once since only the English
// item's RSS title should ever pass the pre-filter.
#define OEM_MAX_RSS_ITEMS   40
#define OEM_MAX_CAP_FETCHES 6

// --- USGS earthquakes near Tokyo ---------------------------------------------
// The 5 most recent M>=4 within 300 km of Tokyo. We only *show* one if it's
// mag >= EQ_MIN_MAG or tsunami-flagged, AND it happened in the last EQ_MAX_AGE_S
// seconds -- needs an NTP clock for that window.
#define EQ_URL          "https://earthquake.usgs.gov/fdsnws/event/1/query?format=geojson&latitude=35.68&longitude=139.77&maxradiuskm=300&minmagnitude=4&orderby=time&limit=5"
#define EQ_MIN_MAG      4.5f
#define EQ_MAX_AGE_S    86400         // 24 h
#define EQ_REFETCH_MS   600000        // 10 min
#define EQ_REF_LAT      35.7528f      // Kita City (Kita-ku) ward office --
#define EQ_REF_LON      139.7336f     // distance/direction is measured from here

// --- ISS overhead ping -----------------------------------------------------
// wheretheiss.at is a free, no-key, single-satellite position API -- one
// small JSON object, no filter needed. Deliberately not a real visible-pass
// predictor (that needs sunlit-satellite/dark-observer math and an API like
// open-notify.org's iss-pass, which has a history of flaky uptime); this is
// just a "something's up there" ping like the plane tracker, straight-line
// distance from home to the ISS's ground point, so it can flash in broad
// daylight same as the middle of the night.
#define ISS_URL         "https://api.wheretheiss.at/v1/satellites/25544"
#define ISS_REFETCH_MS  20000         // it moves at ~7.7 km/s
#define ISS_OVERHEAD_KM 400.0f        // "roughly overhead", not a real horizon calc
#define HOME_LAT        40.7168f      // same point as WX_URL above
#define HOME_LON       -73.9861f

// --- NHC Atlantic tropical storms -------------------------------------------
// NHC's "what's active right now" feed. Unlike Citi Bike this is small --
// usually 0-1 storms worldwide, rarely more than a handful in-season -- so no
// streaming hack needed, just an ArduinoJson filter to drop the GIS/advisory
// sub-objects bundled into every storm entry that we don't use. Only Atlantic
// systems count (`id` starts "al" -- Pacific storms don't threaten NYC), and
// depressions are skipped (NHC hasn't named them yet). This is an ambient
// "something's out there" ping like the Tokyo quake, not a warning -- NWS and
// NYC OEM still own "it's about to hit you", once it's close enough for them
// to say so.
#define NHC_URL          "https://www.nhc.noaa.gov/CurrentStorms.json"
#define NHC_REFETCH_MS   1800000      // 30 min -- advisories update every few hours

// --- MTA BusTime (SIRI stop-monitoring) ----------------------------------
// Upcoming buses at one or more stops. Needs BUSTIME_API_KEY from
// include/secrets.h (free key: https://register.developer.obanyc.com/).
// Empty key -> the whole bus section is skipped.
#define BUS_URL_BASE    "https://bustime.mta.info/api/siri/stop-monitoring-v2.json"
#define BUS_MAX         3            // arrivals shown per stop
#define BUS_REFETCH_MS  30000

// One row per stop. `linePrefix` keeps only matching routes at that stop
// (stops carry more than one line); `disp` is the short route tag shown on
// every arrival frame ("M14 12mn" / "M9 12min").
struct BusFeed {
  const char *stopRef;
  const char *linePrefix;
  const char *disp;
};
const BusFeed BUS_FEEDS[] = {
  { "401150", "M14A", "M14" },  // Grand St/Clinton St, W -> Abingdon Sq
  { "404287", "M9",   "M9"  },  // Essex St/East Broadway, W -> Battery Pk City
};
#define NUM_BUS_FEEDS (sizeof(BUS_FEEDS) / sizeof(BUS_FEEDS[0]))

// --- Citi Bike (GBFS) -----------------------------------------------------
// Clinton St & Grand St got split into two docks at some point: the original
// rack (capacity 54) and a smaller overflow rack added later right next to it
// (short_name "5303.06_", capacity 15) -- both show up separately in GBFS, so
// we track both station_ids and add them together.
//
// GBFS has no per-station query -- station_status.json is a single dump of
// all ~2500 NYC stations (~1 MB). That's far too big to buffer into a String
// or ArduinoJson doc on an ESP32-S3 without PSRAM, so fetchCitibike() streams
// the HTTP response and hand-scans for our two station_id strings instead of
// parsing JSON at all (same reasoning as xmlTag() below: flat text search
// instead of pulling in more parsing machinery than the job needs).
#define CITIBIKE_STATUS_URL "https://gbfs.citibikenyc.com/gbfs/en/station_status.json"
#define CITIBIKE_REFETCH_MS 30000
const char *CITIBIKE_STATION_IDS[] = {
  "66dbc420-0aca-11e7-82f6-3863bb44ef7c",  // main rack, capacity 54
  "2098714441479661752",                    // overflow rack, capacity 15
};
#define NUM_CITIBIKE_STATIONS (sizeof(CITIBIKE_STATION_IDS) / sizeof(CITIBIKE_STATION_IDS[0]))


// _   _                _                          ____ _       _           _
//| | | | __ _ _ __ __| |_      ____ _ _ __ ___   / ___| | ___ | |__   __ _| |___
//| |_| |/ _` | '__/ _` \ \ /\ / / _` | '__/ _ \ | |  _| |/ _ \| '_ \ / _` | / __|
//|  _  | (_| | | | (_| |\ V  V / (_| | | |  __/ | |_| | | (_) | |_) | (_| | \__ \
//|_| |_|\__,_|_|  \__,_| \_/\_/ \__,_|_|  \___|  \____|_|\___/|_.__/ \__,_|_|___/

// instantiate the two i2c LED controllers
Adafruit_AlphaNum4 alpha4_1 = Adafruit_AlphaNum4();
Adafruit_AlphaNum4 alpha4_0 = Adafruit_AlphaNum4();

WebServer server(80);
DNSServer dnsServer;

// Buffer to hold the final HTML page
char htmlPage[2048];


// Replace with default credentials if desired,
// don't need to since it'll spin up an AP and website for you to configure it.
const char *DEFAULT_SSID = "MyWiFi";
const char *DEFAULT_PASSWORD = "MyPassword";

// Timeout for connecting to Wi-Fi
const unsigned long WIFI_TIMEOUT_MS = 20000;

// store settings between sessions
Preferences preferences;

bool g_wifiConnected = false;  // global variable to show WiFi state


//      _ _           _
//   __| (_)___ _ __ | | __ _ _   _
//  / _` | / __| '_ \| |/ _` | | | |
// | (_| | \__ \ |_) | | (_| | |_| |
//  \__,_|_|___/ .__/|_|\__,_|\__, |
//             |_|            |___/
//
// Everything that touches the LED hardware directly. Two verbs: write* does
// one immediate raw write (no delay); show* holds, fades, slides, scrolls or
// blinks before returning -- that's the API feed code below should call.

// Backpack brightness levels (HT16K33 supports 0-15).
#define BRIGHT_FULL 15   // parked frame
#define BRIGHT_DIM   1   // while a frame fades/scrolls in
#define BRIGHT_MIN   0   // dimmest still-lit level -- the loading clock

// Custom 14-segment glyphs, carried through the string/scroll pipeline as
// sentinel bytes: wherever one lands in a frame, writeRawFrame() renders it
// raw instead of as ASCII.
//   downtown -- a down arrowhead "\|/" in the top half   (H + J + K)
//   uptown   -- an up arrowhead   "/|\" in the bottom half (N + M + L)
//   top bar  -- the crest of the tide banner's wave, over '-' and '_'; the bus roof
//   wheel    -- the bus roof over a small 'o'
#define GLYPH_DOWN   (ALPHANUM_SEG_H | ALPHANUM_SEG_J | ALPHANUM_SEG_K)
#define GLYPH_UP     (ALPHANUM_SEG_N | ALPHANUM_SEG_M | ALPHANUM_SEG_L)
#define GLYPH_TOP    ALPHANUM_SEG_A
#define GLYPH_DOWN_CH '\x01'
#define GLYPH_UP_CH   '\x02'
#define GLYPH_TOP_CH  '\x03'
#define GLYPH_WHEEL_CH '\x04'


// Font lookup only -- never begun, so it never touches the bus. The library's
// ASCII font table is file-static, so borrow it through writeDigitAscii().
Adafruit_AlphaNum4 g_fontLookup = Adafruit_AlphaNum4();

// 14-segment bitmask for one character, including the direction-arrow glyphs.
uint16_t glyphFor(char c) {
  if (c == GLYPH_DOWN_CH) return GLYPH_DOWN;
  if (c == GLYPH_UP_CH)   return GLYPH_UP;
  if (c == GLYPH_TOP_CH)  return GLYPH_TOP;
  if (c == GLYPH_WHEEL_CH) return glyphFor('o') | GLYPH_TOP;
  g_fontLookup.writeDigitAscii(0, c);
  return g_fontLookup.displaybuffer[0];
}

// Raw segment bitmasks for all 8 columns -> both backpacks (columns 0-3 on
// the first, 4-7 on the second).
void writeSegs(const uint16_t segs[8]) {
  for (int i = 0; i < 4; i++) {
    alpha4_0.writeDigitRaw(i, segs[i]);
    alpha4_1.writeDigitRaw(i, segs[i + 4]);
  }
  alpha4_0.writeDisplay();
  alpha4_1.writeDisplay();
}

// First 8 characters of text (short text is blank-padded), with the decimal
// point lit on column dPLocation (-1 = none).
void writeRawFrame(String text, int dPLocation = -1) {
  uint16_t segs[8];
  for (int i = 0; i < 8; i++) {
    segs[i] = glyphFor(i < (int)text.length() ? text.charAt(i) : ' ');
    if (i == dPLocation) segs[i] |= ALPHANUM_SEG_DP;
  }
  writeSegs(segs);
}


// What's currently parked on the 8 columns, so the next frame can scroll the
// old data out to the left while the new data scrolls in from the right.
String g_frame = "        ";

// Actual current backpack brightness, so fades start from where we really are
// (e.g. the dim loading clock) instead of snapping to full first.
uint8_t g_bright = BRIGHT_FULL;

void setBrightnessBoth(uint8_t b) {
  g_bright = b;
  alpha4_0.setBrightness(b);
  alpha4_1.setBrightness(b);
}

// Ramp from the current brightness to `to`, one level per stepMs.
void fadeBrightnessBoth(int to, int stepMs) {
  int dir = (to >= g_bright) ? 1 : -1;
  for (int b = g_bright; b != to; b += dir) {
    setBrightnessBoth(b);
    delay(stepMs);
  }
  setBrightnessBoth(to);
}

// Fade down to dim, then scroll the current frame out to the left while
// (the first 8 columns of) `next` slides in from the right. Leaves it parked,
// still dim -- callers decide what brightness to settle at.
void slideIn(String next, int stepMs = 45) {
  next = next.substring(0, 8);
  while (next.length() < 8) next += " ";

  fadeBrightnessBoth(BRIGHT_DIM, 8);

  String buf = g_frame + next;  // 16 columns: old data | new data
  for (int i = 1; i <= 8; i++) {
    writeRawFrame(buf.substring(i, i + 8), -1);
    delay(stepMs);
  }
  g_frame = next;
}

// slideIn(), then fade back up to full and hold for holdMs.
void showFrame(String next, int holdMs, int stepMs = 45) {
  slideIn(next, stepMs);
  fadeBrightnessBoth(BRIGHT_FULL, 14);
  delay(holdMs);
}


void showText(String text, int dpLocation = -1, int holdMs = 2000) {
  if (text.length() <= 8) {
    writeRawFrame(text, dpLocation);
    delay(holdMs);
  } else {
    // Show first 8 characters
    writeRawFrame(text.substring(0, 8), dpLocation);
    delay(holdMs);

    // Scroll until the last character is in the right-most position
    for (int i = 1; i <= text.length() - 8; i++) {
      String frame = text.substring(i, i + 8);
      writeRawFrame(frame, -1);  // No decimal point during scroll
      delay(200);  // Delay between scroll frames
    }
    delay(1000);  // Pause for 1 second at the end
  }
}


// Marquee whatever's past text's first 8 columns across at full brightness,
// then hold the tail a second. No-op for text that already fits.
void marqueeRest(const String &text) {
  if (text.length() <= 8) return;
  for (int i = 1; i <= text.length() - 8; i++) {
    writeRawFrame(text.substring(i, i + 8), -1);
    delay(200);
  }
  g_frame = text.substring(text.length() - 8);
  delay(1000);
}

// A banner that rides all the way through: in from past the right edge,
// across at full brightness, and off the left, pushing whatever was showing
// ahead of it. Leaves the display blank for the next frame to slide in.
void scrollAcross(const String &text, int stepMs = 110) {
  fadeBrightnessBoth(BRIGHT_FULL, 8);
  String buf = g_frame + text + "        ";
  for (int i = 1; i <= (int)buf.length() - 8; i++) {
    writeRawFrame(buf.substring(i, i + 8), -1);
    delay(stepMs);
  }
  g_frame = "        ";
}

// A banner that fades up in place, holds, and fades back out, leaving the
// display blank. (Brightness 0 is still lit, hence the blank frames.)
void fadeInOut(const String &text, int holdMs = 900) {
  fadeBrightnessBoth(0, 8);
  writeRawFrame(text, -1);
  fadeBrightnessBoth(BRIGHT_FULL, 40);
  delay(holdMs);
  fadeBrightnessBoth(0, 40);
  g_frame = "        ";
  writeRawFrame(g_frame, -1);
}

// Like showFrame(), but for content that can run longer than 8 columns
// (plane details, alert text, forecast text, ...): fade down, slide the
// first 8 columns in, fade back up and hold -- then, if there's more text,
// keep marqueeing it across at full brightness before the next field fades
// down in turn. This is the "every non-header field gets a proper
// fade/slide transition, not a hard snap" treatment used everywhere data
// (not a fixed label/tag) is shown -- showText() is for header tags only.
void showFadeFrame(String text, int holdMs = 2000, int stepMs = 45) {
  showFrame(text, holdMs, stepMs);  // first 8 columns
  marqueeRest(text);
}


// Pads text to exactly 8 columns, split evenly on both sides (extra space
// goes right when the pad amount is odd). Used for short static header
// frames that should sit centered rather than crammed against the left.
String center8(const String &s) {
  int pad = 8 - (int)s.length();
  if (pad <= 0) return s.substring(0, 8);
  int left = pad / 2;
  String out;
  for (int i = 0; i < left; i++) out += ' ';
  out += s;
  while (out.length() < 8) out += ' ';
  return out;
}


// Centers a short tag (<= 7 chars, so there's room to move) and jiggles it a
// column left/right a few times before it settles, then holds -- the quake
// banner's "shake". Clamped to the 8 columns, so a 7-char tag like TSUNAMI
// just rocks between its two possible spots.
void shakeText(const String &s, int holdMs = 1200) {
  int pad  = 8 - (int)s.length();
  int home = pad / 2;  // same spot center8() would put it
  int lo = max(home - 1, 0), hi = min(home + 1, pad);
  for (int i = 0; i < 8; i++) {
    int off = (i % 2) ? hi : lo;
    writeRawFrame(String("        ").substring(0, off) + s, -1);
    delay(70);
  }
  g_frame = center8(s);
  writeRawFrame(g_frame, -1);
  delay(holdMs);
}

void blink(bool blinkOn) {

  if (blinkOn) {
    alpha4_0.blinkRate(HT16K33_BLINK_2HZ);
    alpha4_1.blinkRate(HT16K33_BLINK_2HZ);
  } else {
    alpha4_0.blinkRate(HT16K33_BLINK_OFF);
    alpha4_1.blinkRate(HT16K33_BLINK_OFF);
  }
}


// --- CAP alert vocabulary (shared by NWS + NYC OEM) -------------------------
// Both feeds are Common Alerting Protocol messages under the hood -- NWS as
// CAP-in-GeoJSON, NYC OEM as raw CAP XML -- and share the same severity/
// urgency enums straight from the CAP v1.2 spec
// (https://docs.oasis-open.org/emergency/cap/v1.2/CAP-v1.2-os.html):
// severity in {Extreme,Severe,Moderate,Minor,Unknown}, urgency in
// {Immediate,Expected,Future,Past,Unknown}. See tools/nyc_oem_test.py for the
// full source citations.

struct CapAlert {
  String event;
  String headline;
  String severity;
  String urgency;
  String certainty;
  String category;  // CAP <category> -- OEM only; see capIsHighUrgency() below
  long   sent = 0;  // CAP <sent> as a UTC epoch, 0 if missing -- OEM only
};

// Fails OPEN: excludes only known-low values, rather than requiring known-high
// ones. A real active alert with blank/"Unknown" severity or urgency (a CAP
// producer under time pressure not filling every field -- seen for real in
// NWS's own active-alerts list, which currently has a "Test Message" entry
// with severity=Unknown, urgency=Unknown right alongside genuine warnings)
// must still get through; silently dropping a real emergency because a field
// was left blank is worse than showing one that turns out to be routine.
//
// category=="Health" is excluded the same way. Caught live on 2026-09-12: a
// "Public Pool Closure" notice and a real "Basement Preparedness" flood-prep
// advisory both carried identical severity=Severe/urgency=Immediate (NYC's
// Everbridge setup tags nearly everything that way), so severity/urgency
// alone couldn't tell them apart -- but their CAP <category> did: Health vs.
// Geo. See README.md's "NYC OEM alert categories" section for the full CAP
// category table and why this is a denylist (Health only) rather than an
// allowlist of "emergency" categories -- the same fails-open reasoning as
// above applies: NYC's own category tagging doesn't reliably match the CAP
// spec's semantics (that flood-prep alert was Geo, not Met, despite the spec
// listing flood as the example under Met), so guessing every category a real
// emergency might land in is riskier than excluding the one confirmed-noisy
// category.
// Tunable exception: a Moderate-severity alert with urgency=Immediate still
// gets through even though Moderate is normally treated as low. Reasoning:
// NWS/NYC OEM tag Advisory-level products (e.g. Flood *Advisory*, as opposed
// to a Severe-severity Flood *Warning*) as severity=Moderate -- an Advisory
// that's already Immediate is worth showing, while non-Immediate
// Minor/Moderate noise should stay filtered. Set to false to go back to
// filtering out all Minor/Moderate severity regardless of urgency.
#define OEM_MODERATE_IMMEDIATE_PASSES true

// Missing/endangered "Vulnerable Adult" alerts (headline
// "Notify NYC - Missing Vulnerable Adult Alert - <name> (NYC)", confirmed
// live 2026-09-13) carry category=Rescue, severity=Extreme,
// urgency=Immediate -- the same fields a genuine rescue-in-progress would
// carry, so unlike the Health/category split above, severity/urgency/category
// alone can't tell them apart. Blanket-excluding category=="Rescue" would
// risk hiding a real rescue emergency, so this matches on the headline text
// instead, same as the English/ASCII title checks below (oemTitleLooksEnglish).
bool oemIsVulnerablePersonAlert(const String &headline) {
  String h = headline;
  h.toLowerCase();
  return h.indexOf("vulnerable") >= 0;
}

bool capIsHighUrgency(const CapAlert &a) {
  bool severityLow = (a.severity == "Minor" || a.severity == "Moderate");
  if (OEM_MODERATE_IMMEDIATE_PASSES && a.severity == "Moderate" && a.urgency == "Immediate") {
    severityLow = false;
  }
  bool urgencyLow  = (a.urgency  == "Future" || a.urgency  == "Past");
  bool categoryLow = (a.category == "Health");
  bool vulnerable  = oemIsVulnerablePersonAlert(a.headline);
  return !severityLow && !urgencyLow && !categoryLow && !vulnerable;
}

// Shows a CAP alert as a blinking source tag ("NWS", "OEM GEO") followed by
// the event name -- only the source tag blinks, to catch the eye; the event
// text stays steady since blinking while it scrolls makes it hard to read.
// The tag is a fixed short label so it just snaps in (showText); the event
// is content, so it gets the fade/slide/marquee treatment (showFadeFrame).
void showAlert(const char *source, const String &event) {
  if (!event.length()) return;
  setBrightnessBoth(BRIGHT_FULL);
  blink(true);
  showText(source, -1, 1200);
  blink(false);
  showFadeFrame(event, 2500);
}


//  loading clock ---------------------------------------------------------
//  Fetches block loop(), so while a fetch cycle runs the display shows the
//  24h time ("14-05"), dim, with one decimal point stepping along the bottom
//  -- one column per finished HTTP call, bouncing back off either end. The
//  clock digits scramble in through random segment noise, scramble again
//  when the minute ticks over mid-fetch, and scramble out when the pass is
//  done. progReset() at the top of the fetch section each pass,
//  progBegin()/progEnd() around each call, progFinish() after the last one.
//  No NTP clock yet -> just the dot.

static String g_progText = "        ";  // clock frame currently showing
static int    g_progPos  = 0;           // column the dot is on
static int    g_progDir  = 1;           // +1 right, -1 left
static bool   g_progRan  = false;       // clock already up this pass?

String clockFrame() {
  long now = (long)time(nullptr);
  if (now < 1700000000L) return "        ";
  time_t t = (time_t)now;
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d-%02d", tmLocal.tm_hour, tmLocal.tm_min);
  return center8(buf);
}

// Each of the 14 segment bits set with probability `density` (0-1).
uint16_t randomSegs(float density) {
  uint16_t m = 0;
  for (int b = 0; b < 14; b++)
    if (random(1000) < (long)(density * 1000)) m |= (1 << b);
  return m;
}

// Segment static from one frame to another, across all 8 columns: the old
// glyph's segments drop out and the new one's lock in at random, with noise
// peaking halfway, then it settles exactly on `to`. The decimal point on
// dotCol (-1 = none) stays lit throughout.
void noiseMorph(const String &from, const String &to, int dotCol,
                int steps = 12, int stepMs = 35) {
  uint16_t a[8], b[8], segs[8];
  for (int i = 0; i < 8; i++) {
    a[i] = glyphFor(i < (int)from.length() ? from.charAt(i) : ' ');
    b[i] = glyphFor(i < (int)to.length()   ? to.charAt(i)   : ' ');
  }
  for (int s = 1; s <= steps; s++) {
    float p = (float)s / steps;  // 0 -> 1: how far toward `to`
    for (int i = 0; i < 8; i++) {
      segs[i] = (a[i] & randomSegs(1 - p)) | (b[i] & randomSegs(p)) |
                randomSegs(p * (1 - p) * 1.4f);
      if (i == dotCol) segs[i] |= ALPHANUM_SEG_DP;
    }
    writeSegs(segs);
    delay(stepMs);
  }
}

// Like showFadeFrame(), but the first 8 columns scramble in through segment
// noise -- the loading clock's transition -- instead of sliding.
void showNoiseFrame(String text, int holdMs = 2000) {
  String first = text.substring(0, 8);
  while (first.length() < 8) first += " ";
  noiseMorph(g_frame, first, -1);
  g_frame = first;
  fadeBrightnessBoth(BRIGHT_FULL, 14);
  delay(holdMs);
  marqueeRest(text);
}

void progReset() {
  g_progPos = 0;
  g_progDir = 1;
  g_progRan = false;
}

// First call of a pass fades down and scrambles whatever's showing into the
// clock; later ones scramble it again if the minute ticked.
void progBegin() {
  String now = clockFrame();
  if (!g_progRan) {
    g_progRan = true;
    fadeBrightnessBoth(BRIGHT_MIN, 8);
    noiseMorph(g_frame, now, g_progPos);
  } else if (now != g_progText) {
    noiseMorph(g_progText, now, g_progPos);
  }
  g_progText = now;
  g_frame    = now;
}

// `result` ('*' data | '0' none | 'X' error) isn't drawn -- each fetch logs
// its own outcome to Serial. Just steps the dot, bouncing off either end.
void progEnd(char result) {
  (void)result;
  if (g_progPos + g_progDir < 0 || g_progPos + g_progDir > 7) g_progDir = -g_progDir;
  g_progPos += g_progDir;
  writeRawFrame(g_progText, g_progPos);
}

// Scramble the clock out to blank; the next frame slides in over that.
void progFinish() {
  if (!g_progRan) return;
  noiseMorph(g_progText, "        ", -1);
  g_frame = "        ";
}



//  _   _
// | |_(_)_ __ ___   ___
// | __| | '_ ` _ \ / _ \
// | |_| | | | | | |  __/
//  \__|_|_| |_| |_|\___|
//
// The feed hands us absolute timestamps like "2026-09-07T14:30:47-04:00",
// so we need to know "now" to turn them into a countdown.

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// algorithm). Avoids timegm(), which isn't declared in this newlib config.
static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + doe - 719468L;
}

// Parse an ISO-8601 timestamp into a UTC epoch. Handles a trailing "Z", a
// numeric "+HH:MM" / "-HH:MM" offset, and optional fractional seconds
// ("...:09.967-04:00" -- BusTime sends these; the MTA/NWS feeds don't).
// Returns 0 if it can't be parsed.
long isoToEpoch(const char *iso) {
  if (iso == nullptr || iso[0] == '\0') return 0;

  int Y, Mo, D, h, m, s;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &h, &m, &s) < 6) return 0;

  long utc = daysFromCivil(Y, Mo, D) * 86400L + h * 3600L + m * 60L + s;

  // Find the zone designator: scan past the time (and any ".fff") to the first
  // 'Z' / '+' / '-' after the 'T'.
  const char *t = strchr(iso, 'T');
  if (t) {
    const char *z = t + 1;
    while (*z && *z != 'Z' && *z != '+' && *z != '-') z++;
    if (*z == '+' || *z == '-') {
      int tzh = 0, tzm = 0;
      if (sscanf(z + 1, "%d:%d", &tzh, &tzm) >= 1) {
        long offset = (long)tzh * 3600 + (long)tzm * 60;
        utc += (*z == '-') ? offset : -offset;  // "14:30-04:00" is 18:30 UTC
      }
    }
  }
  return utc;
}


// GET url into `out` -- the shared boilerplate for the small feeds.
// Logs "<tag> HTTP <code>" and returns false on anything but 200 (or 206,
// the answer to a Range request). Callers still do their own
// progBegin()/progEnd().
bool httpGet(const char *tag, const String &url, String &out, const char *range = nullptr) {
  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.begin(url);
  if (range) http.addHeader("Range", range);
  int code = http.GET();
  if (code != HTTP_CODE_OK && code != HTTP_CODE_PARTIAL_CONTENT) {
    Serial.printf("%s HTTP %d\n", tag, code);
    http.end();
    return false;
  }
  out = http.getString();
  http.end();
  return true;
}


//  _____             _
// |_   _| __ __ _ ___(_)_ __  ___
//   | || '__/ _` |_  / | '_ \/ __|
//   | || | | (_| |/ /| | | | \__ \
//   |_||_|  \__,_/___|_|_| |_|___/
//
// Countdown to the next F trains at East Broadway, both directions.

// One cached F arrival: absolute epoch + which way it's headed ('U' uptown /
// 'D' downtown). Both directions get merged into one soonest-first list.
struct TrainArrival {
  long epoch;
  char dir;
};

// Up to NUM_TRAINS each way, merged and sorted soonest-first for display.
TrainArrival g_trains[2 * NUM_TRAINS];
int          g_trainCount     = 0;
long         g_trainFetchEpoch  = 0;   // "now" at the moment of the last successful fetch
unsigned long g_lastTrainFetchMs = 0;
bool         g_haveTrainData  = false;

// Hit the JSON proxy and refill g_trains/g_trainCount. Unlike the other
// feeds' RefetchTimer-gated fetches, this keeps retrying every time the
// trains get picked (not just every REFETCH_MS) until the first parse
// succeeds, since a stale/empty countdown is worse than an extra request.
void fetchTrains() {
  if (g_haveTrainData && millis() - g_lastTrainFetchMs < REFETCH_MS) return;

  progBegin();
  // Make HTTP GET to the MTA JSON proxy
  HTTPClient http;
  http.begin(String(API_URL_BASE) + STOP_ID);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println(payload);

    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("JSON parse error: ");
      Serial.println(error.c_str());
      progEnd('X');
      blink(true);
    } else {
      blink(false);

      // "now" from NTP, or fall back to the feed's own update time
      long nowEpoch = (long)time(nullptr);
      if (nowEpoch < 1700000000L) {
        nowEpoch = isoToEpoch(doc["updated"] | "");
      }

      // N == northbound (uptown), S == southbound (downtown / Brooklyn).
      // Take up to NUM_TRAINS from each, tagged with direction; the feed
      // occasionally lists a stray non-F route here, so keep F only.
      g_trainCount = 0;
      struct { const char *key; char dir; } dirs[] = {{"N", 'U'}, {"S", 'D'}};
      for (auto &d : dirs) {
        int added = 0;
        for (JsonObject t : doc["data"][0][d.key].as<JsonArray>()) {
          if (added >= NUM_TRAINS) break;
          if (String(t["route"] | "") != "F") continue;
          long e = isoToEpoch(t["time"] | "");
          if (e > 0) {
            g_trains[g_trainCount].epoch = e;
            g_trains[g_trainCount].dir   = d.dir;
            g_trainCount++;
            added++;
          }
        }
      }

      // Sort soonest-first (tiny list, plain insertion sort).
      for (int a = 1; a < g_trainCount; a++) {
        TrainArrival cur = g_trains[a];
        int b = a - 1;
        while (b >= 0 && g_trains[b].epoch > cur.epoch) {
          g_trains[b + 1] = g_trains[b];
          b--;
        }
        g_trains[b + 1] = cur;
      }

      g_trainFetchEpoch  = nowEpoch;
      g_lastTrainFetchMs = millis();
      g_haveTrainData    = true;
      progEnd(g_trainCount > 0 ? '*' : '0');
    }
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
    progEnd('X');
    showText("HTTP " + String(httpCode));
    blink(true);
    ESP.restart();  // oh well
  }
  http.end();
}

// The whole train display pass: one "E B'WAY" station frame, then every cached
// arrival (both directions, already sorted soonest-first) as "Fv YYmin" (~2s) /
// "Fv  NOW" when it's basically here -- the glyph after the F is the direction
// (down arrowhead "\|/" up top = downtown, up arrowhead "/|\" low = uptown).
// A single "NO F TRN" if nothing's running.
bool showArrivals(long nowEpoch) {
  if (!g_haveTrainData) return false;
  showFrame("E B'WAY", 500);

  if (g_trainCount == 0) {
    showFrame("NO F TRN", 1400);
    return true;
  }

  for (int i = 0; i < g_trainCount; i++) {
    long mins = (g_trains[i].epoch - nowEpoch + 30) / 60;
    char dirCh = (g_trains[i].dir == 'U') ? GLYPH_UP_CH : GLYPH_DOWN_CH;

    char frame[12];
    if (mins <= 0) {
      snprintf(frame, sizeof(frame), "F%c  NOW", dirCh);
    } else {
      if (mins > 99) mins = 99;
      snprintf(frame, sizeof(frame), "F%c %2ldmin", dirCh, mins);
    }
    showFrame(frame, 1300);
  }
  return true;
}


// _                 _
//| |    ___   ___  | | ___   _ _ __  ___
//| |   / _ \ / _ \ | |/ / | | | '_ \/ __|
//| |__| (_) | (_) | |   <| |_| | |_) \__ \
//|_____\___/ \___/  |_|\_\\__,_| .__/|___/
//                              |_|
//
// ICAO airline + aircraft-type codes -> friendly names. Offline and punchy;
// covers what actually flies the LGA approach. Anything not here shows as
// "Unknown" (the route API gives airports, not a friendly airline name).
std::map<String, String> airlineLookup;
std::map<String, String> icacoLookup;

void initLookups() {
  // Populate airline lookup
  airlineLookup["AAL"] = "American";
  airlineLookup["DAL"] = "Delta";
  airlineLookup["UAL"] = "United";
  airlineLookup["JBU"] = "JetBlue";
  airlineLookup["SWA"] = "South West";
  airlineLookup["ACA"] = "Air Canada";
  airlineLookup["NKS"] = "Spirit";
  airlineLookup["FFT"] = "Frontier";
  airlineLookup["WJA"] = "WestJet";
  airlineLookup["POE"] = "Porter";
  airlineLookup["BMA"] = "Bermuda Air";
  airlineLookup["RPA"] = "Republic";
  airlineLookup["EDV"] = "Delta";
  airlineLookup["ENY"] = "American";
  airlineLookup["PDT"] = "American";
  airlineLookup["JIA"] = "American";
  airlineLookup["SKW"] = "Delta";
  airlineLookup["GJS"] = "United / Delta";
  airlineLookup["ASH"] = "United";
  airlineLookup["UCA"] = "United";
  airlineLookup["JZA"] = "Air Canada";
  airlineLookup["AWI"] = "United";

  // what kind of birds are indigenous to Queens?
  icacoLookup["A19N"] = "Airbus A319neo";
  icacoLookup["A20N"] = "Airbus A320neo";
  icacoLookup["A21N"] = "Airbus A321neo";
  icacoLookup["A221"] = "Airbus A220-100";
  icacoLookup["A223"] = "Airbus A220-300";
  icacoLookup["A306"] = "Airbus A300-600";
  icacoLookup["A310"] = "Airbus A310";
  icacoLookup["A318"] = "Airbus A318";
  icacoLookup["A319"] = "Airbus A319";
  icacoLookup["A320"] = "Airbus A320";
  icacoLookup["A321"] = "Airbus A321";
  icacoLookup["A332"] = "Airbus A330-200";
  icacoLookup["A333"] = "Airbus A330-300";
  icacoLookup["A338"] = "Airbus A330-800";
  icacoLookup["A339"] = "Airbus A330-900";
  icacoLookup["A343"] = "Airbus A340-300";
  icacoLookup["A346"] = "Airbus A340-600";
  icacoLookup["A359"] = "Airbus A350-900";
  icacoLookup["A35K"] = "Airbus A350-1000";
  icacoLookup["A388"] = "Airbus A380-800";
  icacoLookup["AT43"] = "ATR 42-300";
  icacoLookup["AT45"] = "ATR 42-500";
  icacoLookup["AT46"] = "ATR 42-600";
  icacoLookup["AT72"] = "ATR 72-200";
  icacoLookup["AT75"] = "ATR 72-500";
  icacoLookup["AT76"] = "ATR 72-600";
  icacoLookup["B37M"] = "Boeing 737 MAX 7";
  icacoLookup["B38M"] = "Boeing 737 MAX 8";
  icacoLookup["B39M"] = "Boeing 737 MAX 9";
  icacoLookup["B3XM"] = "Boeing 737 MAX 10";
  icacoLookup["B712"] = "Boeing 717-200";
  icacoLookup["B737"] = "Boeing 737-700";
  icacoLookup["B738"] = "Boeing 737-800";
  icacoLookup["B739"] = "Boeing 737-900";
  icacoLookup["B744"] = "Boeing 747-400";
  icacoLookup["B748"] = "Boeing 747-8";
  icacoLookup["B752"] = "Boeing 757-200";
  icacoLookup["B753"] = "Boeing 757-300";
  icacoLookup["B762"] = "Boeing 767-200";
  icacoLookup["B763"] = "Boeing 767-300";
  icacoLookup["B764"] = "Boeing 767-400";
  icacoLookup["B772"] = "Boeing 777-200";
  icacoLookup["B77L"] = "Boeing 777-200LR";
  icacoLookup["B77W"] = "Boeing 777-300ER";
  icacoLookup["B779"] = "Boeing 777-9";
  icacoLookup["B788"] = "Boeing 787-8";
  icacoLookup["B789"] = "Boeing 787-9";
  icacoLookup["B78X"] = "Boeing 787-10";
  icacoLookup["BCS1"] = "Bombardier CS100 (A221)";
  icacoLookup["BCS3"] = "Bombardier CS300 (A223)";
  icacoLookup["BE20"] = "Beechcraft Super King Air 200";
  icacoLookup["B350"] = "Beechcraft King Air 350";
  icacoLookup["C172"] = "Cessna 172 Skyhawk";
  icacoLookup["C182"] = "Cessna 182 Skylane";
  icacoLookup["C208"] = "Cessna 208 Caravan";
  icacoLookup["C525"] = "Cessna CitationJet";
  icacoLookup["C56X"] = "Cessna Citation Excel";
  icacoLookup["CRJ1"] = "Bombardier CRJ-100";
  icacoLookup["CRJ2"] = "Bombardier CRJ-200";
  icacoLookup["CRJ7"] = "Bombardier CRJ-700";
  icacoLookup["CRJ9"] = "Bombardier CRJ-900";
  icacoLookup["CRJX"] = "Bombardier CRJ-1000";
  icacoLookup["DH8C"] = "De Havilland Dash 8 Q300";
  icacoLookup["DH8D"] = "De Havilland Dash 8 Q400";
  icacoLookup["DHC6"] = "De Havilland Twin Otter";
  icacoLookup["E135"] = "Embraer ERJ-135";
  icacoLookup["E145"] = "Embraer ERJ-145";
  icacoLookup["E170"] = "Embraer E170";
  icacoLookup["E175"] = "Embraer E175";
  icacoLookup["E75L"] = "Embraer E175 Long Wing";
  icacoLookup["E190"] = "Embraer E190";
  icacoLookup["E195"] = "Embraer E195";
  icacoLookup["E290"] = "Embraer E190-E2";
  icacoLookup["E295"] = "Embraer E195-E2";
  icacoLookup["GLF5"] = "Gulfstream V";
  icacoLookup["GLF6"] = "Gulfstream G650";
  icacoLookup["MD88"] = "Mad Dog MD-88";
  icacoLookup["PC12"] = "Pilatus PC-12";
}


//   __ _  __| |___    | |__
//  / _` |/ _` / __|___| '_ \
// | (_| | (_| \__ \___| |_) |
//  \__,_|\__,_|___/   |_.__/
//
// The other half of the display. Between subway passes, if there's an airliner
// low over Brooklyn on final into LGA, show the northern-most one (the one
// closest to touchdown): flight, airline, origin, aircraft type.

struct Plane {
  bool   valid = false;
  String callsign;              // trimmed, dot stripped: "AAL1389"
  String typeCode;              // ICAO type code: "A321"
  long   altFt = 0;
  float  lat = 0, lon = 0;      // last position -- fed to the route lookup
  String airline;               // resolved friendly name, or "" until looked up
  String origin;                // "MIA MIAMI", or "" if unknown / already at LGA
  bool   routeChecked = false;  // already resolved the route for this callsign?
  bool   confirmedNotLGA = false;  // routeset resolved a plausible route without
                                    // LGA in it (e.g. actually bound for JFK) --
                                    // this plane gets hidden, not just blank-origin
};

Plane g_plane;

// GET adsb.lol and keep the northern-most A3 in the altitude band. Fills
// callsign / typeCode / altFt only -- the route lookup is separate. Returns
// false (and shows nothing) on any HTTP or JSON trouble: a missing plane must
// never take the subway clock down with it.
bool fetchNorthernmostPlane(Plane &out) {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);            // adsb.lol 403s a generic UA
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(String(ADSB_URL_BASE) + ADSB_LAT + "/" + ADSB_LON + "/" + ADSB_RADIUS_NM);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("ADS-B HTTP %d\n", code);
    http.end();
    progEnd('X');
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("ADS-B JSON parse error");
    progEnd('X');
    return false;
  }

  JsonArray ac = doc["ac"];
  if (ac.isNull()) { progEnd('0'); return false; }

  float bestLat = -1000;
  JsonObject best;
  for (JsonObject a : ac) {
    if (String(a["category"] | "") != ADSB_CATEGORY) continue;

    // alt_baro is the string "ground" when parked -> coerces to 0, filtered out.
    long alt = a["alt_baro"] | 0L;
    if (alt <= 0) alt = a["alt_geom"] | 0L;
    if (alt < ADSB_ALT_MIN || alt > ADSB_ALT_MAX) continue;

    float lat = a["lat"] | -1000.0f;
    float lon = a["lon"] | -1000.0f;
    if (lat < BBOX_LAT_MIN || lat > BBOX_LAT_MAX ||
        lon < BBOX_LON_MIN || lon > BBOX_LON_MAX) continue;  // must be over Brooklyn

    if (lat > bestLat) {
      bestLat = lat;
      best = a;
    }
  }
  if (best.isNull()) { progEnd('0'); return false; }

  String cs = String(best["flight"] | "");
  cs.trim();
  if (cs.startsWith(".")) cs = cs.substring(1);  // ".N199UW" -> "N199UW"
  if (cs.isEmpty()) { progEnd('0'); return false; }

  long alt = best["alt_baro"] | 0L;
  if (alt <= 0) alt = best["alt_geom"] | 0L;

  out.valid    = true;
  out.callsign = cs;
  out.typeCode = String(best["t"] | "");
  out.altFt    = alt;
  out.lat      = best["lat"] | 0.0f;
  out.lon      = best["lon"] | 0.0f;
  progEnd('*');
  return true;
}

static bool airportIsLGA(JsonObjectConst ap) {
  return String(ap["iata"] | "") == "LGA" ||
         String(ap["icao"] | "") == "KLGA";
}

// Resolve airline + origin for p, and confirm it's actually LGA-bound. Airline
// name comes from the local table (short, offline). Origin + destination come
// from adsb.lol's routeset: POST the callsign plus the plane's current
// position, get back `_airports` and `plausible`. The bounding box + altitude
// band this plane was already picked from (see fetchNorthernmostPlane()) also
// catches JFK finals over the same stretch of Brooklyn -- LGA and JFK can even
// share the same approach heading depending on the day's runway configuration,
// so geometry alone can't tell them apart. This is the disambiguator: if the
// route resolves and plausibly does NOT include LGA, p.confirmedNotLGA is set
// and the caller hides the plane instead of showing a wrong-airport arrival.
// A route that fails to resolve (network hiccup, unplausible, unknown
// callsign) is NOT treated as "not LGA" -- same fails-open reasoning as
// capIsHighUrgency(): inconclusive data must not hide a real LGA arrival. On
// a transport error, routeChecked is left false so the next pass retries.
void lookupRoute(Plane &p) {
  if (p.callsign.length() >= 3) {
    String icao3 = p.callsign.substring(0, 3);
    if (airlineLookup.count(icao3)) p.airline = airlineLookup[icao3];
  }
  if (p.airline.isEmpty()) p.airline = "Unknown";

  progBegin();

  char body[96];
  snprintf(body, sizeof(body),
           "{\"planes\":[{\"callsign\":\"%s\",\"lat\":%.4f,\"lng\":%.4f}]}",
           p.callsign.c_str(), p.lat, p.lon);

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(ROUTE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Referer", ROUTE_REFERER);   // routeset 201s empty without this
  int code = http.POST((uint8_t *)body, strlen(body));
  char pc = 'X';

  if (code == HTTP_CODE_OK) {                 // 201 == the Referer gate rejected us
    p.routeChecked = true;
    pc = '0';
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && (doc[0]["plausible"] | false)) {
      JsonArrayConst aps = doc[0]["_airports"];
      bool lgaFound = false;
      for (int i = aps.size() - 1; i >= 0; i--) {
        if (airportIsLGA(aps[i])) { lgaFound = true; break; }
      }
      if (!lgaFound) {
        p.confirmedNotLGA = true;  // plausible route, but not headed to LGA
      } else {
        // walk back from the end to the last LGA entry that has a predecessor
        for (int i = aps.size() - 1; i >= 1; i--) {
          if (!airportIsLGA(aps[i])) continue;
          JsonObjectConst from = aps[i - 1];
          String fi = String(from["iata"] | "");
          if (fi.isEmpty()) break;
          String fc = String(from["location"] | "");
          p.origin = fc.isEmpty() ? fi : (fi + " " + fc);
          p.origin.toUpperCase();
          pc = '*';
          break;
        }
      }
    }
  } else {
    Serial.printf("route HTTP %d (will retry)\n", code);
  }
  http.end();
  progEnd(pc);
}

// One plane pass. Every field fades/slides in and out just like the train
// and bus frames, instead of snapping straight to the new text.
void showPlane(const Plane &p) {
  showFrame("*PLANE*", 400);

  // "AAL1389" -> "AAL 1389"; leave registrations / odd callsigns alone
  String flight = p.callsign;
  if (flight.length() > 3 &&
      isAlpha(flight[0]) && isAlpha(flight[1]) && isAlpha(flight[2])) {
    flight = flight.substring(0, 3) + " " + flight.substring(3);
  }
  showFadeFrame(flight, 3000);

  showFadeFrame(p.airline.length() ? p.airline : "Unknown");

  if (icacoLookup.count(p.typeCode))
    showFadeFrame(icacoLookup[p.typeCode]);
  else if (p.typeCode.length())
    showFadeFrame(p.typeCode);

  if (p.altFt > 0) {
    char alt[16];
    snprintf(alt, sizeof(alt), "%ld FT", p.altFt);
    showFadeFrame(alt);
  }

  if (p.origin.length())
    showFadeFrame("FROM " + p.origin);

  // show the flight one last time before fading out, so the user can read it
  showFadeFrame(flight, 3000);
}

// --- Emergency squawks (see SQK_URL_BASE above) ----------------------------
// Shown with the urgent alerts, every cycle while it lasts.

struct SquawkCode {
  const char *code;
  const char *meaning;
};
const SquawkCode SQUAWKS[] = {   // most serious first
  {"7500", "HIJACK"},
  {"7700", "EMERGENCY"},
  {"7600", "RADIO FAILURE"},
};

String g_squawkTag;   // "SQK 7700"
String g_squawkLine;  // "EMERGENCY UAL 123 B738 45 MI NE 12000 FT"; "" when clear

float haversineKm(float lat1, float lon1, float lat2, float lon2);
float bearingDeg(float lat1, float lon1, float lat2, float lon2);
const char *compass8(float deg);

void fetchSquawks() {
  String tag, line;
  for (const SquawkCode &sq : SQUAWKS) {
    progBegin();
    String payload;
    if (!httpGet("SQK", String(SQK_URL_BASE) + sq.code, payload)) {
      progEnd('X');
      continue;
    }

    JsonDocument filter;
    filter["ac"][0]["flight"]   = true;
    filter["ac"][0]["t"]        = true;
    filter["ac"][0]["lat"]      = true;
    filter["ac"][0]["lon"]      = true;
    filter["ac"][0]["alt_baro"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
      Serial.println("SQK JSON parse error");
      progEnd('X');
      continue;
    }

    float bestMi = SQK_RADIUS_MI;
    for (JsonObject a : doc["ac"].as<JsonArray>()) {
      if (a["lat"].isNull()) continue;
      float lat = a["lat"], lon = a["lon"];
      float mi  = haversineKm(HOME_LAT, HOME_LON, lat, lon) * 0.621371f;
      if (mi > bestMi) continue;
      bestMi = mi;

      String cs = String(a["flight"] | "");
      cs.trim();
      if (cs.length() > 3 && isAlpha(cs[0]) && isAlpha(cs[1]) && isAlpha(cs[2]))
        cs = cs.substring(0, 3) + " " + cs.substring(3);   // as in showPlane()
      long alt = a["alt_baro"] | 0L;                        // "ground" -> 0

      tag  = String("SQK ") + sq.code;
      line = String(sq.meaning) + " " + (cs.length() ? cs : String("UNKNOWN"));
      if (String(a["t"] | "").length()) line += " " + String(a["t"] | "");
      line += " " + String((int)roundf(mi)) + " MI " +
              compass8(bearingDeg(HOME_LAT, HOME_LON, lat, lon));
      if (alt > 0) line += " " + String(alt) + " FT";
    }
    progEnd(line.length() ? '*' : '0');
    if (line.length()) break;  // most serious code wins
  }
  g_squawkTag  = tag;
  g_squawkLine = line;
  Serial.printf("SQK: %s\n", line.length() ? line.c_str() : "(none)");
}


//  _               _
// | |__  _   _ ___| |_ ___
// | '_ \| | | / __| __/ _ \
// | |_) | |_| \__ \ ||  __/
// |_.__/ \__,_|___/\__\___|
//
// by the way, Claude is terrible at doing figlets.
// "buste"?  
// 
// Upcoming buses at each stop in BUS_FEEDS (M14A -> Abingdon Sq, M9 -> Battery
// Pk City). Each stop is one-directional so no direction filtering, but stops
// carry more than one route, so keep only the feed's linePrefix.

struct BusArrival {
  long epoch;
};

BusArrival g_bus[NUM_BUS_FEEDS][BUS_MAX];
int        g_busCount[NUM_BUS_FEEDS] = {0};

// Fetch one stop and refill g_bus[fi] / g_busCount[fi]. On any HTTP/JSON
// trouble it leaves the previous list in place.
void fetchOneBusFeed(int fi) {
  const BusFeed &feed = BUS_FEEDS[fi];

  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(String(BUS_URL_BASE) + "?key=" + BUSTIME_API_KEY +
             "&MonitoringRef=" + feed.stopRef);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("BUS %s HTTP %d\n", feed.linePrefix, code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  // One stop's SIRI response is small (a few KB), so just parse the whole
  // thing -- no filter. (An earlier filter build was silently empty, which is
  // why no buses ever showed.) SIRI nests fairly deep, so lift the limit.
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::NestingLimit(20));
  if (err) {
    Serial.printf("BUS %s parse error: %s\n", feed.linePrefix, err.c_str());
    progEnd('X');
    return;
  }

  JsonArray visits = doc["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]
                        ["MonitoredStopVisit"];
  int n = 0;
  for (JsonObject v : visits) {
    if (n >= BUS_MAX) break;
    JsonObject j = v["MonitoredVehicleJourney"];

    // PublishedLineName is an array in SIRI v2 (["M14A-SBS"]); tolerate a
    // bare string too.
    JsonVariant ln = j["PublishedLineName"];
    String line = ln.is<JsonArray>() ? String(ln[0] | "") : String(ln | "");
    if (!line.startsWith(feed.linePrefix)) continue;

    JsonObject mc = j["MonitoredCall"];
    long e = isoToEpoch(mc["ExpectedArrivalTime"] | "");
    if (e == 0) e = isoToEpoch(mc["AimedArrivalTime"] | "");
    if (e == 0) continue;

    g_bus[fi][n].epoch = e;
    n++;
  }
  g_busCount[fi] = n;
  Serial.printf("BUS %s: %d\n", feed.linePrefix, n);
  progEnd(n > 0 ? '*' : '0');
}

void fetchBuses() {
  if (BUSTIME_API_KEY[0] == '\0') return;  // no key compiled in -> section off
  for (size_t fi = 0; fi < NUM_BUS_FEEDS; fi++) fetchOneBusFeed(fi);
}

bool hasBuses() {
  for (size_t i = 0; i < NUM_BUS_FEEDS; i++) if (g_busCount[i]) return true;
  return false;
}

// A bus rolls through -- roof over a front wheel, BUS, roof over dual back
// wheels -- then for each stop with buses, run through the
// upcoming arrivals, each one its own frame carrying the route tag and the
// countdown: "M14 12mn" / "M9 12min" / "M14 NOW". "min" is trimmed to "mn" when
// the whole thing would overflow the 8 columns (M14 + two-digit minutes). SIRI
// hands them back soonest-first already.
bool showBuses(long nowEpoch) {
  if (!hasBuses()) return false;
  scrollAcross("[\x03\x04\x03 BUS \x04\x03\x04\x03]");
  for (size_t fi = 0; fi < NUM_BUS_FEEDS; fi++) {
    if (g_busCount[fi] == 0) continue;

    const char *disp = BUS_FEEDS[fi].disp;
    for (int i = 0; i < g_busCount[fi]; i++) {
      long mins = (g_bus[fi][i].epoch - nowEpoch + 30) / 60;

      char frame[16];
      if (mins <= 0) {
        snprintf(frame, sizeof(frame), "%s NOW", disp);
      } else {
        if (mins > 99) mins = 99;
        snprintf(frame, sizeof(frame), "%s %ldmin", disp, mins);
        if (strlen(frame) > 8)  // "M14 12min" -> "M14 12mn"
          snprintf(frame, sizeof(frame), "%s %ldmn", disp, mins);
      }
      showFrame(frame, 1300);
    }
  }
  return true;
}


//   ____ _ _   _  ____  _ _
//  / ___(_) |_(_) __ )(_) | _____
// | |   | | __| |  _ \| | |/ / _ \
// | |___| | |_| | |_) | |   <  __/
//  \____|_|\__|_|____/|_|_|\_\___|
//
// Bikes + docks available at the Clinton St & Grand St corner, summed across
// its two station_ids (see CITIBIKE_STATION_IDS above). -1 means "no reading
// yet" so showCitibike() can stay quiet on first boot instead of flashing 0s.

long g_citibikeBikes = -1;
long g_citibikeDocks = -1;

// Finds "key"<:><spaces><digits> in `window` and returns the digits, or -1 if
// `key` isn't there. Tolerant of the feed being minified or not -- it's a
// public GBFS mirror, not a contract.
long jsonIntAfterKey(const String &window, const char *key) {
  int i = window.indexOf(String("\"") + key + "\"");
  if (i < 0) return -1;
  int colon = window.indexOf(':', i);
  if (colon < 0) return -1;
  int j = colon + 1;
  while (j < (int)window.length() && window[j] == ' ') j++;
  int start = j;
  while (j < (int)window.length() && isDigit(window[j])) j++;
  return (j == start) ? -1 : window.substring(start, j).toInt();
}

void fetchCitibike() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(8000);
  http.begin(CITIBIKE_STATUS_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("CITIBIKE HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }

  // Stream the body and hand-scan for our station_ids, rather than buffering
  // the ~1 MB all-stations payload -- see the comment on CITIBIKE_STATUS_URL.
  WiFiClient *stream = http.getStreamPtr();
  bool found[NUM_CITIBIKE_STATIONS] = {false};
  long bikes[NUM_CITIBIKE_STATIONS] = {0};
  long docks[NUM_CITIBIKE_STATIONS] = {0};
  size_t stillLooking = NUM_CITIBIKE_STATIONS;

  String buf;
  buf.reserve(1600);
  char chunk[257];
  unsigned long start = millis();
  while (http.connected() && stillLooking > 0 && millis() - start < 10000) {
    size_t avail = stream->available();
    if (avail == 0) { delay(1); continue; }
    if (avail > sizeof(chunk) - 1) avail = sizeof(chunk) - 1;
    size_t got = stream->readBytes(chunk, avail);
    chunk[got] = '\0';
    buf += chunk;
    if (buf.length() > 1400) buf.remove(0, buf.length() - 1400);  // keep a tail window

    for (size_t i = 0; i < NUM_CITIBIKE_STATIONS; i++) {
      if (found[i]) continue;
      int p = buf.indexOf(CITIBIKE_STATION_IDS[i]);
      if (p < 0) continue;
      int windowEnd = p + 400;
      if (windowEnd > (int)buf.length()) windowEnd = buf.length();
      String window = buf.substring(p, windowEnd);
      long ba = jsonIntAfterKey(window, "num_bikes_available");
      long da = jsonIntAfterKey(window, "num_docks_available");
      if (ba < 0 || da < 0) continue;  // fields not fully buffered yet -- retry next chunk
      bikes[i] = ba;
      docks[i] = da;
      found[i] = true;
      stillLooking--;
    }
  }
  http.end();

  if (stillLooking > 0) {
    Serial.printf("CITIBIKE: only found %d/%d stations\n",
                  (int)(NUM_CITIBIKE_STATIONS - stillLooking), (int)NUM_CITIBIKE_STATIONS);
    progEnd('X');
    return;  // keep the last known reading rather than show a partial total
  }

  g_citibikeBikes = 0;
  g_citibikeDocks = 0;
  for (size_t i = 0; i < NUM_CITIBIKE_STATIONS; i++) {
    g_citibikeBikes += bikes[i];
    g_citibikeDocks += docks[i];
  }
  Serial.printf("CITIBIKE: %ld bikes, %ld docks\n", g_citibikeBikes, g_citibikeDocks);
  progEnd('*');
}

// Header, then two frames, same cadence as a bus arrival: bike count then
// dock count.
bool showCitibike(long) {
  if (g_citibikeBikes < 0) return false;  // no reading yet
  showFrame("CitiBike", 1300);
  char frame[16];
  snprintf(frame, sizeof(frame), "%ld bikes", g_citibikeBikes);
  showFrame(frame, 1300);
  snprintf(frame, sizeof(frame), "%ld docks", g_citibikeDocks);
  showFrame(frame, 1300);
  return true;
}


//                    _   _
//  __      ____ __  | | | |
//  \ \ /\ / /\ \/ / | |_| |
//   \ V  V /  >  <  |  _  |
//    \_/\_/  /_/\_\ |_| |_|
//
// Basic NWS emergency info: if there's an active alert for our point, its event
// name gets a frame after the trains. Just the event -- no headline/instruction.
// NWS alerts are CAP messages (CAP-in-GeoJSON), so severity/urgency are kept
// alongside event via the shared CapAlert struct -- see capIsHighUrgency()
// above -- but unlike NYC OEM, NWS still shows *any* active alert for our
// point regardless of severity: the feed is already narrowly scoped to one
// point, so it doesn't need the same noise filter NYC OEM does.

CapAlert g_wxAlert;  // e.g. event "Winter Weather Advisory"; event=="" when clear

void fetchWeatherAlert() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);              // NWS asks for an identifying UA
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(WX_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("WX HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;  // keep the last known alert; don't drop a real one on a blip
  }
  String payload = http.getString();
  http.end();

  // Keep only features[*].properties.{event,severity,urgency} -- full alert
  // bodies are huge.
  JsonDocument filter;
  filter["features"][0]["properties"]["event"]    = true;
  filter["features"][0]["properties"]["severity"] = true;
  filter["features"][0]["properties"]["urgency"]  = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.println("WX JSON parse error");
    progEnd('X');
    return;
  }

  JsonArray feats = doc["features"];
  if (!feats.isNull() && feats.size() > 0) {
    JsonObject props = feats[0]["properties"];
    g_wxAlert.event    = String(props["event"]    | "");
    g_wxAlert.severity = String(props["severity"] | "");
    g_wxAlert.urgency  = String(props["urgency"]  | "");
  } else {
    g_wxAlert = CapAlert();
  }
  Serial.printf("WX: %s\n", g_wxAlert.event.length() ? g_wxAlert.event.c_str() : "(clear)");
  progEnd(g_wxAlert.event.length() ? '*' : '0');    // '0' == no active alert
}


//__        ____  __       _   _ _____        __
//\ \      / /\ \/ /  _ __ | \ | |  _ \\ \    / /
// \ \ /\ / /  \  /  | '_ \|  \| | | | \ \ /\ / /
//  \ V  V /   /  \  | | | | |\  | |_| |\ V  V /
//   \_/\_/   /_/\_\ |_| |_|_| \_|____/  \_/\_/
//
// Current conditions (KNYC / Central Park) and the first WXFC_PERIODS periods
// of the gridpoint forecast, cycling alongside the trains/buses like Citi
// Bike/ISS/hurricane -- no alert treatment, this is just weather.

float cToF(float c) { return c * 9.0f / 5.0f + 32.0f; }

// WX conditions/forecast text scrolls rather than getting truncated (see
// showWxNow()/showWxForecast() below), so it's shown in full, in NWS's own
// mixed case -- abbreviating it used to shorten the scroll but made it read
// as cryptic fragments ("Chc T-storms/Sct Shwrs") rather than words.

struct WxNow {
  float  tempF     = 0;
  float  feelsF    = 0;
  bool   hasFeels  = false;  // only worth its own frame if it differs from tempF
  float  dewF      = 0;
  String conditions;
  bool   valid     = false;
};
WxNow g_wxNow;

void fetchWxNow() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.begin(WXNOW_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("WXNOW HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["properties"]["temperature"]["value"] = true;
  filter["properties"]["dewpoint"]["value"]    = true;
  filter["properties"]["heatIndex"]["value"]   = true;
  filter["properties"]["windChill"]["value"]   = true;
  filter["properties"]["textDescription"]      = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.println("WXNOW JSON parse error");
    progEnd('X');
    return;
  }

  JsonObject p = doc["properties"];
  if (p["temperature"]["value"].isNull()) {
    Serial.println("WXNOW: station has no current reading");
    progEnd('0');  // keep the last known reading
    return;
  }

  float tempC  = p["temperature"]["value"] | 0.0f;
  float dewC   = p["dewpoint"]["value"]    | tempC;
  float feelsC = tempC;
  if (!p["heatIndex"]["value"].isNull())      feelsC = p["heatIndex"]["value"];
  else if (!p["windChill"]["value"].isNull()) feelsC = p["windChill"]["value"];

  g_wxNow.tempF      = cToF(tempC);
  g_wxNow.dewF       = cToF(dewC);
  g_wxNow.feelsF     = cToF(feelsC);
  g_wxNow.hasFeels   = fabsf(g_wxNow.feelsF - g_wxNow.tempF) >= 3.0f;
  g_wxNow.conditions = String(p["textDescription"] | "");
  g_wxNow.valid      = true;

  Serial.printf("WXNOW: %.0fF feels %.0fF dew %.0fF %s\n",
      g_wxNow.tempF, g_wxNow.feelsF, g_wxNow.dewF, g_wxNow.conditions.c_str());
  progEnd('*');
}

// Header, temp, feels-like (only when it actually differs), dew point, then
// the conditions text, in NWS's own natural mixed case. Conditions text is
// free-form/variable-length (unlike the tag+number frames above it), so it
// gets the fade/slide/marquee treatment like the quake line or plane details
// rather than getting silently truncated at 8 columns.
bool showWxNow(long) {
  if (!g_wxNow.valid) return false;

  showFrame("WX Now", 1300);

  char frame[16];
  snprintf(frame, sizeof(frame), "Temp %dF", (int)roundf(g_wxNow.tempF));
  showFrame(frame, 1300);

  if (g_wxNow.hasFeels) {
    snprintf(frame, sizeof(frame), "Feel %dF", (int)roundf(g_wxNow.feelsF));
    showFrame(frame, 1300);
  }

  snprintf(frame, sizeof(frame), "Dew %dF", (int)roundf(g_wxNow.dewF));
  showFrame(frame, 1300);

  if (g_wxNow.conditions.length()) {
    showFadeFrame(g_wxNow.conditions, 2000);
  }
  return true;
}

struct WxForecastPeriod {
  String name;           // NWS's own period name, e.g. "Tonight" -- these are
                          // relative to now, not fixed daily slots, so we show
                          // it as-is (aside from case) rather than guessing
                          // "today/tonight/tomorrow"
  String shortForecast;
};
WxForecastPeriod g_wxForecast[WXFC_PERIODS];
bool             g_haveWxForecast = false;

void fetchWxForecast() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  http.begin(WXFC_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("WXFC HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["properties"]["periods"][0]["name"]          = true;
  filter["properties"]["periods"][0]["shortForecast"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.println("WXFC JSON parse error");
    progEnd('X');
    return;
  }

  int n = 0;
  for (JsonObject p : doc["properties"]["periods"].as<JsonArray>()) {
    if (n >= WXFC_PERIODS) break;
    g_wxForecast[n].name          = String(p["name"] | "");
    g_wxForecast[n].shortForecast = String(p["shortForecast"] | "");
    n++;
  }
  g_haveWxForecast = (n > 0);
  Serial.printf("WXFC: %d periods\n", n);
  progEnd(n > 0 ? '*' : '0');
}

// Two frames per period: its name (e.g. "Tonight", "This Afternoon") as a
// header-style label, then its shortForecast as content -- both can run well
// past 8 columns (period names like "This Afternoon" or "Sunday Night"
// included), so neither gets silently truncated; the label just snaps in
// like other header tags, the forecast text gets the fade/slide/marquee
// treatment like the conditions text above.
bool showWxForecast(long) {
  if (!g_haveWxForecast) return false;
  scrollAcross("...AND NOW FOR YOUR LOCAL FORECAST");
  for (int i = 0; i < WXFC_PERIODS; i++) {
    if (!g_wxForecast[i].name.length()) continue;
    showText(g_wxForecast[i].name, -1, 1800);
    showFadeFrame(g_wxForecast[i].shortForecast, 2000);
  }
  return true;
}


// --- Daily horoscope (novelty, not a real feed) -----------------------------
// freehoroscopeapi.com, no key needed. Paragraph-length prose per sign, same
// scrolling treatment as the WX conditions text above. Only three signs are
// fetched -- Virgo, Capricorn, Aquarius -- and showHoroscope() below picks one
// of them at random each time it's called, rather than cycling through all
// three, so which sign shows up varies day to day.
#define HOROSCOPE_URL_BASE   "https://freehoroscopeapi.com/api/v1/get-horoscope/daily?day=TODAY&sign="
#define HOROSCOPE_REFETCH_MS 21600000   // 6h -- content only actually changes once/day

struct Horoscope {
  const char *sign;   // API param
  const char *label;  // display header
  String      text;
  bool        valid;
};
Horoscope g_horoscopes[] = {
  {"virgo",     "VIRGO",     "", false},
  {"capricorn", "CAPRICORN", "", false},
  {"aquarius",  "AQUARIUS",  "", false},
};
#define NUM_HOROSCOPES (sizeof(g_horoscopes) / sizeof(g_horoscopes[0]))

// Text up to and including the first ". ", "! " or "? " -- the whole thing if
// there's no sentence break. The API's prose runs 4-5 sentences, far too
// long to marquee across 8 columns.
String firstSentence(const String &text) {
  int end = -1;
  for (const char *brk : {". ", "! ", "? "}) {
    int i = text.indexOf(brk);
    if (i >= 0 && (end < 0 || i < end)) end = i;
  }
  return end < 0 ? text : text.substring(0, end + 1);
}

void fetchHoroscopes() {
  for (size_t i = 0; i < NUM_HOROSCOPES; i++) {
    progBegin();

    HTTPClient http;
    http.setUserAgent(USER_AGENT);
    http.setConnectTimeout(4000);
    http.setTimeout(5000);
    http.begin(String(HOROSCOPE_URL_BASE) + g_horoscopes[i].sign);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      Serial.printf("HOROSCOPE %s HTTP %d\n", g_horoscopes[i].sign, code);
      http.end();
      progEnd('X');
      continue;
    }
    String payload = http.getString();
    http.end();

    JsonDocument filter;
    filter["data"]["horoscope"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
      Serial.printf("HOROSCOPE %s JSON parse error\n", g_horoscopes[i].sign);
      progEnd('X');
      continue;
    }

    g_horoscopes[i].text  = firstSentence(String(doc["data"]["horoscope"] | ""));
    g_horoscopes[i].valid = g_horoscopes[i].text.length() > 0;
    Serial.printf("HOROSCOPE %s: %s\n", g_horoscopes[i].sign,
        g_horoscopes[i].valid ? "ok" : "empty");
    progEnd(g_horoscopes[i].valid ? '*' : '0');
  }
}

// One randomly-picked sign's header then its horoscope's first sentence (scrolls,
// like WX conditions).
bool showHoroscope(long) {
  int valid[NUM_HOROSCOPES];
  int numValid = 0;
  for (size_t i = 0; i < NUM_HOROSCOPES; i++) {
    if (g_horoscopes[i].valid) valid[numValid++] = i;
  }
  if (numValid == 0) return false;

  Horoscope &h = g_horoscopes[valid[random(numValid)]];
  showFrame(h.label, 1300);
  showFadeFrame(h.text, 2500);
  return true;
}


//  __  __             _        ___    ____        _ _
// |  \/  | __ _  __ _(_) ___  ( _ )  | __ )  __ _| | |
// | |\/| |/ _` |/ _` | |/ __| / _ \  |  _ \ / _` | | |
// | |  | | (_| | (_| | | (__ | (_) | | |_) | (_| | | |
// |_|  |_|\__,_|\__, |_|\___| \___/  |____/ \__,_|_|_|
//               |___/
//
// No feed -- a random answer from the table, scrambled in like the loading
// clock, then marqueed. Plain ASCII only (the 14-segment font has no curly
// quotes or en dashes).
const char *const MAGIC8_ANSWERS[] = {  // upbeat or silly only -- no doom
  "It is certain.",
  "It is decidedly so.",
  "Without a doubt.",
  "Yes - definitely.",
  "You may rely on it.",
  "As I see it, yes.",
  "Most likely.",
  "Outlook good.",
  "Yes.",
  "Signs point to yes.",
  "Reply hazy, try again.",
  "Ask again later.",
  "Better not tell you now.",
  "Cannot predict now.",
  "Concentrate and ask again.",
  "Do I look like Google?",
  "Try bribing me first.",
  "The future is blurry; buy more snacks.",
  "The universe shrugged.",
  "The answer you seek is behind you.",
  "Dodge left.",
  "The stars are quiet today.",
  "Ask the cat instead.",
  "Not in my job description.",
  "Check under your bed.",
  "It is written, but in cursive.",
  "The simulation is lagging.",
  "A pigeon knows the truth.",
  "Shhh, they are listening.",
  "The shadow says yes.",
  "Manifest harder.",
  "Ask me after I've had my coffee.",
  "Reply pending a cash transfer.",
  "The answer is locked behind a paywall.",
  "That sounds like a problem for future you.",
  "Let's pretend you never asked that.",
  "I'd rather not get involved in this drama.",
  "That's a definite 'meh'.",
  "Don't ask me, I'm just a toy.",
  "Your destiny is currently out of office.",
  "Your future is a bit of a question mark.",
  "Does it look like I have a crystal ball? Wait, don't answer that.",
  "I answered this yesterday. Keep up.",
  // upbeat
  "The stars say go for it.",
  "Yes, and it'll be great.",
  "Absolutely. Treat yourself.",
  "All signs point to pizza.",
  "Today's your day.",
  "Fortune favors you.",
  "Big yes energy.",
  "The cosmos approves.",
  "Lucky you - yes.",
  "Green lights all the way.",
  "Yes, with sprinkles.",
  "The vibes are immaculate.",
  "Odds are in your favor.",
  "A happy surprise is coming.",
  "Smooth sailing ahead.",
  "The universe is rooting for you.",
  "Trust your gut. It's right.",
  "Good things are on the way.",
  "Definitely. Bring snacks.",
  "The dice have spoken: yes.",
  // mysterious
  "The moon knows, but won't say.",
  "Ask again at midnight.",
  "The answer arrives on the next F train.",
  "Somewhere, a cat already knows.",
  "The fog lifts by Thursday.",
  "Seek the one with the umbrella.",
  "A stranger holds the answer.",
  "Follow the pigeons.",
  "The tea leaves are blushing.",
  "Whisper it to the East River.",
  "The mists part... it's a yes.",
  "Three crows say yes.",
  "The cards are face down. For now.",
  "An old friend will tell you.",
  "The tide turns in your favor.",
  "The answer is in your pocket.",
  "Listen to the radiator.",
  "Ask the moon tonight.",
  "The spirits are giggling. Good sign.",
  "The crystal ball says: soon.",
  "It hums with possibility.",
  "Count to three, then decide.",
  "The owl nods.",
  "Look up. The answer is on a rooftop.",
  "The wind changes at noon.",
};
#define NUM_MAGIC8_ANSWERS (sizeof(MAGIC8_ANSWERS) / sizeof(MAGIC8_ANSWERS[0]))

bool showMagic8(long) {
  slideIn("<8-BALL>");
  for (int i = 0; i < 2; i++) {  // pulse: fade in, fade out, twice
    fadeBrightnessBoth(BRIGHT_FULL, 30);
    fadeBrightnessBoth(0, 30);
  }
  showNoiseFrame(MAGIC8_ANSWERS[random(NUM_MAGIC8_ANSWERS)], 1500);
  return true;
}


//  __  __
// |  \/  | ___   ___  _ __
// | |\/| |/ _ \ / _ \| '_ \
// | |  | | (_) | (_) | | | |
// |_|  |_|\___/ \___/|_| |_|
//
// Moon phase, computed locally from a known reference new moon and the
// average synodic month length -- no network fetch needed. Good to about a
// day's accuracy, plenty for a novelty display.
#define MOON_REFERENCE_EPOCH 947182440L    // 2000-01-06 18:14 UTC, a known new moon
#define MOON_SYNODIC_DAYS    29.530588853  // average new-moon-to-new-moon length

// Days since the last new moon, in [0, MOON_SYNODIC_DAYS).
double moonPhaseDays(long nowEpoch) {
  double days  = (nowEpoch - MOON_REFERENCE_EPOCH) / 86400.0;
  double phase = fmod(days, MOON_SYNODIC_DAYS);
  if (phase < 0) phase += MOON_SYNODIC_DAYS;
  return phase;
}

const char *moonPhaseName(double phase) {
  static const char *names[] = {
    "New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
    "Full Moon", "Waning Gibbous", "Last Quarter", "Waning Crescent",
  };
  int idx = ((int)round(phase / MOON_SYNODIC_DAYS * 8.0)) % 8;
  return names[idx];
}

bool showMoonPhase(long nowEpoch) {
  double phase = moonPhaseDays(nowEpoch);
  fadeInOut(center8("Moon"));
  showFadeFrame(moonPhaseName(phase), 2200);

  // Whichever of new/full moon is closer, counting forward from today --
  // full moon sits half a synodic month after new moon, so both "days
  // until" figures are just that offset's distance from the current phase,
  // wrapped forward into the next cycle if it's already past this month's.
  double half       = MOON_SYNODIC_DAYS / 2.0;
  double daysToNew   = fmod(MOON_SYNODIC_DAYS - phase, MOON_SYNODIC_DAYS);
  double daysToFull  = fmod(half - phase + MOON_SYNODIC_DAYS, MOON_SYNODIC_DAYS);

  char frame[10];
  if (daysToNew <= daysToFull) {
    snprintf(frame, sizeof(frame), "New %dd", (int)round(daysToNew));
  } else {
    snprintf(frame, sizeof(frame), "Full %dd", (int)round(daysToFull));
  }
  showFrame(frame, 1300);
  return true;
}


//  ____              _              _
// / ___| _   _ _ __ | |_ __ ___   ___  ___| |_
// \___ \| | | | '_ \| '__/ __| / _ \/ __| __|
//  ___) | |_| | | | | | \__ \|  __/\__ \ |_
// |____/ \__,_|_| |_|_| |___/\___||___/\__|
//
// Sunrise/sunset via sunrise-sunset.org (free, no key) for home's coordinates.
// The feed's times come back UTC; display formatting converts to NY
// wall-clock via the TZ set up alongside NTP in setup() below.
#define SUN_URL         "https://api.sunrise-sunset.org/json?lat=40.7168&lng=-73.9861&formatted=0"
#define SUN_REFETCH_MS  21600000    // 6h -- the times only actually shift by seconds/day

struct SunTimes {
  long sunriseEpoch = 0;
  long sunsetEpoch  = 0;
  bool valid        = false;
};
SunTimes g_sun;

void fetchSunTimes() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.begin(SUN_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("SUN HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["results"]["sunrise"] = true;
  filter["results"]["sunset"]  = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.println("SUN JSON parse error");
    progEnd('X');
    return;
  }

  g_sun.sunriseEpoch = isoToEpoch(doc["results"]["sunrise"] | "");
  g_sun.sunsetEpoch  = isoToEpoch(doc["results"]["sunset"]  | "");
  g_sun.valid        = g_sun.sunriseEpoch > 0 && g_sun.sunsetEpoch > 0;
  Serial.printf("SUN: rise %ld set %ld\n", g_sun.sunriseEpoch, g_sun.sunsetEpoch);
  progEnd(g_sun.valid ? '*' : 'X');
}

// Epoch -> "6-42am", NY local, via the TZ set up in setup() -- always <=7
// chars, so this never needs to scroll.
String hhmmAmPm(long epoch) {
  time_t t = (time_t)epoch;
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  int h = tmLocal.tm_hour % 12;
  if (h == 0) h = 12;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d-%02d%s", h, tmLocal.tm_min, tmLocal.tm_hour < 12 ? "am" : "pm");
  return String(buf);
}

bool showSunTimes(long) {
  if (!g_sun.valid) return false;
  showFrame("Sunrise", 1300);
  showFrame(hhmmAmPm(g_sun.sunriseEpoch), 1300);
  showFrame("Sunset", 1300);
  showFrame(hhmmAmPm(g_sun.sunsetEpoch), 1300);
  return true;
}


//  _____ _     _
// |_   _(_) __| | ___
//   | | | |/ _` |/ _ \
//   | | | | (_| |  __/
//   |_| |_|\__,_|\___|
//
// NOAA tide predictions for The Battery, NY (station 8518750) -- the nearest
// published tide station to home. time_zone=lst_ldt returns the station's own
// local time, already DST-adjusted, so no timezone math is needed beyond
// pulling hours/minutes back out of the string. range=48 starting at the top
// of today comfortably covers "what's next" even late at night, without this
// device ever needing to know what today's date is -- NOAA resolves the
// literal "today" itself.
#define TIDE_URL         "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?begin_date=today&range=48&station=8518750&product=predictions&datum=MLLW&time_zone=lst_ldt&units=english&interval=hilo&format=json"
#define TIDE_REFETCH_MS  14400000   // 4h -- plenty since every fetch already looks 48h ahead
#define TIDE_MAX_EVENTS  16

// wallSec is seconds-of-local-time in the same daysFromCivil() arithmetic
// space isoToEpoch() uses -- not a real UTC epoch, just an axis that's
// consistent with "now" converted the same way (see nowNyWallSec() below), so
// the two can be diffed directly for a countdown/lookup without ever
// materializing a real epoch for either side.
struct TideEvent {
  long  wallSec;
  float ft;
  char  type;   // 'H' or 'L'
};
TideEvent g_tides[TIDE_MAX_EVENTS];
int       g_tideCount = 0;

void fetchTide() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  http.begin(TIDE_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("TIDE HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["predictions"][0]["t"]    = true;
  filter["predictions"][0]["v"]    = true;
  filter["predictions"][0]["type"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.println("TIDE JSON parse error");
    progEnd('X');
    return;
  }

  g_tideCount = 0;
  for (JsonObject p : doc["predictions"].as<JsonArray>()) {
    if (g_tideCount >= TIDE_MAX_EVENTS) break;
    int Y, Mo, D, h, m;
    if (sscanf(p["t"] | "", "%d-%d-%d %d:%d", &Y, &Mo, &D, &h, &m) < 5) continue;
    TideEvent &e  = g_tides[g_tideCount];
    e.wallSec     = daysFromCivil(Y, Mo, D) * 86400L + h * 3600L + m * 60L;
    e.ft          = atof(p["v"] | "0");
    const char *t = p["type"] | "H";
    e.type        = t[0];
    g_tideCount++;
  }
  Serial.printf("TIDE: %d predictions\n", g_tideCount);
  progEnd(g_tideCount > 0 ? '*' : '0');
}

// "Now", in the same wall-clock arithmetic space as g_tides[].wallSec.
long nowNyWallSec(long utcEpoch) {
  time_t t = (time_t)utcEpoch;
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  return daysFromCivil(tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday) * 86400L +
         tmLocal.tm_hour * 3600L + tmLocal.tm_min * 60L + tmLocal.tm_sec;
}

bool showTide(long nowEpoch) {
  if (g_tideCount == 0) return false;
  long nowWall = nowNyWallSec(nowEpoch);

  const TideEvent *next = nullptr;
  for (int i = 0; i < g_tideCount; i++) {
    if (g_tides[i].wallSec >= nowWall) { next = &g_tides[i]; break; }
  }
  if (!next) return false;  // ran off the 48h window; the next fetch refills it

  int h   = (int)((next->wallSec % 86400) / 3600);
  int m   = (int)((next->wallSec % 3600) / 60);
  int h12 = h % 12;
  if (h12 == 0) h12 = 12;

  scrollAcross("_-\x03 TIDE \x03-_");  // a wave: bottom, middle, top bar
  showFrame(next->type == 'H' ? "High" : "Low", 1300);
  char frame[20];
  snprintf(frame, sizeof(frame), "%d-%02d%s %.1fft", h12, m, h < 12 ? "am" : "pm", next->ft);
  showFadeFrame(frame, 2200);
  return true;
}


//  _   _       _ _     _
// | | | | ___ | (_) __| | __ _ _   _
// | |_| |/ _ \| | |/ _` |/ _` | | | |
// |  _  | (_) | | | (_| | (_| | |_| |
// |_| |_|\___/|_|_|\__,_|\__,_|\__, |
//                              |___/
//
// Next US public holiday, via Nager.Date's free no-key API. The list comes
// back soonest-first; we keep the first entry that's actually observed here
// -- `global:true` (nationwide) or `counties` includes "US-NY" -- since a
// state-specific one elsewhere in the list (e.g. some states' Columbus Day
// vs. others' Indigenous Peoples' Day, same date) isn't necessarily the one
// that applies at home. Japan's next holiday comes from the same API.
#define HOLIDAY_URL         "https://date.nager.at/api/v3/NextPublicHolidays/US"
#define JP_HOLIDAY_URL      "https://date.nager.at/api/v3/NextPublicHolidays/JP"
#define HOLIDAY_REFETCH_MS  43200000   // 12h -- the list only changes once a holiday passes

struct Holiday {
  String name;
  long   civilDay = 0;   // daysFromCivil() of the holiday's date
  bool   valid    = false;
};
Holiday g_holiday;
Holiday g_jpHoliday;

// Fills `out` with the first holiday in `url`'s list that's nationwide or,
// when `county` is given, observed there.
void fetchHolidayFrom(const char *tag, const char *url, const char *county, Holiday &out) {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.begin(url);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("%s HTTP %d\n", tag, code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter[0]["date"]     = true;
  filter[0]["name"]     = true;
  filter[0]["global"]   = true;
  filter[0]["counties"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.printf("%s JSON parse error\n", tag);
    progEnd('X');
    return;
  }

  out.valid = false;
  for (JsonObject h : doc.as<JsonArray>()) {
    bool appliesHere = h["global"] | false;
    if (!appliesHere && county) {
      for (JsonVariant c : h["counties"].as<JsonArray>()) {
        if (strcmp(c.as<const char *>(), county) == 0) { appliesHere = true; break; }
      }
    }
    if (!appliesHere) continue;

    int Y, Mo, D;
    if (sscanf(h["date"] | "", "%d-%d-%d", &Y, &Mo, &D) < 3) continue;
    out.name     = String(h["name"] | "");
    out.civilDay = daysFromCivil(Y, Mo, D);
    out.valid    = true;
    break;  // list is soonest-first
  }
  Serial.printf("%s: %s\n", tag, out.valid ? out.name.c_str() : "(none applicable)");
  progEnd(out.valid ? '*' : '0');
}

void fetchHoliday()   { fetchHolidayFrom("HOLIDAY", HOLIDAY_URL, "US-NY", g_holiday); }
void fetchJpHoliday() { fetchHolidayFrom("JP HOLIDAY", JP_HOLIDAY_URL, nullptr, g_jpHoliday); }
bool showHolidayFrom(const Holiday &h, const char *header, long nowEpoch);
bool showHoliday(long nowEpoch)   { return showHolidayFrom(g_holiday, "Holiday", nowEpoch); }
bool showJpHoliday(long nowEpoch) { return showHolidayFrom(g_jpHoliday, "Japan", nowEpoch); }

// Header, the holiday's name, then a countdown in days.
bool showHolidayFrom(const Holiday &h, const char *header, long nowEpoch) {
  if (!h.valid) return false;

  time_t t = (time_t)nowEpoch;
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  long today = daysFromCivil(tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday);
  long daysAway = h.civilDay - today;
  if (daysAway < 0) daysAway = 0;  // stale cache from just after it passed; next fetch refills

  showFrame(header, 1300);
  showFadeFrame(h.name, 2000);
  char frame[10];
  snprintf(frame, sizeof(frame), "%ldd", daysAway);
  showFrame(frame, 1300);
  return true;
}


//  _   _ ____  ____
// | \ | |  _ \|  _ \
// |  \| | |_) | |_) |
// | |\  |  __/|  _ <
// |_| \_|_|   |_| \_\
//
// NPR headlines, scraped from text.npr.org -- NPR's text-only site, ~6 KB,
// listed in homepage order (unlike the RSS feeds, which are newest-first and
// mix in the Up First newsletter and Spanish-language stories). Each
// headline is an <a class="topic-title">; we keep the first NEWS_MAX and
// show a random one each time.
#define NEWS_URL         "https://text.npr.org/"
#define NEWS_REFETCH_MS  900000     // 15 min
#define NEWS_MAX         10

String g_news[NEWS_MAX];
int    g_newsCount = 0;

// Headline text -> what the 14-segment font can draw: HTML entities decoded,
// and the common non-ASCII UTF-8 (curly quotes, dashes, accented letters)
// folded to plain ASCII. Anything else non-ASCII is dropped.
String asciiFold(const String &in) {
  String s = in;
  while (s.indexOf("&amp;") >= 0) s.replace("&amp;", "&");  // NPR sometimes escapes twice
  s.replace("&#38;", "&");
  s.replace("&#038;", "&");
  s.replace("&quot;", "\"");
  s.replace("&#39;", "'");
  s.replace("&#x27;", "'");
  s.replace("&apos;", "'");
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("\xE2\x80\x98", "'");   // curly single quotes
  s.replace("\xE2\x80\x99", "'");
  s.replace("\xE2\x80\x9C", "\"");  // curly double quotes
  s.replace("\xE2\x80\x9D", "\"");
  s.replace("\xE2\x80\x93", "-");   // en dash
  s.replace("\xE2\x80\x94", "-");   // em dash
  s.replace("\xE2\x80\xA6", "...");
  s.replace("\xC2\xA0", " ");       // no-break space

  String out;
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c < 0x80) { out += (char)c; continue; }
    if (c == 0xC3 && i + 1 < s.length()) {   // Latin-1 letters: U+00C0-U+00FF
      static const char latin[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
      uint8_t lo = s[i + 1];
      if (lo >= 0x80 && lo <= 0xBF) out += latin[lo - 0x80];
      i++;
      continue;
    }
    // other multi-byte sequence: skip its continuation bytes
    while (i + 1 < s.length() && ((uint8_t)s[i + 1] & 0xC0) == 0x80) i++;
  }
  out.trim();
  return out;
}

void fetchNews() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.begin(NEWS_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("NEWS HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }
  String page = http.getString();
  http.end();

  int n = 0;
  int at = 0;
  while (n < NEWS_MAX) {
    at = page.indexOf("class=\"topic-title\"", at);
    if (at < 0) break;
    int start = page.indexOf('>', at) + 1;
    int end   = page.indexOf("</a>", start);
    if (start <= 0 || end < 0) break;
    String h = asciiFold(page.substring(start, end));
    if (h.length()) g_news[n++] = h;
    at = end;
  }
  if (n) g_newsCount = n;  // a bad scrape keeps the last good batch
  Serial.printf("NEWS: %d headlines\n", n);
  progEnd(n ? '*' : '0');
}

// NEWS pulses in and out (fade, like the 8-ball intro -- it's not an alert,
// so no blink), then one random headline scrolls.
bool showNews(long) {
  if (!g_newsCount) return false;
  slideIn(center8("NEWS"));
  for (int i = 0; i < 2; i++) {
    fadeBrightnessBoth(BRIGHT_FULL, 30);
    fadeBrightnessBoth(0, 30);
  }
  showFadeFrame(g_news[random(g_newsCount)], 2500);
  return true;
}


//   ___              _
//  / _ \ _   _  ___ | |_ ___  ___
// | | | | | | |/ _ \| __/ _ \/ __|
// | |_| | |_| | (_) | ||  __/\__ \
//  \__\_\\__,_|\___/ \__\___||___/
//
// S&P 500, USD/JPY and Bitcoin, via Yahoo Finance's chart endpoint -- free
// and keyless, but undocumented (the one behind their own site), so it could
// start refusing us or change shape without notice. Live during market hours;
// otherwise it's the last close and that day's change. The change is computed
// from chartPreviousClose rather than read from Yahoo's own change fields, so
// it only depends on the two numbers.
#define QUOTE_URL_BASE   "https://query1.finance.yahoo.com/v8/finance/chart/"
#define QUOTE_REFETCH_MS 300000      // 5 min

struct Quote {
  const char *label;     // header frame
  const char *symbol;    // Yahoo symbol, URL-encoded
  int         decimals;  // for the level and point change
  float       bigPct;    // |change| at/above this -> the whole thing blinks
  float       price;     // the rest is the cache, zeroed as a global
  float       prevClose;
  bool        valid;
};
Quote g_spx = {"STOCKS",  "%5EGSPC", 2, 2.0f};
Quote g_yen = {"YEN",     "JPY%3DX", 2, 1.0f};   // yen per dollar
Quote g_btc = {"BITCOIN", "BTC-USD", 0, 5.0f};

void fetchQuote(Quote &q) {
  progBegin();
  String payload;
  if (!httpGet(q.label, String(QUOTE_URL_BASE) + q.symbol + "?interval=1d&range=1d", payload)) {
    progEnd('X');
    return;
  }

  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.printf("%s JSON parse error\n", q.label);
    progEnd('X');
    return;
  }

  JsonObject meta = doc["chart"]["result"][0]["meta"];
  float price = meta["regularMarketPrice"] | 0.0f;
  float prev  = meta["chartPreviousClose"] | 0.0f;
  if (price > 0 && prev > 0) {  // a bad fetch keeps the last good quote
    q.price     = price;
    q.prevClose = prev;
    q.valid     = true;
  }
  Serial.printf("%s: %.2f (prev %.2f)\n", q.label, price, prev);
  progEnd(price > 0 && prev > 0 ? '*' : '0');
}

// The label, the level, the point change, the percent change, then the level
// again to finish on. A move of q.bigPct or more either way blinks the whole
// sequence.
bool showQuote(const Quote &q) {
  if (!q.valid) return false;
  float chg = q.price - q.prevClose;
  float pct = chg / q.prevClose * 100.0f;
  bool  big = fabsf(pct) >= q.bigPct;

  char price[16], pts[16], pctTxt[16];
  snprintf(price,  sizeof(price),  "%.*f", q.decimals, q.price);
  snprintf(pts,    sizeof(pts),    "%+.*f", q.decimals, chg);
  snprintf(pctTxt, sizeof(pctTxt), "%+.2f%%", pct);

  if (big) blink(true);
  showFrame(center8(q.label), 1300);
  showFrame(price,  1800);
  showFrame(pts,    1500);
  showFrame(pctTxt, 1500);
  showFrame(price,  1500);
  if (big) blink(false);
  return true;
}

void fetchStocks()     { fetchQuote(g_spx); }
void fetchYen()        { fetchQuote(g_yen); }
void fetchBitcoin()    { fetchQuote(g_btc); }
bool showStocks(long)  { return showQuote(g_spx); }
bool showYen(long)     { return showQuote(g_yen); }
bool showBitcoin(long) { return showQuote(g_btc); }


//                          ___  _____ __  __
//   _ __  _   _  ___      / _ \| ____|  \/  |
//  | '_ \| | | |/ __|    | | | |  _| | |\/| |
//  | | | | |_| | (__     | |_| | |___| |  | |
//  |_| |_|\__, |\___|     \___/|_____|_|  |_|
//         |___/
//
// NYC OEM emergency alerts, via Notify NYC's live Everbridge CAP feed (see
// OEM_RSS_URL above for the full background/citations). Unlike NWS this feed
// covers everything from a subway delay to a building collapse, so we only
// ever surface an item when capIsHighUrgency() says so -- otherwise it'd be
// the noisiest thing on the display. g_oemAlert.event is "" when nothing
// currently qualifies.

CapAlert g_oemAlert;

// Pulls the first <tag>...</tag> value out of a flat XML blob. Not a real XML
// parser (no nesting/attribute/entity handling) -- CAP's <info> fields are
// flat text leaves and RSS <item>/<link> are too, so this is enough and avoids
// pulling in an XML library this project doesn't otherwise need.
String xmlTag(const String &xml, const char *tag) {
  String open  = String("<") + tag + ">";
  String close = String("</") + tag + ">";
  int i = xml.indexOf(open);
  if (i < 0) return "";
  i += open.length();
  int j = xml.indexOf(close, i);
  if (j < 0) return "";
  return xml.substring(i, j);
}

// OEM re-sends every alert once per language (senderName "NYCEM [English]",
// "NYCEM [Spanish]", etc, all with identical severity/urgency/event) -- only
// the English one should ever reach the display. Translations aren't
// reliably older/newer than the English item in feed order, so this can't be
// skipped by position and must be checked per-item against the CAP doc.
bool oemIsEnglish(const String &cap) {
  return xmlTag(cap, "senderName") == "NYCEM [English]";
}

// Cheap pre-filter on the RSS item's own <title>, before spending an HTTPS
// fetch on its CAP doc: OEM's English items are titled "Notify NYC - ...".
// That prefix alone isn't enough, though -- caught live on 2026-09-12, a
// "Public Pool Closure" alert's Yiddish/Urdu/Spanish/Russian/Polish/Korean
// items ALL carried the same "Notify NYC - <native text>" prefix (only a
// few, like French, happened to read as plain ASCII too), unlike a
// same-day "Basement Preparedness" alert whose translations had no prefix
// at all. Six prefixed-but-foreign items in a row burned through
// OEM_MAX_CAP_FETCHES before the real English item (or the next alert
// after it) was ever reached, so the display went blank instead of showing
// either alert. Requiring the whole title to be plain ASCII on top of the
// prefix fixes that: real English titles are ASCII, and almost every
// translation (accented Latin, Cyrillic, CJK, RTL scripts, or an em dash)
// fails it, so only ~1 stray ASCII translation (e.g. French) ever costs a
// wasted fetch instead of six. This is a convention, not a spec guarantee,
// so oemIsEnglish() above still makes the authoritative call against the
// fetched CAP doc's senderName -- this just avoids fetching a CAP doc at
// all for the items that are obviously not it.
bool oemTitleLooksEnglish(const String &title) {
  if (!title.startsWith("Notify NYC")) return false;
  for (size_t i = 0; i < title.length(); i++) {
    if ((uint8_t)title[i] >= 0x80) return false;  // non-ASCII byte -> a translation
  }
  return true;
}

// OEM headlines are wrapped as "Notify NYC - <specific title> (NYC)"; the
// wrapper is boilerplate repeated on every alert, so strip it and scroll just
// the specific title, e.g. "Basement Preparedness - 9/13".
String oemCleanHeadline(String h) {
  static const char PREFIX[] = "Notify NYC - ";
  static const char SUFFIX[] = " (NYC)";
  if (h.startsWith(PREFIX)) h = h.substring(strlen(PREFIX));
  if (h.endsWith(SUFFIX))   h = h.substring(0, h.length() - strlen(SUFFIX));
  return h;
}

// Per the OASIS CAP-feeds v1.0 spec, an RSS item wrapping a CAP alert points
// at the full CAP XML via <enclosure type="application/cap+xml" url="..."/>;
// some CAP-feed implementations instead put the CAP doc URL directly in
// <link>. Try enclosure first, then link -- same fallback proven out in
// tools/nyc_oem_test.py's _cap_doc_url().
String oemCapDocUrl(const String &item) {
  int enc = item.indexOf("<enclosure");
  if (enc >= 0 && item.indexOf("cap", enc) >= 0) {
    int u = item.indexOf("url=\"", enc);
    if (u >= 0) {
      u += 5;
      int uEnd = item.indexOf('"', u);
      if (uEnd > u) return item.substring(u, uEnd);
    }
  }
  return xmlTag(item, "link");
}

void fetchOemAlert() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(OEM_RSS_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OEM HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;  // keep the last known alert; don't drop a real one on a blip
  }
  String rss = http.getString();
  http.end();

  g_oemAlert = CapAlert();

  // Walk each <item>...</item> newest-first. Most are translations of the
  // same alert (see OEM_MAX_RSS_ITEMS above) -- skip those via a free string
  // check on the RSS title before ever touching the network, so the (much
  // smaller) OEM_MAX_CAP_FETCHES budget for the expensive part -- following a
  // candidate's CAP doc link for severity/urgency, which the RSS item doesn't
  // carry -- is normally spent on just the one English item. Log every item
  // actually fetched (not just a match) so the serial monitor can tell
  // "feed's quiet" apart from "feed's format changed and this is silently
  // finding nothing" -- the two look identical otherwise.
  int pos = 0;
  int scanned = 0;
  int fetched = 0;
  for (; scanned < OEM_MAX_RSS_ITEMS; scanned++) {
    int itemStart = rss.indexOf("<item>", pos);
    if (itemStart < 0) break;
    int itemEnd = rss.indexOf("</item>", itemStart);
    if (itemEnd < 0) break;
    String item = rss.substring(itemStart, itemEnd);
    pos = itemEnd + 7;

    String title = xmlTag(item, "title");
    if (!oemTitleLooksEnglish(title)) continue;  // translation -- no HTTP spent

    if (fetched >= OEM_MAX_CAP_FETCHES) {
      Serial.println("OEM: hit OEM_MAX_CAP_FETCHES, stopping early");
      break;
    }

    String capUrl = oemCapDocUrl(item);
    if (!capUrl.length()) {
      Serial.printf("OEM item %d: \"%s\" -- no CAP doc URL found (enclosure/link)\n",
                    fetched, title.c_str());
      fetched++;
      continue;
    }

    HTTPClient capHttp;
    capHttp.setUserAgent(USER_AGENT);
    capHttp.setConnectTimeout(4000);
    capHttp.setTimeout(4000);
    capHttp.begin(capUrl);
    int capCode = capHttp.GET();
    if (capCode != HTTP_CODE_OK) {
      Serial.printf("OEM item %d: \"%s\" -- CAP doc fetch HTTP %d\n",
                    fetched, title.c_str(), capCode);
      capHttp.end();
      fetched++;
      continue;
    }
    String cap = capHttp.getString();
    capHttp.end();
    fetched++;

    CapAlert a;
    a.event     = xmlTag(cap, "event");
    a.headline  = oemCleanHeadline(xmlTag(cap, "headline"));
    a.severity  = xmlTag(cap, "severity");
    a.urgency   = xmlTag(cap, "urgency");
    a.certainty = xmlTag(cap, "certainty");
    a.category  = xmlTag(cap, "category");
    a.sent      = isoToEpoch(xmlTag(cap, "sent").c_str());
    bool english = oemIsEnglish(cap);
    bool match = english && capIsHighUrgency(a);

    Serial.printf("OEM item: event=\"%s\" headline=\"%s\" severity=\"%s\" urgency=\"%s\" certainty=\"%s\" category=\"%s\" sent=%ld %s\n",
                  a.event.c_str(), a.headline.c_str(), a.severity.c_str(),
                  a.urgency.c_str(), a.certainty.c_str(), a.category.c_str(), a.sent,
                  match ? "-> SHOWING" : (english ? "(filtered out)" : "(non-English, skipped)"));

    if (match) {
      g_oemAlert = a;
      break;  // feed is newest-first, so the first qualifying item wins
    }
  }
  if (scanned == 0) Serial.println("OEM: 0 items in RSS feed");
  else if (fetched == 0) Serial.printf("OEM: %d items, none English-titled\n", scanned);

  Serial.printf("OEM: %s\n", g_oemAlert.event.length() ? g_oemAlert.event.c_str() : "(none)");
  progEnd(g_oemAlert.event.length() ? '*' : '0');
}

// OEM alerts can sit in the feed for days. Age the current one from its CAP
// <sent> time: past OEM_HALF_AFTER_S it shows every other cycle, past
// OEM_MAX_SHOW_S not at all. If <sent> is missing, fall back to when this
// device first showed it (keyed by headline + event, RAM only).
#define OEM_HALF_AFTER_S (12L * 3600)
#define OEM_MAX_SHOW_S   (24L * 3600)

bool oemShouldShow(long now) {
  if (!g_oemAlert.event.length()) return false;
  if (now < 1700000000L) return true;  // no clock -> can't age it

  static String seenKey;
  static long firstShown = 0;
  static bool skipNext = false;

  String key = g_oemAlert.headline + "|" + g_oemAlert.event;
  if (key != seenKey) {
    seenKey    = key;
    firstShown = now;
    skipNext   = false;
  }

  long age = now - (g_oemAlert.sent ? g_oemAlert.sent : firstShown);
  if (age > OEM_MAX_SHOW_S) return false;
  if (age > OEM_HALF_AFTER_S) {
    skipNext = !skipNext;
    return !skipNext;  // alternates: skip, show, skip, ...
  }
  return true;
}

// "25M AGO" / "13H AGO" since the alert's CAP <sent>; "" if unknown.
String oemAgeText(long now) {
  if (!g_oemAlert.sent || now < 1700000000L) return "";
  long age = now - g_oemAlert.sent;
  if (age < 0) age = 0;
  if (age < 3600) return String(age / 60) + "M AGO";
  return String(age / 3600) + "H AGO";
}


//                      _
//   __ _ _   _  __ _  | | _____
//  / _` | | | |/ _` | | |/ / _ \
// | (_| | |_| | (_| | |   <  __/
//  \__, |\__,_|\__,_| |_|\_\___|
//     |_|
//
// A notable earthquake near Tokyo. USGS gives the recent M4+ list; we surface
// one only if it's mag >= EQ_MIN_MAG or tsunami-flagged, and only if it landed
// in the last EQ_MAX_AGE_S seconds. Shown like a weather alert, except
// g_quakeTag shakes instead of blinking, then g_quakeLine scrolls steady
// (or "" when nothing).

String g_quakeTag;   // "QUAKE" or "TSUNAMI"
String g_quakeLine;  // "M5.2 74 KM NE"; "" when quiet

void fetchQuake() {
  long now = (long)time(nullptr);
  if (now < 1700000000L) return;      // no clock yet -> can't judge the 24h window

  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(EQ_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("EQ HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("EQ JSON parse error");
    progEnd('X');
    return;
  }

  g_quakeLine = "";
  for (JsonObject f : doc["features"].as<JsonArray>()) {   // newest first
    JsonObject pr = f["properties"];
    float     mag = pr["mag"] | 0.0f;
    bool      tsu = (pr["tsunami"] | 0) != 0;
    long long tms = pr["time"] | 0LL;                       // epoch milliseconds
    long      age = now - (long)(tms / 1000);

    if (age < 0 || age > EQ_MAX_AGE_S) continue;            // too old / clock skew
    if (mag < EQ_MIN_MAG && !tsu) continue;                 // not big enough, no tsunami

    JsonArray xy = f["geometry"]["coordinates"];             // [lon, lat, depth]
    float lon = xy[0] | 0.0f, lat = xy[1] | 0.0f;
    float km  = haversineKm(EQ_REF_LAT, EQ_REF_LON, lat, lon);
    g_quakeTag   = tsu ? "TSUNAMI" : "QUAKE";
    g_quakeLine  = "M" + String(mag, 1) + " " + String((int)roundf(km)) + " KM ";
    g_quakeLine += String(compass8(bearingDeg(EQ_REF_LAT, EQ_REF_LON, lat, lon)));
    break;
  }
  Serial.printf("EQ: %s\n", g_quakeLine.length() ? g_quakeLine.c_str() : "(none)");
  progEnd(g_quakeLine.length() ? '*' : '0');
}


//  ___ ____ ____
// |_ _/ ___/ ___|
//  | |\___ \___ \
//  | | ___) |__) |
// |___|____/____/
//
// Straight-line distance from home to the ISS's current ground point. See the
// comment on ISS_URL above for why this isn't a real visible-pass predictor.

bool g_issOverhead = false;

float haversineKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f;  // km
  float dLat = (lat2 - lat1) * DEG_TO_RAD;
  float dLon = (lon2 - lon1) * DEG_TO_RAD;
  float a = sinf(dLat / 2) * sinf(dLat / 2) +
            cosf(lat1 * DEG_TO_RAD) * cosf(lat2 * DEG_TO_RAD) *
                sinf(dLon / 2) * sinf(dLon / 2);
  return R * 2 * atan2f(sqrtf(a), sqrtf(1 - a));
}

// Initial compass bearing from (lat1,lon1) to (lat2,lon2), 0-360.
float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  float phi1 = lat1 * DEG_TO_RAD, phi2 = lat2 * DEG_TO_RAD;
  float dLon = (lon2 - lon1) * DEG_TO_RAD;
  float y = sinf(dLon) * cosf(phi2);
  float x = cosf(phi1) * sinf(phi2) - sinf(phi1) * cosf(phi2) * cosf(dLon);
  float deg = atan2f(y, x) * RAD_TO_DEG;
  if (deg < 0) deg += 360;
  return deg;
}

// Nearest 8-point compass letter(s) for a bearing -- used by the hurricane
// feed below, for both "which way is it from home" and "which way is it
// heading" -- and the Tokyo quake's direction from Kita, and squawks'.
const char *compass8(float deg) {
  static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int idx = ((int)((deg + 22.5f) / 45.0f)) % 8;
  return pts[idx];
}

void fetchIss() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.begin(ISS_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("ISS HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("ISS JSON parse error");
    progEnd('X');
    return;
  }

  float lat = doc["latitude"]  | 0.0f;
  float lon = doc["longitude"] | 0.0f;
  float km  = haversineKm(HOME_LAT, HOME_LON, lat, lon);
  g_issOverhead = km <= ISS_OVERHEAD_KM;
  Serial.printf("ISS: %.0f km away%s\n", km, g_issOverhead ? " -- OVERHEAD" : "");
  progEnd(g_issOverhead ? '*' : '0');
}

// One frame, same as a bus arrival -- shown only while it's actually overhead.
bool showIss(long) {
  if (!g_issOverhead) return false;
  showFrame("ISS OVER", 1300);
  return true;
}


//  _   _ _   _  ____
// | \ | | | | |/ ___|
// |  \| | |_| | |
// | |\  |  _  | |___
// |_| \_|_| |_|\____|
//
// The nearest active Atlantic named storm to home, if any. See the comment on
// NHC_URL above for the basin/depression filtering.

struct HurricaneInfo {
  String name;
  String classification;  // "TS", "HU", "PTC", "EX", ...
  int    windKt = 0;
  float  distanceMi = -1;
  float  bearingFromHome = 0;   // which way it is from home
  float  moveDir = 0;           // which way it's heading
  int    moveSpeedMph = 0;
  bool   valid = false;
};
HurricaneInfo g_hurricane;

// Saffir-Simpson category from max sustained wind (knots); 0 = not
// hurricane-strength.
int hurricaneCategory(int windKt) {
  if (windKt >= 137) return 5;
  if (windKt >= 113) return 4;
  if (windKt >= 96)  return 3;
  if (windKt >= 83)  return 2;
  if (windKt >= 64)  return 1;
  return 0;
}

void fetchHurricane() {
  progBegin();

  HTTPClient http;
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  http.begin(NHC_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("NHC HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }
  String payload = http.getString();
  http.end();

  // Each storm entry also carries a pile of GIS/advisory sub-objects (track
  // cones, KMZ links, ...) we have no use for -- filter down to the handful
  // of scalar fields the display actually needs.
  JsonDocument filter;
  filter["activeStorms"][0]["id"]               = true;
  filter["activeStorms"][0]["name"]             = true;
  filter["activeStorms"][0]["classification"]   = true;
  filter["activeStorms"][0]["intensity"]        = true;
  filter["activeStorms"][0]["latitudeNumeric"]  = true;
  filter["activeStorms"][0]["longitudeNumeric"] = true;
  filter["activeStorms"][0]["movementDir"]      = true;
  filter["activeStorms"][0]["movementSpeed"]    = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.println("NHC JSON parse error");
    progEnd('X');
    return;
  }

  g_hurricane = HurricaneInfo();  // clear -- stays invalid unless one qualifies below
  float bestMi = -1;

  for (JsonObject s : doc["activeStorms"].as<JsonArray>()) {
    String id = String(s["id"] | "");
    if (!id.startsWith("al")) continue;                 // Atlantic basin only

    String cls = String(s["classification"] | "");
    if (cls == "TD" || cls == "STD") continue;          // not named yet -- TS and up all show

    float lat = s["latitudeNumeric"]  | 0.0f;
    float lon = s["longitudeNumeric"] | 0.0f;
    float mi  = haversineKm(HOME_LAT, HOME_LON, lat, lon) * 0.621371f;
    if (bestMi >= 0 && mi >= bestMi) continue;          // keep only the nearest

    bestMi = mi;
    g_hurricane.valid           = true;
    g_hurricane.name            = String(s["name"] | "");
    g_hurricane.classification  = cls;
    // NHC sends intensity as a JSON *string* ("50"), unlike the numeric
    // fields above -- ArduinoJson's `| default` only returns the real value
    // when is<T>() already matches the target type, so `s["intensity"] | 0`
    // would silently always be 0 here. as<int>() does the string->int
    // conversion `|` doesn't.
    g_hurricane.windKt          = s["intensity"].as<int>();
    g_hurricane.moveDir         = s["movementDir"]   | 0;
    g_hurricane.moveSpeedMph    = s["movementSpeed"] | 0;  // NHC reports this in mph already
    g_hurricane.distanceMi      = mi;
    g_hurricane.bearingFromHome = bearingDeg(HOME_LAT, HOME_LON, lat, lon);
  }

  Serial.printf("NHC: %s\n", g_hurricane.valid
      ? (g_hurricane.name + " " + String((int)g_hurricane.distanceMi) + "mi").c_str()
      : "(none)");
  progEnd(g_hurricane.valid ? '*' : '0');
}

// Five frames, same cadence as a bus arrival: a "NOAA NHC" header, name,
// strength, distance + direction from home, then heading -- the last one is
// what tells you whether it's actually coming this way or just passing
// through the ocean.
bool showHurricane(long) {
  if (!g_hurricane.valid) return false;

  showFrame("NOAA NHC", 1300);  // header frame, same idea as "E B'WAY" for trains
  showFrame(g_hurricane.name, 1300);  // natural case; truncated to 8 cols if it runs long

  char frame[16];
  int cat = hurricaneCategory(g_hurricane.windKt);
  if (cat > 0) {
    snprintf(frame, sizeof(frame), "CAT %d", cat);
  } else {
    int mph = (int)(g_hurricane.windKt * 1.15078f);
    snprintf(frame, sizeof(frame), "%s %dmph", g_hurricane.classification.c_str(), mph);
  }
  showFrame(frame, 1300);

  snprintf(frame, sizeof(frame), "%dmi %s", (int)g_hurricane.distanceMi,
           compass8(g_hurricane.bearingFromHome));
  showFrame(frame, 1300);

  snprintf(frame, sizeof(frame), "%s %dmph", compass8(g_hurricane.moveDir),
           g_hurricane.moveSpeedMph);
  showFrame(frame, 1300);
  return true;
}


//  _____ ___  _   _ _____ _____
// | ____/ _ \| \ | | ____|_   _|
// |  _|| | | |  \| |  _|   | |
// | |__| |_| | |\  | |___  | |
// |_____\___/|_| \_|_____| |_|
//
// NASA EONET (Earth Observatory Natural Event Tracker), free, no key -- used
// only for the biggest open Antarctic iceberg. Gotchas:
//  - `days=` (last-update window) doesn't trim an event's `geometry[]` track
//    history, so a tracked berg carries ~50 points. `magMin=` keeps only the
//    giants (~18 KB instead of ~94 KB for all of them), and the response is
//    still parsed straight off the stream through a filter rather than
//    buffered into a String. useHTTP10() keeps the server from
//    chunk-encoding the body, which ArduinoJson can't read off a raw stream.
//  - The National Ice Center updates berg sizes every few days, so one fetch
//    a day is plenty.
#define EONET_ICE_URL     "https://eonet.gsfc.nasa.gov/api/v3/events?" \
                          "category=seaLakeIce&status=open&days=30&magID=sq_NM&magMin=300"
#define EONET_REFETCH_MS  86400000    // 24h
#define MANHATTAN_SQMI    22.8f       // for iceberg scale

struct Iceberg {
  String name;          // "A81"
  float  sqnm = 0;      // area of the latest geometry point, NM^2
  bool   valid = false;
};
Iceberg g_iceberg;

// Keeps the last data on an HTTP/parse error; clears it on an empty-but-OK
// response.
void fetchIceberg() {
  progBegin();

  HTTPClient http;
  http.useHTTP10(true);  // no chunked body -- see above
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(8000);
  http.begin(EONET_ICE_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("EONET HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }

  JsonDocument filter;
  filter["events"][0]["title"]                         = true;
  filter["events"][0]["geometry"][0]["magnitudeValue"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("EONET JSON %s\n", err.c_str());
    progEnd('X');
    return;
  }

  g_iceberg = Iceberg();
  for (JsonObject e : doc["events"].as<JsonArray>()) {
    JsonArray geo = e["geometry"];
    if (geo.size() == 0) continue;
    float sqnm = geo[geo.size() - 1]["magnitudeValue"] | 0.0f;  // oldest-first
    if (g_iceberg.valid && sqnm <= g_iceberg.sqnm) continue;

    String name = e["title"] | "";
    if (name.startsWith("Iceberg ")) name = name.substring(8);
    g_iceberg.name  = name;
    g_iceberg.sqnm  = sqnm;
    g_iceberg.valid = true;
  }

  Serial.printf("EONET: %s\n", g_iceberg.valid ? g_iceberg.name.c_str() : "(none)");
  progEnd(g_iceberg.valid ? '*' : '0');
}

// Biggest open iceberg: name, area, and how many Manhattans that is.
bool showIceberg(long) {
  if (!g_iceberg.valid) return false;

  char frame[20];
  showFrame("Iceberg", 1300);
  showFrame(g_iceberg.name, 1300);  // "A81"
  float sqmi = g_iceberg.sqnm * 1.32432f;
  snprintf(frame, sizeof(frame), "%d sqmi", (int)sqmi);
  showFrame(frame, 1300);
  int manhattans = (int)(sqmi / MANHATTAN_SQMI + 0.5f);
  if (manhattans >= 2) {
    snprintf(frame, sizeof(frame), "%dx Manhattan", manhattans);
    showFadeFrame(frame, 1500);
  }
  return true;
}


// Fires true the first time it's called, then again once every intervalMs --
// replaces the hand-copied "static lastMs + static first" pair each feed's
// refetch gate used to carry.
struct RefetchTimer {
  unsigned long lastMs = 0;
  bool first = true;
  bool due(unsigned long intervalMs) {
    if (!first && millis() - lastMs < intervalMs) return false;
    first = false;
    lastMs = millis();
    return true;
  }
};


//     _    _                       _
//    / \  (_)_ __ _ __   ___  _ __| |_ ___
//   / _ \ | | '__| '_ \ / _ \| '__| __/ __|
//  / ___ \| | |  | |_) | (_) | |  | |_\__ \
// /_/   \_\_|_|  | .__/ \___/|_|   \__|___/
//                |_|
//
// FAA's national airport status feed (nasstatus.faa.gov), free, no key, ~2 KB
// of XML covering every US airport with something going on. We only care
// about EWR, LGA and JFK. The feed groups records by <Delay_type> -- ground
// stops, ground delay programs, general arrival/departure delays, closures --
// each record carrying its own <ARPT>. First (most serious) entry per airport
// wins.
#define FAA_URL         "https://nasstatus.faa.gov/api/airport-status-information"
#define FAA_REFETCH_MS  300000        // 5 min
const char *const FAA_AIRPORTS[] = {"EWR", "LGA", "JFK"};
#define NUM_FAA_AIRPORTS (sizeof(FAA_AIRPORTS) / sizeof(FAA_AIRPORTS[0]))

String g_airportDelay[NUM_FAA_AIRPORTS];  // "LGA GROUND STOP - thunderstorms"; "" when normal

// "1 hour and 19 minutes" -> "1h19m", "16 minutes" -> "16m". Anything it
// can't make sense of comes back unchanged.
String durShort(const String &s) {
  String out;
  int num = -1;
  for (unsigned int i = 0; i < s.length();) {
    if (isDigit(s[i])) {
      num = 0;
      while (i < s.length() && isDigit(s[i])) num = num * 10 + (s[i++] - '0');
    } else if (isAlpha(s[i])) {
      char unit = tolower(s[i]);
      while (i < s.length() && isAlpha(s[i])) i++;
      if (num >= 0 && (unit == 'h' || unit == 'm')) out += String(num) + unit;
      num = -1;
    } else {
      i++;
    }
  }
  return out.length() ? out : s;
}

void fetchAirports() {
  progBegin();
  String xml;
  if (!httpGet("FAA", FAA_URL, xml)) {
    progEnd('X');
    return;
  }

  String found[NUM_FAA_AIRPORTS];
  int at = 0;
  while ((at = xml.indexOf("<Delay_type>", at)) >= 0) {
    int end = xml.indexOf("</Delay_type>", at);
    if (end < 0) break;
    String block = xml.substring(at, end);
    at = end;
    String kind = xmlTag(block, "Name");  // "Ground Stop Programs", ...

    for (int r = block.indexOf("<ARPT>"); r >= 0;) {
      int next = block.indexOf("<ARPT>", r + 6);
      String rec = block.substring(r, next < 0 ? block.length() : next);
      r = next;

      String arpt = xmlTag(rec, "ARPT");
      for (size_t i = 0; i < NUM_FAA_AIRPORTS; i++) {
        if (arpt != FAA_AIRPORTS[i] || found[i].length()) continue;
        String what;
        String reason = xmlTag(rec, "Reason");
        if (kind.indexOf("Ground Stop") >= 0) {
          what = "GROUND STOP";
          String until = xmlTag(rec, "End_Time");
          if (until.length()) what += " TIL " + until;
        } else if (kind.indexOf("Ground Delay") >= 0) {
          what = "DELAYS AVG " + durShort(xmlTag(rec, "Avg"));
        } else if (kind.indexOf("Closure") >= 0) {
          what = "CLOSED";
          reason = "";  // a raw NOTAM, far too long to scroll
        } else {
          what  = rec.indexOf("\"Arrival\"") >= 0 ? "ARR DELAYS " : "DEP DELAYS ";
          what += durShort(xmlTag(rec, "Min")) + "-" + durShort(xmlTag(rec, "Max"));
        }
        found[i] = String(FAA_AIRPORTS[i]) + " " + what;
        if (reason.length()) found[i] += " - " + reason;
      }
    }
  }

  int n = 0;
  for (size_t i = 0; i < NUM_FAA_AIRPORTS; i++) {
    g_airportDelay[i] = found[i];
    if (found[i].length()) {
      Serial.printf("FAA: %s\n", found[i].c_str());
      n++;
    }
  }
  if (!n) Serial.println("FAA: EWR/LGA/JFK normal");
  progEnd(n ? '*' : '0');
}

bool hasAirportDelays() {
  for (size_t i = 0; i < NUM_FAA_AIRPORTS; i++)
    if (g_airportDelay[i].length()) return true;
  return false;
}

// DELAYS, then one scrolling line per affected airport.
bool showAirports(long) {
  if (!hasAirportDelays()) return false;
  showFrame(center8("DELAYS"), 1300);
  for (size_t i = 0; i < NUM_FAA_AIRPORTS; i++)
    if (g_airportDelay[i].length()) showFadeFrame(g_airportDelay[i], 2000);
  return true;
}


//     _
//    / \  _   _ _ __ ___  _ __ __ _
//   / _ \| | | | '__/ _ \| '__/ _` |
//  / ___ \ |_| | | | (_) | | | (_| |
// /_/   \_\__,_|_|  \___/|_|  \__,_|
//
// NOAA SWPC's planetary K-index, the standard 0-9 geomagnetic storm scale,
// ~5 KB of JSON covering the last week in 3-hour steps. Kp 7+ (a strong, G3
// storm) is roughly when the aurora can reach NYC's latitude -- as it did in
// May 2024 -- so that's when it joins the urgent alerts. Rare by design.
#define KP_URL         "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json"
#define KP_REFETCH_MS  900000         // 15 min
#define AURORA_KP      7.0f
#define KP_MAX_AGE_S   (6L * 3600)    // ignore a reading older than this

float g_kp = -1;  // latest Kp; -1 when unknown or stale

void fetchKp() {
  progBegin();
  String payload;
  if (!httpGet("KP", KP_URL, payload)) {
    progEnd('X');
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("KP JSON parse error");
    progEnd('X');
    return;
  }
  JsonArray rows = doc.as<JsonArray>();
  if (rows.size() == 0) {
    progEnd('0');
    return;
  }

  // Oldest-first. Rows are objects today; SWPC used to send arrays of
  // strings (header row first), so accept either shape.
  JsonVariant last = rows[rows.size() - 1];
  float  kp;
  String when;
  if (last.is<JsonObject>()) {
    kp   = last["Kp"] | -1.0f;
    when = String(last["time_tag"] | "");
  } else {
    kp   = atof(last[1] | "-1");
    when = String(last[0] | "");
  }
  when.replace(' ', 'T');  // "2026-10-01 09:00:00.000" -> isoToEpoch()'s shape

  long now = (long)time(nullptr);
  if (now > 1700000000L && now - isoToEpoch(when.c_str()) > KP_MAX_AGE_S) kp = -1;
  g_kp = kp;
  Serial.printf("KP: %.2f\n", g_kp);
  progEnd(g_kp >= 0 ? '*' : '0');
}


// __        __    _
// \ \      / /_ _| |_ ___ _ __
//  \ \ /\ / / _` | __/ _ \ '__|
//   \ V  V / (_| | ||  __/ |
//    \_/\_/ \__,_|\__\___|_|
//
// Harbor water temperature at The Battery -- same NOAA CO-OPS API and station
// as the tide feed, product=water_temperature, ~150 bytes.
#define WATER_URL        "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=water_temperature&station=8518750&date=latest&units=english&time_zone=lst_ldt&format=json"
#define WATER_REFETCH_MS 1800000      // 30 min

float g_waterF = -1000;  // -1000 = no reading yet

void fetchWater() {
  progBegin();
  String payload;
  if (!httpGet("WATER", WATER_URL, payload)) {
    progEnd('X');
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("WATER JSON parse error");
    progEnd('X');
    return;
  }
  const char *v = doc["data"][0]["v"] | "";  // a string: "66.6"
  if (v[0]) g_waterF = atof(v);              // a bad reading keeps the last one
  Serial.printf("WATER: %s\n", v[0] ? v : "(none)");
  progEnd(v[0] ? '*' : '0');
}

bool showWater(long) {
  if (g_waterF < -100) return false;
  showFrame("Water", 1300);
  char frame[12];
  snprintf(frame, sizeof(frame), "%dF", (int)roundf(g_waterF));
  showFrame(frame, 1500);
  return true;
}


//  _____     _
// |_   _|__ | | ___   _  ___
//   | |/ _ \| |/ / | | |/ _ \
//   | | (_) |   <| |_| | (_) |
//   |_|\___/|_|\_\\__, |\___/
//                 |___/
//
// Local time and weather in Kita City (EQ_REF_LAT/LON, same spot the quake
// distance is measured from), via Open-Meteo -- free, no key, ~400 bytes.
// The clock is our own NTP time shifted to JST; Japan has no DST.
#define TOKYO_WX_URL        "https://api.open-meteo.com/v1/forecast?latitude=35.7528&longitude=139.7336&current=temperature_2m,weather_code"
#define TOKYO_WX_REFETCH_MS 1800000   // 30 min
#define TOKYO_UTC_OFFSET_S  32400     // JST = UTC+9

struct TokyoNow {
  float tempC = 0;
  int   code  = 0;   // WMO weather code
  bool  valid = false;
};
TokyoNow g_tokyo;

// WMO weather interpretation code (what Open-Meteo returns) -> a few words.
const char *wmoText(int c) {
  if (c == 0)  return "Clear";
  if (c == 1)  return "Mostly clear";
  if (c == 2)  return "Partly cloudy";
  if (c == 3)  return "Overcast";
  if (c <= 48) return "Fog";
  if (c <= 57) return "Drizzle";
  if (c <= 67) return "Rain";
  if (c <= 77) return "Snow";
  if (c <= 82) return "Showers";
  if (c <= 86) return "Snow showers";
  return "Thunderstorm";
}

void fetchTokyo() {
  progBegin();
  String payload;
  if (!httpGet("TOKYO", TOKYO_WX_URL, payload)) {
    progEnd('X');
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload) || doc["current"]["temperature_2m"].isNull()) {
    Serial.println("TOKYO JSON parse error");
    progEnd('X');
    return;
  }
  g_tokyo.tempC = doc["current"]["temperature_2m"];
  g_tokyo.code  = doc["current"]["weather_code"] | 0;
  g_tokyo.valid = true;
  Serial.printf("TOKYO: %.1fC %s\n", g_tokyo.tempC, wmoText(g_tokyo.code));
  progEnd('*');
}

// Tokyo, the time there, the temperature in both units, then the sky.
bool showTokyo(long nowEpoch) {
  if (!g_tokyo.valid) return false;
  showFrame("Tokyo", 1300);

  char frame[16];
  if (nowEpoch > 1700000000L) {
    time_t t = (time_t)(nowEpoch + TOKYO_UTC_OFFSET_S);
    struct tm tmJst;
    gmtime_r(&t, &tmJst);
    int h12 = tmJst.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    snprintf(frame, sizeof(frame), "%d-%02d%s", h12, tmJst.tm_min, tmJst.tm_hour < 12 ? "am" : "pm");
    showFrame(frame, 1500);
  }

  snprintf(frame, sizeof(frame), "%dC %dF", (int)roundf(g_tokyo.tempC), (int)roundf(cToF(g_tokyo.tempC)));
  showFrame(frame, 1500);
  showFadeFrame(wmoText(g_tokyo.code), 1500);
  return true;
}


//     _    _
//    / \  (_)_ __
//   / _ \ | | '__|
//  / ___ \| | |
// /_/   \_\_|_|
//
// US AQI and UV index at home, via Open-Meteo's air-quality API -- free, no
// key, ~400 bytes. One fetch feeds two entries in FEEDS: Air (always) and UV
// (only once it's high), so fetchAir() gates itself rather than letting each
// entry's own timer fetch it twice.
#define AIR_URL         "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=40.7168&longitude=-73.9861&current=us_aqi,uv_index"
#define AIR_REFETCH_MS  1800000       // 30 min
#define AQI_BLINK       101           // "unhealthy for sensitive groups" and worse
#define UV_SHOW_MIN     6.0f          // "high" and up

struct AirNow {
  int   aqi = -1;  // -1 = no reading yet
  float uv  = -1;
};
AirNow g_air;

void fetchAir() {
  static RefetchTimer timer;
  if (!timer.due(AIR_REFETCH_MS)) return;

  progBegin();
  String payload;
  if (!httpGet("AIR", AIR_URL, payload)) {
    progEnd('X');
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload) || doc["current"]["us_aqi"].isNull()) {
    Serial.println("AIR JSON parse error");
    progEnd('X');
    return;
  }
  g_air.aqi = doc["current"]["us_aqi"];
  g_air.uv  = doc["current"]["uv_index"] | -1.0f;
  Serial.printf("AIR: AQI %d, UV %.1f\n", g_air.aqi, g_air.uv);
  progEnd('*');
}

const char *aqiText(int aqi) {
  if (aqi <= 50)  return "Good";
  if (aqi <= 100) return "Moderate";
  if (aqi <= 150) return "Unhealthy for some";
  if (aqi <= 200) return "Unhealthy";
  if (aqi <= 300) return "Very unhealthy";
  return "Hazardous";
}

const char *uvText(float uv) {
  if (uv < 3)  return "Low";
  if (uv < 6)  return "Moderate";
  if (uv < 8)  return "High";
  if (uv < 11) return "Very high";
  return "Extreme";
}

// Air, the AQI, then what it means. Blinks throughout once it's unhealthy.
bool showAir(long) {
  if (g_air.aqi < 0) return false;
  bool bad = g_air.aqi >= AQI_BLINK;
  if (bad) blink(true);
  showFrame("Air", 1300);
  char frame[12];
  snprintf(frame, sizeof(frame), "AQI %d", g_air.aqi);
  showFrame(frame, 1500);
  showFadeFrame(aqiText(g_air.aqi), 1500);
  if (bad) blink(false);
  return true;
}

bool showUv(long) {
  if (g_air.uv < UV_SHOW_MIN) return false;
  char frame[12];
  snprintf(frame, sizeof(frame), "UV %d", (int)roundf(g_air.uv));
  showFrame(frame, 1500);
  showFadeFrame(uvText(g_air.uv), 1500);
  return true;
}
//     _        _                 _     _
//    / \   ___| |_ ___ _ __ ___ (_) __| |
//   / _ \ / __| __/ _ \ '__/ _ \| |/ _` |
//  / ___ \\__ \ ||  __/ | | (_) | | (_| |
// /_/   \_\___/\__\___|_|  \___/|_|\__,_|
//
// The closest asteroid flyby in the next week, from NASA/JPL's close-approach
// API -- free, no key, ~1 KB. dist-max=0.05 au (~19 lunar distances) and
// sort=dist&limit=1 hand back just the nearest one. Size is estimated from
// its absolute magnitude H assuming a typical 14% albedo -- good to within a
// factor of two or so, which is all a "~50m wide" needs.
#define CAD_URL         "https://ssd-api.jpl.nasa.gov/cad.api?dist-max=0.05&date-min=now&date-max=%2B7&sort=dist&limit=1"
#define CAD_REFETCH_MS  21600000      // 6h
#define MI_PER_AU       92955807.0f

struct Asteroid {
  String name;        // "2019 AS2"
  float  mi    = 0;   // miss distance
  int    sizeM = 0;   // estimated diameter
  long   epoch = 0;   // time of closest approach, UTC
  bool   valid = false;
};
Asteroid g_rock;

// "2026-Oct-02 19:28" -> UTC epoch (JPL's TDB is ~1 min off UTC; close enough).
long cadTimeToEpoch(const char *cd) {
  int Y, D, h, m;
  char mon[4];
  if (sscanf(cd, "%d-%3s-%d %d:%d", &Y, mon, &D, &h, &m) < 5) return 0;
  const char *p = strstr("JanFebMarAprMayJunJulAugSepOctNovDec", mon);
  if (!p) return 0;
  int Mo = (p - "JanFebMarAprMayJunJulAugSepOctNovDec") / 3 + 1;
  return daysFromCivil(Y, Mo, D) * 86400L + h * 3600L + m * 60L;
}

void fetchAsteroid() {
  progBegin();
  String payload;
  if (!httpGet("CAD", CAD_URL, payload)) {
    progEnd('X');
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("CAD JSON parse error");
    progEnd('X');
    return;
  }

  // fields: des, orbit_id, jd, cd, dist, dist_min, dist_max, v_rel, v_inf, t_sigma_f, h
  JsonArray row = doc["data"][0];
  g_rock.valid = false;
  if (!row.isNull()) {
    float au = atof(row[4] | "0");
    float H  = atof(row[10] | "30");
    g_rock.name  = String(row[0] | "");
    g_rock.mi    = au * MI_PER_AU;
    g_rock.sizeM = (int)roundf(3551930.0f * powf(10.0f, -H / 5.0f));  // 1329 km / sqrt(0.14)
    g_rock.epoch = cadTimeToEpoch(row[3] | "");
    g_rock.valid = g_rock.name.length() && g_rock.epoch > 0;
  }
  Serial.printf("CAD: %s\n", g_rock.valid ? g_rock.name.c_str() : "(none within range)");
  progEnd(g_rock.valid ? '*' : '0');
}

// NY-local day count from now until epoch: "Today", "Tomorrow", "in 3d".
String daysAwayText(long epoch, long nowEpoch) {
  time_t a = (time_t)epoch, b = (time_t)nowEpoch;
  struct tm ta, tb;
  localtime_r(&a, &ta);
  localtime_r(&b, &tb);
  long d = daysFromCivil(ta.tm_year + 1900, ta.tm_mon + 1, ta.tm_mday) -
           daysFromCivil(tb.tm_year + 1900, tb.tm_mon + 1, tb.tm_mday);
  if (d <= 0) return "Today";
  if (d == 1) return "Tomorrow";
  return "in " + String(d) + "d";
}

// Asteroid, its name, how big, how close, and when.
bool showAsteroid(long nowEpoch) {
  if (!g_rock.valid) return false;
  char frame[16];
  showFrame("Asteroid", 1300);
  showFadeFrame(g_rock.name, 1500);
  snprintf(frame, sizeof(frame), "%dm wide", g_rock.sizeM);
  showFadeFrame(frame, 1500);
  if (g_rock.mi < 1000000.0f) snprintf(frame, sizeof(frame), "%dK mi", (int)(g_rock.mi / 1000.0f));
  else                        snprintf(frame, sizeof(frame), "%.1fM mi", g_rock.mi / 1000000.0f);
  showFrame(frame, 1500);
  showFrame(daysAwayText(g_rock.epoch, nowEpoch), 1500);
  return true;
}


//  _                           _
// | |    __ _ _   _ _ __   ___| |__
// | |   / _` | | | | '_ \ / __| '_ \
// | |__| (_| | |_| | | | | (__| | | |
// |_____\__,_|\__,_|_| |_|\___|_| |_|
//
// The next rocket launch anywhere, from The Space Devs' Launch Library 2 --
// free, no key, ~1 KB with limit=1&mode=list. The free tier allows 15
// requests an hour, so this polls hourly. Names come as
// "Falcon 9 Block 5 | Crew-13": rocket before the bar, mission after.
#define LAUNCH_URL         "https://ll.thespacedevs.com/2.3.0/launches/upcoming/?limit=1&mode=list"
#define LAUNCH_REFETCH_MS  3600000    // 1h

struct Launch {
  String rocket;      // "Falcon 9"
  String mission;     // "Crew-13"
  long   net = 0;     // scheduled liftoff, UTC
  bool   valid = false;
};
Launch g_launch;

void fetchLaunch() {
  progBegin();
  String payload;
  if (!httpGet("LAUNCH", LAUNCH_URL, payload)) {
    progEnd('X');
    return;
  }
  JsonDocument filter;
  filter["results"][0]["name"] = true;
  filter["results"][0]["net"]  = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.println("LAUNCH JSON parse error");
    progEnd('X');
    return;
  }

  JsonObject r = doc["results"][0];
  String name  = asciiFold(String(r["name"] | ""));
  long   net   = isoToEpoch(r["net"] | "");
  if (!name.length() || !net) {
    progEnd('0');
    return;  // keep the last one
  }
  int bar = name.indexOf(" | ");
  g_launch.rocket  = bar < 0 ? String("") : name.substring(0, bar);
  g_launch.mission = bar < 0 ? name : name.substring(bar + 3);
  int block = g_launch.rocket.indexOf(" Block");  // "Falcon 9 Block 5" -> "Falcon 9"
  if (block > 0) g_launch.rocket = g_launch.rocket.substring(0, block);
  g_launch.net   = net;
  g_launch.valid = true;
  Serial.printf("LAUNCH: %s / %s at %ld\n", g_launch.rocket.c_str(), g_launch.mission.c_str(), net);
  progEnd('*');
}

// Hidden once it's an hour past liftoff, until the next fetch moves on.
bool hasLaunch() {
  long now = (long)time(nullptr);
  return g_launch.valid && (now < 1700000000L || now < g_launch.net + 3600);
}

// Launch, the mission, the rocket, then a T-minus countdown.
bool showLaunch(long nowEpoch) {
  if (!hasLaunch()) return false;
  showFrame("Launch", 1300);
  showFadeFrame(g_launch.mission, 1500);
  if (g_launch.rocket.length()) showFadeFrame(g_launch.rocket, 1500);

  if (nowEpoch > 1700000000L) {
    long d = g_launch.net - nowEpoch;
    char frame[16];
    if (d < 0)               snprintf(frame, sizeof(frame), "T+%ldm", -d / 60);
    else if (d >= 2 * 86400) snprintf(frame, sizeof(frame), "T-%ldd", d / 86400);
    else if (d >= 3600)      snprintf(frame, sizeof(frame), "T-%ldh%02ldm", d / 3600, (d % 3600) / 60);
    else                     snprintf(frame, sizeof(frame), "T-%ldm", d / 60);
    showFrame(frame, 1500);
  }
  return true;
}


//  ____              _ _       _     _
// |  _ \  __ _ _   _| (_) __ _| |__ | |_
// | | | |/ _` | | | | | |/ _` | '_ \| __|
// | |_| | (_| | |_| | | | (_| | | | | |_
// |____/ \__,_|\__, |_|_|\__, |_| |_|\__|
//              |___/     |___/
//
// How much daylight today, and how that compares with yesterday -- two calls
// to the same sunrise-sunset.org API as the sunrise/sunset feed, which takes
// date=today / date=yesterday directly and returns day_length in seconds.
#define DAYLEN_URL_BASE   "https://api.sunrise-sunset.org/json?lat=40.7168&lng=-73.9861&formatted=0&tzid=America/New_York&date="
#define DAYLEN_REFETCH_MS 21600000    // 6h

long g_dayLen[2] = {-1, -1};  // seconds: [0] today, [1] yesterday

void fetchDaylight() {
  const char *days[2] = {"today", "yesterday"};
  for (int i = 0; i < 2; i++) {
    progBegin();
    String payload;
    if (!httpGet("DAYLEN", String(DAYLEN_URL_BASE) + days[i], payload)) {
      progEnd('X');
      continue;
    }
    JsonDocument filter;
    filter["results"]["day_length"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
      Serial.println("DAYLEN JSON parse error");
      progEnd('X');
      continue;
    }
    long len = doc["results"]["day_length"] | -1L;
    if (len > 0) g_dayLen[i] = len;
    progEnd(len > 0 ? '*' : '0');
  }
  Serial.printf("DAYLEN: %ld s, yesterday %ld s\n", g_dayLen[0], g_dayLen[1]);
}

// Daylight, today's length, then the change since yesterday ("-2m40s").
bool showDaylight(long) {
  if (g_dayLen[0] < 0 || g_dayLen[1] < 0) return false;
  char frame[16];
  showFrame("Daylight", 1300);
  snprintf(frame, sizeof(frame), "%ldh%02ldm", g_dayLen[0] / 3600, (g_dayLen[0] % 3600) / 60);
  showFrame(frame, 1500);
  long d    = g_dayLen[0] - g_dayLen[1];
  char sign = d < 0 ? '-' : '+';
  d = labs(d);
  if (d >= 60) snprintf(frame, sizeof(frame), "%c%ldm%02lds", sign, d / 60, d % 60);
  else         snprintf(frame, sizeof(frame), "%c%lds", sign, d);
  showFrame(frame, 1500);
  return true;
}


//  ____
// / ___|  ___  __ _ ___  ___  _ __  ___
// \___ \ / _ \/ _` / __|/ _ \| '_ \/ __|
//  ___) |  __/ (_| \__ \ (_) | | | \__ \
// |____/ \___|\__,_|___/\___/|_| |_|___/
//
// Countdown to the next equinox or solstice. No network: Meeus, Astronomical
// Algorithms ch. 27, table 27.C (mean equinoxes/solstices, years 2000-3000),
// which lands within ~10 minutes of the real moment -- plenty for a day count.

// k: 0 March equinox, 1 June solstice, 2 September equinox, 3 December solstice.
long seasonEpoch(int year, int k) {
  static const double c[4][5] = {
    {2451623.80984, 365242.37404,  0.05169, -0.00411, -0.00057},
    {2451716.56767, 365241.62603,  0.00325,  0.00888, -0.00030},
    {2451810.21715, 365242.01767, -0.11575,  0.00337,  0.00078},
    {2451900.05952, 365242.74049, -0.06223, -0.00823,  0.00032},
  };
  const double *a = c[k];
  double Y   = (year - 2000) / 1000.0;
  double jde = a[0] + Y * (a[1] + Y * (a[2] + Y * (a[3] + Y * a[4])));
  return (long)((jde - 2440587.5) * 86400.0);  // Julian day -> Unix epoch
}

// The season about to start, then how many days off it is.
bool showSeasons(long nowEpoch) {
  if (nowEpoch < 1700000000L) return false;
  static const char *const NAMES[4] = {"Spring", "Summer", "Autumn", "Winter"};
  time_t t = (time_t)nowEpoch;
  struct tm now;
  localtime_r(&t, &now);
  long today = daysFromCivil(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);

  for (int y = now.tm_year + 1900; y <= now.tm_year + 1901; y++) {
    for (int k = 0; k < 4; k++) {
      time_t e = (time_t)seasonEpoch(y, k);
      struct tm te;
      localtime_r(&e, &te);
      long d = daysFromCivil(te.tm_year + 1900, te.tm_mon + 1, te.tm_mday) - today;
      if (d < 0) continue;
      scrollAcross("* Season *");
      showFrame(NAMES[k], 1300);
      char frame[12];
      if (d == 0)      snprintf(frame, sizeof(frame), "Today!");
      else if (d == 1) snprintf(frame, sizeof(frame), "Tomorrow");
      else             snprintf(frame, sizeof(frame), "%ld days", d);
      showFrame(frame, 1500);
      return true;
    }
  }
  return false;
}


//   ____ ___ ____
//  / ___/ _ \___ \
// | |  | | | |__) |
// | |__| |_| / __/
//  \____\___/_____|
//
// Atmospheric CO2 at Mauna Loa, from NOAA GML's daily record. The file is
// ~570 KB going back to 1974, but the server honors Range requests, so we ask
// for just the last 300 bytes and read the final line:
// "  2026   9  30  2026.7452    425.81".
#define CO2_URL         "https://gml.noaa.gov/webdata/ccgg/trends/co2/co2_daily_mlo.txt"
#define CO2_REFETCH_MS  43200000      // 12h -- it updates daily
#define CO2_PREINDUSTRIAL 280.0f      // ppm, the usual pre-1750 baseline

float g_co2 = -1;

void fetchCo2() {
  progBegin();
  String tail;
  if (!httpGet("CO2", CO2_URL, tail, "bytes=-300")) {
    progEnd('X');
    return;
  }
  tail.trim();
  String line = tail.substring(tail.lastIndexOf('\n') + 1);
  int Y, Mo, D;
  float dec, ppm;
  if (sscanf(line.c_str(), "%d %d %d %f %f", &Y, &Mo, &D, &dec, &ppm) == 5 && ppm > 0) {
    g_co2 = ppm;
    Serial.printf("CO2: %.2f ppm (%d-%02d-%02d)\n", ppm, Y, Mo, D);
    progEnd('*');
  } else {
    Serial.printf("CO2: can't parse \"%s\"\n", line.c_str());
    progEnd('X');
  }
}

bool showCo2(long) {
  if (g_co2 < 0) return false;
  showNoiseFrame(center8("**CO2**"), 1300);  // scrambles in...
  noiseMorph(g_frame, "        ", -1);         // ...and back out
  g_frame = "        ";
  char frame[40];
  snprintf(frame, sizeof(frame), "%.1fppm", g_co2);
  showFrame(frame, 1500);
  snprintf(frame, sizeof(frame), "+%d%% over pre-industrial",
           (int)roundf((g_co2 / CO2_PREINDUSTRIAL - 1) * 100));
  showFadeFrame(frame, 1500);
  return true;
}


// __        __            _
// \ \      / /__  _ __ __| |
//  \ \ /\ / / _ \| '__/ _` |
//   \ V  V / (_) | | | (_| |
//    \_/\_/ \___/|_|  \__,_|
//
// Merriam-Webster's word of the day, from their RSS feed. The whole feed is
// ~50 KB (ten days of entries with examples and etymology), but today's word
// and its one-line definition are in the first ~2 KB, so we read only until
// "See the entry" -- the link right after the definition -- and hang up.
#define WOTD_URL        "https://www.merriam-webster.com/wotd/feed/rss2"
#define WOTD_REFETCH_MS 21600000      // 6h
#define WOTD_MAX_BYTES  8000          // give up if the marker never shows

struct WordOfDay {
  String word;  // "slew"
  String pos;   // "noun"
  String def;   // "Slew is an informal word for a large number of people or things."
  bool   valid = false;
};
WordOfDay g_word;

// HTML fragment -> plain display text: tags dropped, entities and non-ASCII
// folded by asciiFold(), runs of whitespace collapsed.
String stripTags(const String &html) {
  String s;
  bool inTag = false;
  for (unsigned int i = 0; i < html.length(); i++) {
    char c = html[i];
    if (c == '<') inTag = true;
    else if (c == '>') inTag = false;
    else if (!inTag) s += (c == '\n' || c == '\t') ? ' ' : c;
  }
  s.replace("&nbsp;", " ");
  for (int i = s.indexOf("&#"); i >= 0; i = s.indexOf("&#", i)) {  // numeric entities
    int end = s.indexOf(';', i);
    if (end < 0 || end - i > 8) break;
    long n = s.substring(i + 2, end).toInt();
    String r = (n == 8216 || n == 8217) ? "'" : (n == 8220 || n == 8221) ? "\""
             : (n == 8211 || n == 8212) ? "-" : (n >= 32 && n < 127) ? String((char)n) : "";
    s = s.substring(0, i) + r + s.substring(end + 1);
  }
  s = asciiFold(s);
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  s.trim();
  return s;
}

void fetchWord() {
  progBegin();

  HTTPClient http;
  http.useHTTP10(true);  // a plain body, no chunk-size lines mixed in
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.begin(WOTD_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("WOTD HTTP %d\n", code);
    http.end();
    progEnd('X');
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  String buf;
  buf.reserve(WOTD_MAX_BYTES);
  char chunk[257];
  unsigned long start = millis();
  while ((http.connected() || stream->available()) &&
         buf.length() < WOTD_MAX_BYTES && millis() - start < 8000) {
    size_t avail = stream->available();
    if (avail == 0) { delay(1); continue; }
    if (avail > sizeof(chunk) - 1) avail = sizeof(chunk) - 1;
    size_t got = stream->readBytes(chunk, avail);
    chunk[got] = '\0';
    buf += chunk;
    if (buf.indexOf("See the entry") >= 0) break;
  }
  http.end();

  // <item><title><![CDATA[slew]]></title> ... <em>noun</em><br /> <p>definition</p>
  int item = buf.indexOf("<item>");
  int t0   = item < 0 ? -1 : buf.indexOf("<![CDATA[", item);
  int t1   = t0 < 0 ? -1 : buf.indexOf("]]>", t0);
  int br   = t1 < 0 ? -1 : buf.indexOf("<br />", t1);
  int em1  = br < 0 ? -1 : buf.lastIndexOf("</em>", br);
  int em0  = em1 < 0 ? -1 : buf.lastIndexOf("<em>", em1);
  int p0   = br < 0 ? -1 : buf.indexOf("<p>", br);
  int p1   = p0 < 0 ? -1 : buf.indexOf("</p>", p0);
  if (p1 < 0 || em0 < t1) {
    Serial.println("WOTD: couldn't find today's entry");
    progEnd('X');
    return;
  }
  g_word.word  = stripTags(buf.substring(t0 + 9, t1));
  g_word.word.toUpperCase();  // easier to read than lowercase on 14 segments
  g_word.pos   = stripTags(buf.substring(em0 + 4, em1));
  g_word.def   = firstSentence(stripTags(buf.substring(p0 + 3, p1)));
  g_word.valid = g_word.word.length() && g_word.def.length();
  Serial.printf("WOTD: %s (%s)\n", g_word.word.c_str(), g_word.pos.c_str());
  progEnd(g_word.valid ? '*' : '0');
}

// Word, the word, its part of speech, then the definition.
bool showWord(long) {
  if (!g_word.valid) return false;
  showFrame("Word", 1300);
  showFadeFrame(g_word.word, 1800);
  if (g_word.pos.length()) showFrame(g_word.pos, 1300);
  showFadeFrame(g_word.def, 2000);
  return true;
}
//  ____                   _
// / ___| _ __   ___  _ __| |_ ___
// \___ \| '_ \ / _ \| '__| __/ __|
//  ___) | |_) | (_) | |  | |_\__ \
// |____/| .__/ \___/|_|   \__|___/
//       |_|
//
// Next (or just-finished, or in-progress) game for each team below, from
// ESPN's team endpoint -- free, no key, but unofficial. Each team's page is
// 7-33 KB, with the game we want ("nextEvent") near the end, so:
//  - only one team is refreshed per fetchSports() call: a team that's
//    mid-game first (once a minute), otherwise the stalest one
//  - the body is read off the stream: skip to "nextEvent", parse just that
//    array through a filter, hang up
// A team shows only while its game is live, finished within SPORTS_RECENT_S,
// or starting within SPORTS_SOON_S -- so off-season teams just drop out.
#define ESPN_URL_BASE          "https://site.api.espn.com/apis/site/v2/sports/"
#define SPORTS_REFETCH_MS      1800000        // per team, when it isn't playing
#define SPORTS_LIVE_REFETCH_MS 60000          // a team that's mid-game
#define SPORTS_RECENT_S        (36L * 3600)   // keep showing a final this long after the start
#define SPORTS_SOON_S          (72L * 3600)   // show an upcoming game this far ahead

struct Team {
  const char   *label;  // header frame
  const char   *path;   // under ESPN_URL_BASE
  const char   *abbr;   // ESPN's abbreviation for this team, to tell us from them
  // the rest is the cache, zeroed/empty as a global
  char          state;  // 'p' upcoming, 'i' in progress, 'f' final, 0 nothing
  long          start;  // UTC
  bool          home;
  String        opp, usScore, oppScore;
  String        detail; // ESPN's short status: "Bot 7th", "Q3 5:21"
  bool          won, lost;
  bool          fetched;
  unsigned long fetchedMs;
};
Team g_teams[] = {
  {"LIBERTY",  "basketball/wnba/teams/ny", "NY"},
  {"KNICKS",   "basketball/nba/teams/ny",  "NY"},
  {"METS",     "baseball/mlb/teams/nym",   "NYM"},
  {"YANKEES",  "baseball/mlb/teams/nyy",   "NYY"},
  {"NATS",     "baseball/mlb/teams/wsh",   "WSH"},
  {"COMMNDRS", "football/nfl/teams/wsh",   "WSH"},
};
#define NUM_TEAMS (sizeof(g_teams) / sizeof(g_teams[0]))

// ESPN's game times are "2026-09-27T17:05Z" -- no seconds, which
// isoToEpoch() wants.
long espnTimeToEpoch(const char *iso) {
  long t = isoToEpoch(iso);
  if (t) return t;
  int Y, Mo, D, h, m;
  if (sscanf(iso, "%d-%d-%dT%d:%d", &Y, &Mo, &D, &h, &m) < 5) return 0;
  return daysFromCivil(Y, Mo, D) * 86400L + h * 3600L + m * 60L;
}

void fetchTeam(Team &t) {
  progBegin();
  t.fetched   = true;
  t.fetchedMs = millis();

  HTTPClient http;
  http.useHTTP10(true);  // a plain body, no chunk-size lines mixed in
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(8000);
  http.begin(String(ESPN_URL_BASE) + t.path);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("SPORTS %s HTTP %d\n", t.label, code);
    http.end();
    progEnd('X');
    return;
  }

  WiFiClient &stream = http.getStream();
  if (!stream.find("\"nextEvent\":")) {
    Serial.printf("SPORTS %s: no nextEvent\n", t.label);
    http.end();
    progEnd('X');
    return;
  }

  JsonDocument filter;
  filter[0]["date"] = true;
  JsonObject fc = filter[0]["competitions"][0].to<JsonObject>();
  fc["status"]["type"]["state"]       = true;
  fc["status"]["type"]["shortDetail"] = true;
  fc["competitors"][0]["homeAway"]             = true;
  fc["competitors"][0]["winner"]               = true;
  fc["competitors"][0]["score"]                = true;
  fc["competitors"][0]["team"]["abbreviation"] = true;

  JsonDocument doc;  // parses just the nextEvent array, then stops reading
  DeserializationError err = deserializeJson(doc, stream, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("SPORTS %s JSON %s\n", t.label, err.c_str());
    progEnd('X');
    return;
  }

  t.state = 0;
  JsonObject ev = doc[0];
  if (ev.isNull()) {  // nothing scheduled -- off-season
    Serial.printf("SPORTS %s: no game\n", t.label);
    progEnd('0');
    return;
  }
  JsonObject comp  = ev["competitions"][0];
  String     state = comp["status"]["type"]["state"] | "";
  for (JsonObject c : comp["competitors"].as<JsonArray>()) {
    JsonVariant sc    = c["score"];  // {"displayValue":"6"} here, a bare string elsewhere
    String      score = sc.is<JsonObject>() ? String(sc["displayValue"] | "") : String(sc | "");
    if (String(c["team"]["abbreviation"] | "") == t.abbr) {
      t.home    = String(c["homeAway"] | "") == "home";
      t.usScore = score;
      t.won     = c["winner"] | false;
    } else {
      t.opp      = String(c["team"]["abbreviation"] | "");
      t.oppScore = score;
      t.lost     = c["winner"] | false;
    }
  }
  t.detail = String(comp["status"]["type"]["shortDetail"] | "");
  t.start  = espnTimeToEpoch(ev["date"] | "");
  t.state  = state == "pre" ? 'p' : state == "in" ? 'i' : state == "post" ? 'f' : 0;
  Serial.printf("SPORTS %s: %c %s %s-%s %s\n", t.label, t.state ? t.state : '-',
                t.opp.c_str(), t.usScore.c_str(), t.oppScore.c_str(), t.detail.c_str());
  progEnd('*');
}

// Refreshes one team: a live game first, else whichever is most overdue.
void fetchSports() {
  Team *pick = nullptr;
  for (Team &t : g_teams) {
    if (t.state == 'i' && millis() - t.fetchedMs >= SPORTS_LIVE_REFETCH_MS) { pick = &t; break; }
  }
  if (!pick) {
    for (Team &t : g_teams) {
      if (t.fetched && millis() - t.fetchedMs < SPORTS_REFETCH_MS) continue;
      if (!pick || (pick->fetched && (!t.fetched || t.fetchedMs < pick->fetchedMs))) pick = &t;
    }
  }
  if (pick) fetchTeam(*pick);
}

bool teamIsNews(const Team &t, long now) {
  if (t.state == 'i') return true;
  if (now < 1700000000L) return false;
  if (t.state == 'f') return now - t.start < SPORTS_RECENT_S;
  if (t.state == 'p') return t.start - now < SPORTS_SOON_S;
  return false;
}

bool hasSports() {
  long now = (long)time(nullptr);
  for (const Team &t : g_teams)
    if (teamIsNews(t, now)) return true;
  return false;
}

// One random team with a game worth mentioning:
//   final:    METS / L 4-6 / @ WSH
//   live:     METS / 4-6 / @ WSH / Bot 7th
//   upcoming: KNICKS / @ PHI / Mon 7-00pm
bool showSports(long nowEpoch) {
  int live[NUM_TEAMS], n = 0;
  for (size_t i = 0; i < NUM_TEAMS; i++)
    if (teamIsNews(g_teams[i], nowEpoch)) live[n++] = i;
  if (!n) return false;
  const Team &t = g_teams[live[random(n)]];

  String vs = (t.home ? "vs " : "@ ") + t.opp;
  showFrame(t.label, 1300);
  if (t.state == 'p') {
    showFrame(vs, 1500);
    time_t a = (time_t)t.start, b = (time_t)nowEpoch;
    struct tm ta, tb;
    localtime_r(&a, &ta);
    localtime_r(&b, &tb);
    long d = daysFromCivil(ta.tm_year + 1900, ta.tm_mon + 1, ta.tm_mday) -
             daysFromCivil(tb.tm_year + 1900, tb.tm_mon + 1, tb.tm_mday);
    static const char *const DOW[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    String day = d <= 0 ? "Today" : d == 1 ? "Tmrw" : DOW[ta.tm_wday];
    showFadeFrame(day + " " + hhmmAmPm(t.start), 1500);
  } else if (t.state == 'i') {
    showFrame(t.usScore + "-" + t.oppScore, 1500);
    showFrame(vs, 1300);
    if (t.detail.length()) showFadeFrame(t.detail, 1500);
  } else {
    String result = t.won ? "W " : t.lost ? "L " : "T ";
    showFrame(result + t.usScore + "-" + t.oppScore, 1800);
    showFrame(vs, 1300);
  }
  return true;
}


//  ____
// / ___|  ___  _ __   ___  ___
// \___ \ / _ \| '_ \ / _ \/ __|
//  ___) | (_) | | | | (_) \__ \
// |____/ \___/|_| |_|\___/|___/
//
// What the Sonos in this room is playing, asked of the speakers themselves
// over the LAN (UPnP SOAP on port 1400 -- no key, no cloud). Only a group's
// lead speaker knows the track, and the lead is whichever one playback was
// started from, so each fetch asks any speaker for the group layout, finds the
// group holding one of SONOS_ROOMS, then asks its lead. That first speaker is
// found by SSDP and kept until it stops answering.
//
// Spotify and Sonos Radio send artist and title. NPR One sends only the audio
// URL: a newscast just says so, and a podcast episode is looked up in NPR's
// feed for the show (the URL's `p=`) by its episode ID (`awEpisodeId=`).
const char *const SONOS_ROOMS[] = { "Living Room", "Dining Room" };
#define SONOS_REFETCH_MS   15000      // shorter than a cycle, so fresh every cycle
#define SONOS_FIND_MS      300000     // with no speaker known, search again this often
#define SONOS_SSDP_PORT    51900      // our end of the SSDP search
#define NPR_FEED_URL_BASE  "https://feeds.npr.org/"
#define NPR_FEED_MAX_BYTES 65536      // newest episodes come first; give up past this

struct NowPlaying {
  String who;   // "Bobbi Humphrey", "Up First", "NPR"
  String what;  // "The Sidewinder", the episode title, "Newscast"
  bool   valid = false;
};
NowPlaying g_sonos;
String     g_sonosIp;  // any speaker in the house; the group layout comes from it

// SSDP: multicast a search for Sonos players and take the first to answer.
String sonosFind() {
  WiFiUDP udp;
  if (!udp.begin(SONOS_SSDP_PORT)) return "";
  udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
  udp.print("M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 1\r\n"
            "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n");
  udp.endPacket();

  String ip;
  char buf[512];
  unsigned long start = millis();
  while (!ip.length() && millis() - start < 2000) {
    if (udp.parsePacket() <= 0) { delay(10); continue; }
    int n = udp.read(buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    if (strstr(buf, "Sonos")) ip = udp.remoteIP().toString();  // a Hue bridge answers every search
  }
  udp.stop();
  return ip;
}

// One SOAP call to a speaker: `service` is the UPnP service ("AVTransport"),
// `path` its control URL, `args` the action's arguments as XML.
bool sonosSoap(const String &ip, const char *path, const char *service,
               const char *action, const char *args, String &out) {
  String urn = String("urn:schemas-upnp-org:service:") + service + ":1";
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  http.begin("http://" + ip + ":1400" + path);
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION", "\"" + urn + "#" + action + "\"");
  int code = http.POST(String("<?xml version=\"1.0\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:") +
      action + " xmlns:u=\"" + urn + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>");
  if (code != HTTP_CODE_OK) {
    Serial.printf("SONOS %s HTTP %d\n", action, code);
    http.end();
    return false;
  }
  out = http.getString();
  http.end();
  return true;
}

// Sonos nests XML inside XML, escaped once per level.
String xmlUnescape(String s) {
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("&quot;", "\"");
  s.replace("&amp;", "&");  // last, so "&amp;apos;" comes out "&apos;" for asciiFold()
  return s;
}

// Value of `key` in a URL's query string, or "".
String urlParam(const String &url, const char *key) {
  String k = String(key) + "=";
  int i = url.indexOf("?" + k);
  if (i < 0) i = url.indexOf("&" + k);
  if (i < 0) return "";
  i += k.length() + 1;
  int j = url.indexOf('&', i);
  return url.substring(i, j < 0 ? url.length() : j);
}

// The lead speaker's IP for the group holding one of SONOS_ROOMS, from the
// group layout: <ZoneGroup Coordinator="RINCON_..."> around members like
// <ZoneGroupMember UUID="RINCON_..." Location="http://192.168.1.20:1400/..." ZoneName="Desk">.
String sonosLeadIp(const String &layout) {
  for (const char *room : SONOS_ROOMS) {
    int m = layout.indexOf(String("ZoneName=\"") + room + "\"");
    int g = m < 0 ? -1 : layout.lastIndexOf("<ZoneGroup ", m);
    int c = g < 0 ? -1 : layout.indexOf("Coordinator=\"", g);
    if (c < 0) continue;
    c += 13;
    String uuid = layout.substring(c, layout.indexOf('"', c));
    int u = layout.indexOf("UUID=\"" + uuid + "\"", g);
    int l = u < 0 ? -1 : layout.indexOf("Location=\"http://", u);
    if (l < 0) continue;
    l += 17;
    return layout.substring(l, layout.indexOf(':', l));
  }
  return "";
}

// Drops the reissue tags streaming services tack on: "So Danco Samba -
// 1996 Remastered", "Help! (Remastered 2009)", "Song - Mono Version".
String sonosCleanTitle(String t) {
  for (;;) {
    int cut = (t.endsWith(")") || t.endsWith("]"))
                ? max(t.lastIndexOf(" ("), t.lastIndexOf(" ["))
                : t.lastIndexOf(" - ");
    if (cut <= 0) return t;
    String tail = t.substring(cut);
    tail.toLowerCase();
    if (tail.indexOf("remaster") < 0 && tail.indexOf("version") < 0 &&
        tail.indexOf("mono") < 0 && tail.indexOf("stereo") < 0) return t;
    t = t.substring(0, cut);
  }
}

// NPR One podcast episode -> show name and episode title, from NPR's feed for
// the show. The feed runs to megabytes but is newest-first, so stream it and
// stop at the episode. Each <item>'s <title> comes before its audio URL (which
// carries the episode ID), so the last title seen when the ID turns up is the
// episode's; the feed's first title is the show's. False if the feed didn't
// load; `episode` stays empty if it loaded but the episode wasn't near the top.
bool nprEpisode(const String &showId, const String &episodeId, String &show, String &episode) {
  HTTPClient http;
  http.useHTTP10(true);  // a plain body, no chunk-size lines mixed in
  http.setUserAgent(USER_AGENT);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.begin(NPR_FEED_URL_BASE + showId + "/podcast.xml");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("NPR FEED HTTP %d\n", code);
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  String buf, title;
  show = "";
  episode = "";
  char chunk[257];
  size_t total = 0;
  unsigned long start = millis();
  while (!episode.length() && (http.connected() || stream->available()) &&
         total < NPR_FEED_MAX_BYTES && millis() - start < 8000) {
    size_t avail = stream->available();
    if (avail == 0) { delay(1); continue; }
    if (avail > sizeof(chunk) - 1) avail = sizeof(chunk) - 1;
    size_t got = stream->readBytes(chunk, avail);
    chunk[got] = '\0';
    total += got;
    buf += chunk;

    for (;;) {
      int t0 = buf.indexOf("<title>");
      int id = buf.indexOf(episodeId);
      if (id >= 0 && (t0 < 0 || id < t0)) { episode = title; break; }
      int t1 = t0 < 0 ? -1 : buf.indexOf("</title>", t0);
      if (t1 < 0) {  // no whole title buffered yet: keep enough to finish a split match
        buf.remove(0, t0 >= 0 ? t0 : max(0, (int)buf.length() - 64));
        break;
      }
      title = buf.substring(t0 + 7, t1);
      title.replace("<![CDATA[", "");
      title.replace("]]>", "");
      title = asciiFold(title);
      if (!show.length()) show = title;
      buf.remove(0, t1 + 8);
    }
  }
  http.end();

  show.replace(" from NPR", "");  // "Up First from NPR"
  Serial.printf("NPR FEED %s: %s / %s\n", showId.c_str(), show.c_str(),
                episode.length() ? episode.c_str() : "(episode not found)");
  return true;
}

// NPR One track URL -> who/what. The feed lookup is cached per episode.
void sonosNpr(const String &uri, NowPlaying &np) {
  np.who = "NPR";
  if (uri.indexOf("isNewscast=true") >= 0 || uri.indexOf("/newscasts/") >= 0) {
    np.what = "Newscast";
    return;
  }
  static String cachedEp, cachedShow, cachedTitle;
  String ep = urlParam(uri, "awEpisodeId"), showId = urlParam(uri, "p");
  if (ep.length() && showId.length() && ep != cachedEp) {
    String show, title;
    if (nprEpisode(showId, ep, show, title)) {  // a failed load retries next fetch
      cachedEp    = ep;
      cachedShow  = show;
      cachedTitle = title;
    }
  }
  if (ep.length() && ep == cachedEp) {
    if (cachedShow.length()) np.who = cachedShow;
    np.what = cachedTitle;
  }
}

void fetchSonos() {
  static RefetchTimer findTimer;
  g_sonos.valid = false;
  if (!g_sonosIp.length() && !findTimer.due(SONOS_FIND_MS)) return;

  progBegin();
  if (!g_sonosIp.length()) {
    g_sonosIp = sonosFind();
    Serial.printf("SONOS: found %s\n", g_sonosIp.length() ? g_sonosIp.c_str() : "nothing");
    if (!g_sonosIp.length()) {
      progEnd('X');
      return;
    }
  }

  String resp;
  if (!sonosSoap(g_sonosIp, "/ZoneGroupTopology/Control", "ZoneGroupTopology",
                 "GetZoneGroupState", "", resp)) {
    g_sonosIp = "";  // gone or moved -- search again
    progEnd('X');
    return;
  }
  String lead = sonosLeadIp(xmlUnescape(xmlTag(resp, "ZoneGroupState")));
  if (!lead.length()) {
    Serial.println("SONOS: room not found");
    progEnd('0');
    return;
  }

  const char *AVT = "/MediaRenderer/AVTransport/Control";
  const char *ID0 = "<InstanceID>0</InstanceID>";
  if (!sonosSoap(lead, AVT, "AVTransport", "GetTransportInfo", ID0, resp)) {
    progEnd('X');
    return;
  }
  String state = xmlTag(resp, "CurrentTransportState");
  if (state != "PLAYING") {
    Serial.printf("SONOS: %s\n", state.c_str());
    progEnd('0');
    return;
  }
  if (!sonosSoap(lead, AVT, "AVTransport", "GetPositionInfo", ID0, resp)) {
    progEnd('X');
    return;
  }

  String uri    = xmlUnescape(xmlTag(resp, "TrackURI"));
  String meta   = xmlUnescape(xmlTag(resp, "TrackMetaData"));
  String title  = asciiFold(xmlTag(meta, "dc:title"));
  String artist = asciiFold(xmlTag(meta, "dc:creator"));
  String radio  = asciiFold(xmlTag(meta, "r:streamContent"));  // "ARTIST - TITLE" on some stations
  if (title.startsWith("x-")) title = "";                       // some stations put the stream URL here
  if (radio.startsWith("ZPSTR_")) radio = "";                   // "ZPSTR_BUFFERING" and the like

  NowPlaying np;
  if (title.length() && artist.length()) {
    np.who  = artist;
    np.what = sonosCleanTitle(title);
  } else if (radio.length()) {
    int dash = radio.indexOf(" - ");
    np.who  = dash < 0 ? "" : radio.substring(0, dash);
    np.what = dash < 0 ? radio : radio.substring(dash + 3);
  } else if (uri.indexOf("NPROne=true") >= 0) {
    sonosNpr(uri, np);
  } else {
    np.what = sonosCleanTitle(title);
  }
  np.valid = np.who.length() || np.what.length();
  g_sonos = np;
  Serial.printf("SONOS: %s / %s\n", np.who.c_str(), np.what.c_str());
  progEnd(np.valid ? '*' : '0');
}

// PLAYING, then who (artist / show) and what (song / episode).
bool showSonos(long) {
  if (!g_sonos.valid) return false;
  showFrame(center8("PLAYING"), 1300);
  if (g_sonos.who.length())  showFadeFrame(g_sonos.who, 1500);
  if (g_sonos.what.length()) showFadeFrame(g_sonos.what, 2000);
  return true;
}


// __        ___ _____ _   ___         ____             __ _
// \ \      / (_)  ___(_) / _ \ ___   / ___|___  _ __  / _(_) __ _
//  \ \ /\ / /| | |_  | | | | / __| | |   / _ \| '_ \| |_| |/ _` |
//   \ V  V / | |  _| | | | |_\__ \ | |__| (_) | | | |  _| | (_| |
//    \_/\_/  |_|_|   |_|  \__/___/  \____\___/|_| |_|_| |_|\__, |
//                                                          |___/
//
// The captive-portal setup flow: an open AP + tiny web form to save WiFi
// creds to flash, used when the mode switch is in SETUP.

// HTML template for the configuration page
const char *htmlTemplate =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>uptown-f-esp32 Config</title>\n"
  "</head>\n"
  "<body>\n"
  "  <h1>Andy's uptown-f-esp32 WIFI Config</h1>\n"
  "  <p><a href=\"https://github.com/andyhomecode/ads-b-esp32\">https://github.com/andyhomecode/ads-b-esp32</a></p>\n"
  "  <h2>Configure WiFi</h2>\n"
  "  <form action=\"/save\" method=\"post\">\n"
  "    <label for=\"ssid\">SSID:</label><br>\n"
  "    <input type=\"text\" id=\"ssid\" name=\"ssid\" value=\"%s\"><br><br>\n"
  "    <label for=\"password\">Password:</label><br>\n"
  "    <input type=\"password\" id=\"password\" name=\"password\" value=\"%s\"><br><br>\n"
  "    <input type=\"submit\" value=\"Save\">\n"
  "  </form>\n"
  "</body>\n"
  "</html>\n";

bool connectToWiFi(const char *ssid, const char *password) {
  WiFi.begin(ssid, password);
  Serial.printf("Connecting to WiFi: %s\n", ssid);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected to %s\n", ssid);
    Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());

    char tempOut[20];
    sprintf(tempOut, "IP %s", WiFi.localIP().toString().c_str());
    showText(tempOut);
    return true;
  } else {
    Serial.println("\nFailed to connect.");

    char tempOut[20];
    for (int i = 0; i < 3; i++)
      showText("Wi-Fi Failed to Connect");

    return false;
  }
}

void startAccessPoint() {
  const char *apSSID = "SUBWAY-ESP32";
  const char *apPassword = "";  // no password,
                                //the Wifi AP is only on when the switch is in SETUP,
                                // and with Arduino's Harvard architecture there's very little attack surface for overflows or other such shenanigans

  if (g_wifiConnected) {
    // if we're already connected to wifi for some reason, restart so we can start the AP.
    ESP.restart();
  }

  showText("Connect to SUBWAY-ESP32...");

  WiFi.softAP(apSSID, apPassword);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP started. IP: %s\n", IP.toString().c_str());

  char tempOut[20];
  sprintf(tempOut, "IP %s", IP.toString());

  for (int i = 0; i < 3; i++)
    showText(tempOut);

  dnsServer.start(53, "*", IP);


  // load stored settings or defaults to prefill form.
  const String ssid = preferences.getString("ssid", "");
  const String password = preferences.getString("password", "");


  Serial.printf("ssid: %s\n", ssid.c_str());


  // Use snprintf to insert variables dynamically
  snprintf(htmlPage, sizeof(htmlPage), htmlTemplate, ssid, password);


  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", htmlPage);
  });


  // preferences saves wifi creds to non-volatile memory on the ESP32
  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      preferences.putString("ssid", server.arg("ssid"));
      preferences.putString("password", server.arg("password"));

      server.send(200, "text/html", "<h1>Credentials Saved.</h1><p>Disconnect from setup Wi-Fi and flip switch to RUN. And catch some trains!</p>");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/html", "<h1>Invalid Input</h1>");
    }
  });

  server.begin();

  // begin endless loop of waiting for requests and handling them.
  while (true) {

    // did someone flip the switch to Run from Setup?
    if (digitalRead(SWITCH_PIN) == HIGH) {
      // let them know and reboot.
      showText("-REBOOT-");
      delay(300);
      ESP.restart();
    }

    showText("-Setup-");
    dnsServer.processNextRequest();
    server.handleClient();
  }
}


//  ____       _
// / ___|  ___| |_ _   _ _ __
// \___ \ / _ \ __| | | | '_ \
//  ___) |  __/ |_| |_| | |_) |
// |____/ \___|\__|\__,_| .__/
//                      |_|


void setup() {
  Serial.begin(9600);
  Serial.println("Board started");
  preferences.begin("wifi-creds", false);

  initLookups();  // airline + aircraft-type code tables for the ADS-B block

  pinMode(SWITCH_PIN, INPUT_PULLUP);  // enable the setup vs run switch

  Serial.print("in Setup\n");

  // setup the LED displays

  Wire.begin(SDA, SCL);  // SDA pin 9 and one in on LCD board, SLC pin 18 and rightmost on LCD board

  // setup the LED displays by device number
  alpha4_0.begin(0x70);  // first one
  alpha4_1.begin(0x71);  // 2nd one

  alpha4_0.clear();
  alpha4_1.clear();
  setBrightnessBoth(BRIGHT_FULL);

  // title screen
  showText("github.com/andyhomecode/ads-b-esp32");
  showText("Andy's Bullshit Display");
  showText(" V 7.9");

  // get the stored Wifi credentials
  String ssid = preferences.getString("ssid", DEFAULT_SSID);
  String password = preferences.getString("password", DEFAULT_PASSWORD);


  // If Setup switch is in RUN, try to connect to WiFi using stored creds
  if (digitalRead(SWITCH_PIN) == HIGH && connectToWiFi(ssid.c_str(), password.c_str())) {
    g_wifiConnected = true;

    // Kick off NTP so we can turn arrival timestamps into a countdown.
    // Work in UTC (offset 0); the feed's timestamps carry their own offset.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    for (int i = 0; i < 20 && time(nullptr) < 1700000000L; i++) {
      delay(250);
    }
    Serial.printf("NTP epoch: %ld\n", (long)time(nullptr));

    // localtime_r() below (sunrise/sunset/tide display formatting) needs a
    // real NY zone, DST included -- time(nullptr) itself is unaffected by TZ,
    // so this doesn't touch anything upstream that already assumes UTC.
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
  } else {
    g_wifiConnected = false;
  }
}


// Everything that isn't an urgent alert or the plane. Each cycle picks
// RANDOM_FEEDS_PER_CYCLE of these up front and runs only their fetches, so a
// pass never waits on an API call for a feed it isn't about to show.
//
// `weight` is each feed's odds of being picked, relative to the others -- the
// knob to turn when something shows up too often or not enough (0 = never).
// `has` says whether the feed has anything to draw once fetched (ISS, the
// hurricane, airport delays, high UV and the like are usually empty), so the
// picker moves on to another. fetch is nullptr for feeds computed locally;
// refetchMs 0 means the fetch gates itself.
#define RANDOM_FEEDS_PER_CYCLE 3
typedef bool (*FeedShow)(long nowEpoch);
struct Feed {
  int           weight;
  const char   *name;   // for the serial log
  FeedShow      show;
  bool          (*has)();
  void          (*fetch)();
  unsigned long refetchMs;
};

bool hasHoroscope() {
  for (size_t i = 0; i < NUM_HOROSCOPES; i++) if (g_horoscopes[i].valid) return true;
  return false;
}

const Feed FEEDS[] = {
// weight name          show            has                                        fetch            refetchMs
  { 8,  "trains",      showArrivals,   [] { return g_haveTrainData; },            fetchTrains,     0 },
  { 6,  "buses",       showBuses,      hasBuses,                                  fetchBuses,      BUS_REFETCH_MS },
  { 4,  "citibike",    showCitibike,   [] { return g_citibikeBikes >= 0; },       fetchCitibike,   CITIBIKE_REFETCH_MS },
  { 6,  "wx now",      showWxNow,      [] { return g_wxNow.valid; },              fetchWxNow,      WXNOW_REFETCH_MS },
  { 5,  "forecast",    showWxForecast, [] { return g_haveWxForecast; },           fetchWxForecast, WXFC_REFETCH_MS },
  { 2,  "air",         showAir,        [] { return g_air.aqi >= 0; },             fetchAir,        0 },
  { 2,  "uv",          showUv,         [] { return g_air.uv >= UV_SHOW_MIN; },    fetchAir,        0 },
  { 6,  "airports",    showAirports,   hasAirportDelays,                          fetchAirports,   FAA_REFETCH_MS },
  { 6,  "sports",      showSports,     hasSports,                                 fetchSports,     0 },
  { 3,  "news",        showNews,       [] { return g_newsCount > 0; },            fetchNews,       NEWS_REFETCH_MS },
  { 4,  "stocks",      showStocks,     [] { return g_spx.valid; },                fetchStocks,     QUOTE_REFETCH_MS },
  { 3,  "yen",         showYen,        [] { return g_yen.valid; },                fetchYen,        QUOTE_REFETCH_MS },
  { 1,  "bitcoin",     showBitcoin,    [] { return g_btc.valid; },                fetchBitcoin,    QUOTE_REFETCH_MS },
  { 3,  "tokyo",       showTokyo,      [] { return g_tokyo.valid; },              fetchTokyo,      TOKYO_WX_REFETCH_MS },
  { 2,  "jp holiday",  showJpHoliday,  [] { return g_jpHoliday.valid; },          fetchJpHoliday,  HOLIDAY_REFETCH_MS },
  { 2,  "holiday",     showHoliday,    [] { return g_holiday.valid; },            fetchHoliday,    HOLIDAY_REFETCH_MS },
  { 3,  "sun",         showSunTimes,   [] { return g_sun.valid; },                fetchSunTimes,   SUN_REFETCH_MS },
  { 2,  "daylight",    showDaylight,   [] { return g_dayLen[0] > 0 && g_dayLen[1] > 0; }, fetchDaylight, DAYLEN_REFETCH_MS },
  { 1,  "seasons",     showSeasons,    [] { return true; },                       nullptr,         0 },
  { 3,  "moon",        showMoonPhase,  [] { return true; },                       nullptr,         0 },
  { 3,  "tide",        showTide,       [] { return g_tideCount > 0; },            fetchTide,       TIDE_REFETCH_MS },
  { 2,  "water",       showWater,      [] { return g_waterF > -100; },            fetchWater,      WATER_REFETCH_MS },
  { 6,  "iss",         showIss,        [] { return g_issOverhead; },              fetchIss,        ISS_REFETCH_MS },
  { 5,  "hurricane",   showHurricane,  [] { return g_hurricane.valid; },          fetchHurricane,  NHC_REFETCH_MS },
  { 3,  "launch",      showLaunch,     hasLaunch,                                 fetchLaunch,     LAUNCH_REFETCH_MS },
  { 2,  "asteroid",    showAsteroid,   [] { return g_rock.valid; },               fetchAsteroid,   CAD_REFETCH_MS },
  { 1,  "iceberg",     showIceberg,    [] { return g_iceberg.valid; },            fetchIceberg,    EONET_REFETCH_MS },
  { 1,  "co2",         showCo2,        [] { return g_co2 > 0; },                  fetchCo2,        CO2_REFETCH_MS },
  { 1,  "word",        showWord,       [] { return g_word.valid; },               fetchWord,       WOTD_REFETCH_MS },
  { 2,  "horoscope",   showHoroscope,  hasHoroscope,                              fetchHoroscopes, HOROSCOPE_REFETCH_MS },
  { 1,  "magic 8",     showMagic8,     [] { return true; },                       nullptr,         0 },
};
#define NUM_FEEDS (sizeof(FEEDS) / sizeof(FEEDS[0]))

// Draws feeds at random, weighted by FEEDS[].weight and without repeats,
// fetching each candidate if it's due, until RANDOM_FEEDS_PER_CYCLE of them
// have data. Fills `out` with their indices and returns how many it found.
int pickFeeds(size_t out[RANDOM_FEEDS_PER_CYCLE]) {
  static RefetchTimer timers[NUM_FEEDS];
  bool tried[NUM_FEEDS] = {false};
  int picked = 0;
  while (picked < RANDOM_FEEDS_PER_CYCLE) {
    long total = 0;
    for (size_t i = 0; i < NUM_FEEDS; i++) if (!tried[i]) total += FEEDS[i].weight;
    if (total <= 0) break;  // ran out of candidates

    long r = random(total);
    size_t i = 0;
    for (;; i++) {
      if (tried[i]) continue;
      if (r < FEEDS[i].weight) break;
      r -= FEEDS[i].weight;
    }
    tried[i] = true;

    const Feed &f = FEEDS[i];
    if (f.fetch && timers[i].due(f.refetchMs)) f.fetch();
    if (f.has()) {
      out[picked++] = i;
      Serial.printf("pick: %s\n", f.name);
    }
  }
  return picked;
}

// **********************************************************
//  _                   
// | | ___   ___  _ __  
// | |/ _ \ / _ \| '_ \ 
// | | (_) | (_) | |_) |
// |_|\___/ \___/| .__/ 
//                |_|    
// **********************************************************

void loop() {

  Serial.println("Start of loop");


  // check to see if the mode switch is set to SETUP
  if (digitalRead(SWITCH_PIN) == LOW) {
    // we're in setup mode

    // so show the web server and handle it.
    showText("*Setup*");
    startAccessPoint();  // we're not coming back from there.  It starts the wifi access point and web server.
  } else {


    if (WiFi.status() != WL_CONNECTED) {
      // ruh roh.  Not connected to wi-fi.
      showText("No Wi-fi");
      ESP.restart();  // maybe better luck next time?
    }

    if (g_wifiConnected) {

      //        _______________
      //   _____|_[]_[]_[]_[]__|__
      //  |_ East Broadway  <-> F  |
      //  |_o_______________o______|
      //     O-O               O-O

      // The plan, each pass:
      // - pick this cycle's random feeds (see pickFeeds())
      // - fetch only what's about to be shown -- alerts, the plane, the Sonos and those
      //   picks -- each still gated by its own refetch interval
      // - draw everything from the caches

      progReset();  // start a fresh loading clock for whatever fetches fire below

      // Alerts, the plane and the Sonos show every cycle, so they always fetch
      // (each on its own clock); the random feeds fetch only when picked below.
      // --- NWS: refresh the weather alert on its own (slow) clock ---------
      static RefetchTimer wxTimer;
      if (wxTimer.due(WX_REFETCH_MS)) fetchWeatherAlert();

      // --- NYC OEM: refresh the emergency alert on its own (slow) clock ---
      static RefetchTimer oemTimer;
      if (oemTimer.due(OEM_REFETCH_MS)) fetchOemAlert();

      // --- USGS: check for a big Tokyo quake -----------------------------
      static RefetchTimer eqTimer;
      if (eqTimer.due(EQ_REFETCH_MS)) fetchQuake();

      // --- adsb.lol: any plane near NYC squawking an emergency -----------
      static RefetchTimer sqkTimer;
      if (sqkTimer.due(SQK_REFETCH_MS)) fetchSquawks();

      // --- SWPC: geomagnetic storm strong enough for an aurora here -------
      static RefetchTimer kpTimer;
      if (kpTimer.due(KP_REFETCH_MS)) fetchKp();

      // --- Sonos: what's playing, over the LAN ----------------------------
      static RefetchTimer sonosTimer;
      if (sonosTimer.due(SONOS_REFETCH_MS)) fetchSonos();

      // --- ADS-B: refresh the plane cache on its own (faster) clock ---------
      // Same decoupled pattern as the trains: poll here, draw from the cache.
      // Anything going wrong just clears the plane; the countdown is unaffected.
      static RefetchTimer planeTimer;
      if (planeTimer.due(ADSB_REFETCH_MS)) {
        Plane p;
        if (fetchNorthernmostPlane(p)) {
          // still the same flight? keep what we already resolved for it --
          // checked by callsign, not g_plane.valid, so a callsign we already
          // confirmedNotLGA last cycle (and so hid) doesn't get re-queried
          // against routeset every 20s for as long as it lingers in the box.
          if (g_plane.callsign == p.callsign && p.callsign.length()) {
            p.airline         = g_plane.airline;
            p.origin          = g_plane.origin;
            p.routeChecked    = g_plane.routeChecked;
            p.confirmedNotLGA = g_plane.confirmedNotLGA;
          }
          g_plane = p;
          if (!g_plane.routeChecked) lookupRoute(g_plane);
          if (g_plane.confirmedNotLGA) g_plane.valid = false;  // e.g. actually JFK-bound
        } else {
          g_plane.valid = false;  // nobody on final in the bounding area
        }
      }

      // --- This cycle's random feeds, fetched only if due ------------------
      size_t picks[RANDOM_FEEDS_PER_CYCLE];
      int numPicks = pickFeeds(picks);

      progFinish();  // scramble the loading clock away

      // current time: NTP if we have it, else the fetch clock plus elapsed
      long nowEpoch = (long)time(nullptr);
      if (nowEpoch < 1700000000L) {
        nowEpoch = g_trainFetchEpoch + (long)((millis() - g_lastTrainFetchMs) / 1000);
      }

      // Every cycle: urgent alerts first (NWS, NYC OEM, Tokyo quake, squawks,
      // aurora) if present...

      // weather alert, source tag then just the title. Header is centered
      // ("-=NWS=-") rather than left-justified like the other source tags.
      showAlert(center8("-=NWS=-").c_str(), g_wxAlert.event);

      // NYC OEM alert (only ever set when capIsHighUrgency() said yes, and
      // only from the English-language copy -- see oemIsEnglish()). Show
      // headline, not event: OEM's CAP event is always the generic SAME code
      // name "Civil Emergency Message" regardless of what's actually going
      // on, unlike NWS where event is already specific ("Flood Watch" etc).
      // headline is where OEM puts the actual "what" (e.g. "Basement
      // Preparedness - 9/13", already stripped of its "Notify NYC - ...
      // (NYC)" wrapper by oemCleanHeadline()). Fall back to event only if
      // headline is somehow empty (malformed CAP doc). Source tag is "OEM"
      // plus the CAP <category> (e.g. "OEM GEO") rather than a bare "NYC
      // OEM", since category is now the thing that decides whether an OEM
      // alert qualifies at all (see capIsHighUrgency()) -- showing it lets
      // you tell at a glance which bucket tripped the filter.
      String oemSource = g_oemAlert.category.length()
                            ? "OEM " + g_oemAlert.category
                            : String("OEM");
      oemSource.toUpperCase();
      if (oemShouldShow(nowEpoch)) {  // aged out / halved -- see oemShouldShow()
        showAlert(oemSource.c_str(), g_oemAlert.headline.length() ? g_oemAlert.headline : g_oemAlert.event);
        String age = oemAgeText(nowEpoch);
        if (age.length()) showFadeFrame(center8(age), 1500);
      }

      // ...a notable Tokyo earthquake in the last 24h -- like the weather
      // alert, but the tag shakes instead of blinking.
      if (g_quakeLine.length()) {
        setBrightnessBoth(BRIGHT_FULL);
        shakeText(g_quakeTag);
        showFadeFrame(g_quakeLine, 2500);
      }

      // ...a plane near NYC squawking 7500/7700/7600...
      showAlert(g_squawkTag.c_str(), g_squawkLine);

      // ...a geomagnetic storm strong enough to push the aurora this far south...
      if (g_kp >= AURORA_KP) {
        char line[64];
        snprintf(line, sizeof(line), "Kp %.1f - northern lights possible, look north", g_kp);
        showAlert(center8("AURORA").c_str(), line);
      }

      // ...then the plane on final, if there is one...
      if (g_plane.valid) showPlane(g_plane);

      // ...then what the Sonos is playing, if anything...
      showSonos(nowEpoch);

      // ...then the random feeds picked above.
      for (int i = 0; i < numPicks; i++) FEEDS[picks[i]].show(nowEpoch);
    } else {
      Serial.println("Not connected to Wi-Fi.");
      showText("No Wi-fi");
      ESP.restart();
      //
      //   .------------------------.
      //  ( Is this the uptown side? )
      //   `-----------.  ,---------'
      //     ___________\ |____________
      //   _|___________________________|_
      //  |  __   __   __   __   __   __  |
      //  |_|__|_|__|_|__|_|__|_|__|_|__|_|
      //  |_____________________________ _|
      //     (O)                   (O)
      //
    }
  }
}
