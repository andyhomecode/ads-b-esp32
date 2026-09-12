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
// Weather -> Quake -> WiFi & config portal -> Setup & loop. Each feed section
// carries a fetchX() (pulls data into a cache) and a showX() (renders the
// cache); the Display section is the only place that touches the LED
// hardware directly.

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <time.h>
#include <string.h>  // strchr, for isoToEpoch

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
#define USER_AGENT      "ads-b-esp32/5.0 (+https://github.com/andyhomecode/ads-b-esp32)"

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
#define OEM_MAX_ITEMS   5             // only the most recent few are worth checking

// --- USGS earthquakes near Tokyo ---------------------------------------------
// The 5 most recent M>=4 within 300 km of Tokyo. We only *show* one if it's
// mag >= EQ_MIN_MAG or tsunami-flagged, AND it happened in the last EQ_MAX_AGE_S
// seconds -- needs an NTP clock for that window.
#define EQ_URL          "https://earthquake.usgs.gov/fdsnws/event/1/query?format=geojson&latitude=35.68&longitude=139.77&maxradiuskm=300&minmagnitude=4&orderby=time&limit=5"
#define EQ_MIN_MAG      4.3f
#define EQ_MAX_AGE_S    86400         // 24 h
#define EQ_REFETCH_MS   600000        // 10 min

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
#define BRIGHT_MIN   0   // dimmest still-lit level -- the progress bar

// Two custom 14-segment glyphs for the train direction, carried through the
// string/scroll pipeline as sentinel bytes: wherever one lands in a frame,
// writeRawFrame() renders it raw instead of as ASCII.
//   downtown -- a down arrowhead "\|/" in the top half   (H + J + K)
//   uptown   -- an up arrowhead   "/|\" in the bottom half (N + M + L)
#define GLYPH_DOWN   (ALPHANUM_SEG_H | ALPHANUM_SEG_J | ALPHANUM_SEG_K)
#define GLYPH_UP     (ALPHANUM_SEG_N | ALPHANUM_SEG_M | ALPHANUM_SEG_L)
#define GLYPH_DOWN_CH '\x01'
#define GLYPH_UP_CH   '\x02'


void writeRawFrame(String text, int dPLocation = -1) {

  // add spaces to the end so we don't get null
  // yes, I know this is a terrible hack, and it shouldn't happen,
  text += "        ";

  // Clear both displays
  alpha4_0.clear();
  alpha4_1.clear();


  // Write to Display 1
  // you can only set one character at one position at a time
  // and there's 4 characters per display
  // so go through the first 4 characters of the text, put them in the spots
  // and if you're on the character where the decimal point is, turn on the bool
  // It's weird, but that's because there's no ASCII modifier meaning "number or letter with a decimal point"
  for (int i = 0; i <= 3; i++) {
    char c = text.charAt(i);
    if (c == GLYPH_DOWN_CH || c == GLYPH_UP_CH)
      alpha4_0.writeDigitRaw(i, c == GLYPH_DOWN_CH ? GLYPH_DOWN : GLYPH_UP);
    else
      alpha4_0.writeDigitAscii(i, c, i == dPLocation);  // Write each character to the display, if it's the character with the decimal point, show it
  }

  // Write to Display 2
  for (int i = 0; i <= 3; i++) {
    char c = text.charAt(i + 4);                            // remember we're showing the next 4 digits
    if (c == GLYPH_DOWN_CH || c == GLYPH_UP_CH)
      alpha4_1.writeDigitRaw(i, c == GLYPH_DOWN_CH ? GLYPH_DOWN : GLYPH_UP);
    else
      alpha4_1.writeDigitAscii(i, c, (i == dPLocation - 4));  // Write each character to the display, ditto for the decimal point
  }

  // Update both displays
  alpha4_0.writeDisplay();
  alpha4_1.writeDisplay();
}


// What's currently parked on the 8 columns, so the next frame can scroll the
// old data out to the left while the new data scrolls in from the right.
String g_frame = "        ";

// Actual current backpack brightness, so fades start from where we really are
// (e.g. the dim progress bar) instead of snapping to full first.
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

// Fade down to dim, scroll the current frame out to the left while `next` slides
// in from the right (all at low brightness), then fade back up to full and hold
// for holdMs.
void showFrame(String next, int holdMs, int stepMs = 45) {
  while (next.length() < 8) next += " ";
  next = next.substring(0, 8);

  fadeBrightnessBoth(BRIGHT_DIM, 8);

  String buf = g_frame + next;  // 16 columns: old data | new data
  for (int i = 1; i <= 8; i++) {
    writeRawFrame(buf.substring(i, i + 8), -1);
    delay(stepMs);
  }
  g_frame = next;

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
  String severity;
  String urgency;
  String certainty;
};

// Fails OPEN: excludes only known-low values, rather than requiring known-high
// ones. A real active alert with blank/"Unknown" severity or urgency (a CAP
// producer under time pressure not filling every field -- seen for real in
// NWS's own active-alerts list, which currently has a "Test Message" entry
// with severity=Unknown, urgency=Unknown right alongside genuine warnings)
// must still get through; silently dropping a real emergency because a field
// was left blank is worse than showing one that turns out to be routine.
bool capIsHighUrgency(const CapAlert &a) {
  bool severityLow = (a.severity == "Minor" || a.severity == "Moderate");
  bool urgencyLow  = (a.urgency  == "Future" || a.urgency  == "Past");
  return !severityLow && !urgencyLow;
}

// Shows a CAP alert as a blinking source tag ("NWS", "NYC OEM") followed by
// the event name, both flashing to catch the eye -- one shared treatment for
// every CAP-based feed instead of each one rolling its own.
void showAlert(const char *source, const String &event) {
  if (!event.length()) return;
  setBrightnessBoth(BRIGHT_FULL);
  blink(true);
  showText(source, -1, 1200);
  showText(event, -1, 2500);
  blink(false);
  g_frame = "        ";
}


//  progress bar ----------------------------------------------------------
//  Fetches block loop(), so while a fetch cycle runs the display becomes a
//  dim left-to-right bar: one column per HTTP call. '-' (with its decimal
//  point lit = "working") the moment a call starts; when it returns the
//  '-' morphs, segment by segment, into the result -- '*' got data (dash
//  blooms into a star), '0' call ok but nothing (a ring closes around the
//  dash, then the dash dissolves), 'X' error (the dash tips over into an
//  X) -- and the decimal point goes dark. progReset() at the top of the
//  fetch section each pass; progBegin()/progEnd() wrap each call.

// 14-seg building blocks (segment names per Adafruit_LEDBackpack.h).
#define SEG_MID    (ALPHANUM_SEG_G1 | ALPHANUM_SEG_G2)   // the dash
#define SEG_VERT   (ALPHANUM_SEG_J  | ALPHANUM_SEG_M)    // center vertical |
#define SEG_SLASH  (ALPHANUM_SEG_K  | ALPHANUM_SEG_L)    // /
#define SEG_BSLASH (ALPHANUM_SEG_H  | ALPHANUM_SEG_N)    // backslash
#define SEG_RING   (ALPHANUM_SEG_A | ALPHANUM_SEG_B | ALPHANUM_SEG_C | \
                    ALPHANUM_SEG_D | ALPHANUM_SEG_E | ALPHANUM_SEG_F)

// Morph frames, last one == the plain-ASCII glyph so the settle is seamless.
static const uint16_t MORPH_STAR[] = {          // '-' -> '*'
  SEG_MID,
  SEG_MID | SEG_VERT,                           // "+"
  SEG_MID | SEG_VERT | SEG_SLASH,
  SEG_MID | SEG_VERT | SEG_SLASH | SEG_BSLASH,  // 0x3FC0 == '*'
};
static const uint16_t MORPH_X[] = {             // '-' -> 'X'
  SEG_MID,
  SEG_BSLASH,                                   // dash tipped to "\"
  SEG_BSLASH | SEG_SLASH,                       // 0x2D00 == 'X'
};
static const uint16_t MORPH_ZERO[] = {          // '-' -> '0'
  SEG_MID,
  SEG_MID | ALPHANUM_SEG_A | ALPHANUM_SEG_D,    // dash + top & bottom rails
  SEG_MID | SEG_RING,                           // ring closed around the dash
  SEG_RING | SEG_SLASH,                         // dash gone -> 0x0C3F == '0'
};

static char g_prog[9] = "        ";
static int  g_progN   = 0;      // next column

void progReset() {
  memset(g_prog, ' ', 8);
  g_prog[8] = '\0';
  g_progN = 0;
}

void progBegin() {
  if (g_progN >= 8) return;
  g_prog[g_progN] = '-';
  setBrightnessBoth(BRIGHT_MIN);
  writeRawFrame(g_prog, g_progN);  // dp lit = "working"
}

void progEnd(char result) {          // '*' data | '0' none | 'X' error
  if (g_progN >= 8) return;

  const uint16_t *frames = nullptr;
  int n = 0;
  switch (result) {
    case '*': frames = MORPH_STAR; n = sizeof(MORPH_STAR) / sizeof(MORPH_STAR[0]); break;
    case '0': frames = MORPH_ZERO; n = sizeof(MORPH_ZERO) / sizeof(MORPH_ZERO[0]); break;
    case 'X': frames = MORPH_X;    n = sizeof(MORPH_X)    / sizeof(MORPH_X[0]);    break;
  }

  // Animate just the active column; the rest of the bar stays put.
  Adafruit_AlphaNum4 &disp = (g_progN < 4) ? alpha4_0 : alpha4_1;
  uint8_t pos = g_progN & 3;
  for (int f = 0; f < n; f++) {
    disp.writeDigitRaw(pos, frames[f]);
    disp.writeDisplay();
    delay(70);
  }

  // Settle: clean re-render with the real glyph, decimal point off.
  g_prog[g_progN] = result;
  writeRawFrame(g_prog, -1);
  g_progN++;
}

bool progRan() { return g_progN > 0; }  // did this pass hit the network?


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
// feeds' RefetchTimer-gated fetches, this keeps retrying every pass (not
// just every REFETCH_MS) until the first parse succeeds -- same cadence as
// the original inline loop() code, since a stale/empty countdown is worse
// than an extra request.
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
void showArrivals(long nowEpoch) {
  showFrame("E B'WAY", 500);

  if (g_trainCount == 0) {
    showFrame("NO F TRN", 1400);
    return;
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

// Resolve airline + origin for p. Airline name comes from the local table
// (short, offline). Origin comes from adsb.lol's routeset: POST the callsign
// plus the plane's current position, get back `_airports` and `plausible`.
// Since this plane is on final into LGA, LGA is the last airport in the route
// -- the origin is the one right before it. If the route has no LGA, or isn't
// plausible, or the lookup errors, we show no origin (better than a wrong one)
// and, on a transport error, leave routeChecked false so the next pass retries.
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
  } else {
    Serial.printf("route HTTP %d (will retry)\n", code);
  }
  http.end();
  progEnd(pc);
}

// Like showFrame(), but for plane details that can run longer than 8 columns
// (airline names, "FROM ..." origins): fade down, slide the first 8 columns
// in, fade back up and hold -- then, if there's more text, keep marqueeing it
// across at full brightness before the next field fades down in turn.
void showPlaneFrame(String text, int holdMs = 2000, int stepMs = 45) {
  String first = text.substring(0, 8);
  while (first.length() < 8) first += " ";

  fadeBrightnessBoth(BRIGHT_DIM, 8);

  String buf = g_frame + first;
  for (int i = 1; i <= 8; i++) {
    writeRawFrame(buf.substring(i, i + 8), -1);
    delay(stepMs);
  }
  g_frame = first;

  fadeBrightnessBoth(BRIGHT_FULL, 14);
  delay(holdMs);

  if (text.length() > 8) {
    for (int i = 1; i <= text.length() - 8; i++) {
      String frame = text.substring(i, i + 8);
      writeRawFrame(frame, -1);
      delay(200);
    }
    g_frame = text.substring(text.length() - 8);
    delay(1000);
  }
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
  showPlaneFrame(flight, 3000);

  showPlaneFrame(p.airline.length() ? p.airline : "Unknown");

  if (icacoLookup.count(p.typeCode))
    showPlaneFrame(icacoLookup[p.typeCode]);
  else if (p.typeCode.length())
    showPlaneFrame(p.typeCode);

  if (p.altFt > 0) {
    char alt[16];
    snprintf(alt, sizeof(alt), "%ld FT", p.altFt);
    showPlaneFrame(alt);
  }

  if (p.origin.length())
    showPlaneFrame("FROM " + p.origin);

  // show the flight one last time before fading out, so the user can read it
  showPlaneFrame(flight, 3000);
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

// For each stop with buses, run through the upcoming arrivals, each one its own
// frame carrying the route tag and the countdown: "M14 12mn" / "M9 12min" /
// "M14 NOW". "min" is trimmed to "mn" when the whole thing would overflow the 8
// columns (M14 + two-digit minutes). SIRI hands them back soonest-first already.
void showBuses(long nowEpoch) {
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

  // Walk each <item>...</item> newest-first, following its CAP doc link to
  // check severity/urgency -- the RSS item itself doesn't carry them. Log
  // every item checked (not just a match) so the serial monitor can tell
  // "feed's quiet" apart from "feed's format changed and this is silently
  // finding nothing" -- the two look identical otherwise.
  int pos = 0;
  int checked = 0;
  for (; checked < OEM_MAX_ITEMS; checked++) {
    int itemStart = rss.indexOf("<item>", pos);
    if (itemStart < 0) break;
    int itemEnd = rss.indexOf("</item>", itemStart);
    if (itemEnd < 0) break;
    String item = rss.substring(itemStart, itemEnd);
    pos = itemEnd + 7;

    String title  = xmlTag(item, "title");
    String capUrl = oemCapDocUrl(item);
    if (!capUrl.length()) {
      Serial.printf("OEM item %d: \"%s\" -- no CAP doc URL found (enclosure/link)\n",
                    checked, title.c_str());
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
                    checked, title.c_str(), capCode);
      capHttp.end();
      continue;
    }
    String cap = capHttp.getString();
    capHttp.end();

    CapAlert a;
    a.event     = xmlTag(cap, "event");
    a.severity  = xmlTag(cap, "severity");
    a.urgency   = xmlTag(cap, "urgency");
    a.certainty = xmlTag(cap, "certainty");
    bool match = capIsHighUrgency(a);

    Serial.printf("OEM item %d: event=\"%s\" severity=\"%s\" urgency=\"%s\" certainty=\"%s\" %s\n",
                  checked, a.event.c_str(), a.severity.c_str(), a.urgency.c_str(),
                  a.certainty.c_str(), match ? "-> SHOWING" : "(filtered out)");

    if (match) {
      g_oemAlert = a;
      break;  // feed is newest-first, so the first qualifying item wins
    }
  }
  if (checked == 0) Serial.println("OEM: 0 items in RSS feed");

  Serial.printf("OEM: %s\n", g_oemAlert.event.length() ? g_oemAlert.event.c_str() : "(none)");
  progEnd(g_oemAlert.event.length() ? '*' : '0');
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
// in the last EQ_MAX_AGE_S seconds. g_quakeLine is one ready-to-scroll string
// (or "" when nothing) -- shown exactly like a weather alert.

String g_quakeLine;  // "TOKYO EQ M4.7 74 KM E OF TOMIOKA, JAPAN"; "" when quiet

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

    String place = String(pr["place"] | "");
    place.toUpperCase();
    g_quakeLine  = tsu ? "TSUNAMI " : "TOKYO EQ ";
    g_quakeLine += "M" + String(mag, 1);
    if (place.length()) g_quakeLine += " " + place;
    break;
  }
  Serial.printf("EQ: %s\n", g_quakeLine.length() ? g_quakeLine.c_str() : "(none)");
  progEnd(g_quakeLine.length() ? '*' : '0');
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
  showText("FTRAIN +");
  showText("PLANES");
  showText(" V 5.0");

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
  } else {
    g_wifiConnected = false;
  }
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

      // The plan:
      // - every REFETCH_MS, hit the JSON proxy and cache the next few arrival
      //   times as absolute epochs
      // - every pass through loop(), redraw the countdown from that cache so the
      //   minutes tick down without hammering the server

      progReset();  // start a fresh progress bar for whatever fetches fire below
      fetchTrains();

      // --- ADS-B: refresh the plane cache on its own (faster) clock ---------
      // Same decoupled pattern as the trains: poll here, draw from the cache.
      // Anything going wrong just clears the plane; the countdown is unaffected.
      static RefetchTimer planeTimer;
      if (planeTimer.due(ADSB_REFETCH_MS)) {
        Plane p;
        if (fetchNorthernmostPlane(p)) {
          // still the same flight? keep the airline/origin we already resolved
          if (g_plane.valid && g_plane.callsign == p.callsign) {
            p.airline      = g_plane.airline;
            p.origin       = g_plane.origin;
            p.routeChecked = g_plane.routeChecked;
          }
          g_plane = p;
          if (!g_plane.routeChecked) lookupRoute(g_plane);
        } else {
          g_plane.valid = false;  // nobody on final in the bounding area
        }
      }

      // --- MTA BusTime: refresh the bus list on its own clock -------------
      static RefetchTimer busTimer;
      if (busTimer.due(BUS_REFETCH_MS)) fetchBuses();

      // --- NWS: refresh the weather alert on its own (slow) clock ---------
      static RefetchTimer wxTimer;
      if (wxTimer.due(WX_REFETCH_MS)) fetchWeatherAlert();

      // --- NYC OEM: refresh the emergency alert on its own (slow) clock ---
      static RefetchTimer oemTimer;
      if (oemTimer.due(OEM_REFETCH_MS)) fetchOemAlert();

      // --- USGS: check for a big Tokyo quake -----------------------------
      static RefetchTimer eqTimer;
      if (eqTimer.due(EQ_REFETCH_MS)) fetchQuake();

      // If we hit the network this pass, hold the finished bar a beat, then let
      // the first real frame scroll it away.
      if (progRan()) {
        delay(350);
        g_frame = String(g_prog);
      }

      // current time: NTP if we have it, else the fetch clock plus elapsed
      long nowEpoch = (long)time(nullptr);
      if (nowEpoch < 1700000000L) {
        nowEpoch = g_trainFetchEpoch + (long)((millis() - g_lastTrainFetchMs) / 1000);
      }

      // Always show Weather alerts, NYC OEM alerts, and Tokyo quakes if present

      // weather alert, source tag then just the title
      showAlert("NWS", g_wxAlert.event);

      // NYC OEM alert (only ever set when capIsHighUrgency() said yes)
      showAlert("NYC OEM", g_oemAlert.event);

      // ...a notable Tokyo earthquake in the last 24h -- same as the weather alert.
      if (g_quakeLine.length()) {
        setBrightnessBoth(BRIGHT_FULL);
        blink(true);
        showText(g_quakeLine, -1, 2500);
        blink(false);
        g_frame = "        ";
      }

      // then if there's a plane, show just the plane because that's what's cool.
      // Otherwise, show the trains and buses

      if (g_plane.valid) {
        // A plane over Brooklyn is the main event -- when one's up there, it's
        // all we show. Trains/buses/weather keep fetching in the background so
        // they're current again the moment it passes.
        showPlane(g_plane);
      } else {
        if (g_haveTrainData) {
          showArrivals(nowEpoch);
        }

        // ...then the buses (M14A -> Abingdon Sq, M9 -> Battery Pk City), if any.
        showBuses(nowEpoch);


      }

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
