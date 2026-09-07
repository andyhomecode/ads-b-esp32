//  _   _ ____ _____ _____        _   _   _
// | | | |  _ \_   _|_   _|__ __ | \ | | | |
// | | | | |_) || |   | |/ _ \ V /|  \| | | |
// | |_| |  __/ | |   | | (_) V / | |\  | |_|
//  \___/|_|    |_|   |_|\___/_/  |_| \_| (_)
//
// uptown-f-esp32
// Andy Maxwell | andy@maxwell.nyc
// 2025 12 25, repurposed 2026 09 07
// Show the countdown to the next uptown (northbound) F trains
// at the East Broadway station on the Lower East Side.
//
// Same hardware as the old plane-spotter: ESP32-S3 Wroom 1 Dev Board,
// two Adafruit 14-segment LED backpacks (0x70 / 0x71), mode switch on GPIO 13.
//
// for the ESP32-S3 Wroom 1 Dev Board,
// use the COM port, not USB
//
// built on PlatformIO on linux

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <time.h>

// LED output
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"

#include <ArduinoJson.h>

#define SWITCH_PIN 13  // GPIO pin for mode switch

#define SDA 9
#define SCL 18

// What we're watching.
// Stop IDs come from the MTA GTFS feed; East Broadway on the F is "F16".
// The "N" array in the response is northbound (uptown / Manhattan- & Queens-bound).
#define API_URL_BASE   "https://api.wheresthefuckingtrain.com/by-id/"
#define STOP_ID        "F16"
#define NUM_TRAINS     3      // how many upcoming trains to show
#define REFETCH_MS     30000  // re-hit the server about this often; the display loops faster

#define BRIGHT_FULL    15     // HT16K33 brightness, parked frame
#define BRIGHT_DIM     1      // HT16K33 brightness while a frame scrolls in

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


bool isConnected = false;  // global variable to show WiFi state


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


//      _ _           _
//   __| (_)___ _ __ | | __ _ _   _
//  / _` | / __| '_ \| |/ _` | | | |
// | (_| | \__ \ |_) | | (_| | |_| |
//  \__,_|_|___/ .__/|_|\__,_|\__, |
//             |_|            |___/


void displayStringAcrossTwoDisplays(String text, int dPLocation = -1) {

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
    alpha4_0.writeDigitAscii(i, c, i == dPLocation);  // Write each character to the display, if it's the character with the decimal point, show it
  }

  // Write to Display 2
  for (int i = 0; i <= 3; i++) {
    char c = text.charAt(i + 4);                            // remember we're showing the next 4 digits
    alpha4_1.writeDigitAscii(i, c, (i == dPLocation - 4));  // Write each character to the display, ditto for the decimal point
  }

  // Update both displays
  alpha4_0.writeDisplay();
  alpha4_1.writeDisplay();
}



void displayText(String text, int dpLocation = -1, int holdMs = 2000) {
  if (text.length() <= 8) {
    displayStringAcrossTwoDisplays(text, dpLocation);
    delay(holdMs);
  } else {
    // Show first 8 characters
    displayStringAcrossTwoDisplays(text.substring(0, 8), dpLocation);
    delay(holdMs);

    // Scroll until the last character is in the right-most position
    for (int i = 1; i <= text.length() - 8; i++) {
      String frame = text.substring(i, i + 8);
      displayStringAcrossTwoDisplays(frame, -1);  // No decimal point during scroll
      delay(200);  // Delay between scroll frames
    }
    delay(1000);  // Pause for 1 second at the end
  }
}


// What's currently parked on the 8 columns, so the next frame can scroll the
// old data out to the left while the new data scrolls in from the right.
String g_frame = "        ";

void setBrightnessBoth(uint8_t b) {
  alpha4_0.setBrightness(b);
  alpha4_1.setBrightness(b);
}

void fadeBrightnessBoth(int from, int to, int stepMs) {
  int dir = (to >= from) ? 1 : -1;
  for (int b = from; b != to; b += dir) {
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

  fadeBrightnessBoth(BRIGHT_FULL, BRIGHT_DIM, 8);

  String buf = g_frame + next;  // 16 columns: old data | new data
  for (int i = 1; i <= 8; i++) {
    displayStringAcrossTwoDisplays(buf.substring(i, i + 8), -1);
    delay(stepMs);
  }
  g_frame = next;

  fadeBrightnessBoth(BRIGHT_DIM, BRIGHT_FULL, 14);
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

// Parse an ISO-8601 timestamp with a numeric TZ offset (or trailing "Z")
// into a UTC epoch. Returns 0 if it can't be parsed.
long isoToEpoch(const char *iso) {
  if (iso == nullptr || iso[0] == '\0') return 0;

  int Y, Mo, D, h, m, s;
  char sign = 'Z';
  int tzh = 0, tzm = 0;
  int n = sscanf(iso, "%d-%d-%dT%d:%d:%d%c%d:%d",
                 &Y, &Mo, &D, &h, &m, &s, &sign, &tzh, &tzm);
  if (n < 6) return 0;

  long utc = daysFromCivil(Y, Mo, D) * 86400L + h * 3600L + m * 60L + s;

  // Back out the offset so we're in true UTC. "14:30-04:00" is 18:30 UTC.
  if (n >= 7 && (sign == '+' || sign == '-')) {
    long offset = (long)tzh * 3600 + (long)tzm * 60;
    utc += (sign == '-') ? offset : -offset;
  }
  return utc;
}


// One display pass: each frame fades down, scrolls the old data out / new data
// in, then fades back up and holds -- "UPTOWN F" (~1s), "E B'WAY" (~1s), then
// each cached arrival as "n XXmin" (~2s), or "n  NOW" when it's basically here.
void showArrivals(const long *arrivals, int count, long nowEpoch) {
  showFrame("UPTOWN F", 500);
  showFrame("E B'WAY", 500);

  if (count == 0) {
    showFrame("NO F TRN", 1400);
    return;
  }

  for (int i = 0; i < count; i++) {
    long mins = (arrivals[i] - nowEpoch + 30) / 60;

    char frame[12];
    if (mins <= 0) {
      snprintf(frame, sizeof(frame), "%d  NOW", i + 1);
    } else {
      if (mins > 99) mins = 99;
      snprintf(frame, sizeof(frame), "%d %2ldmin", i + 1, mins);
    }
    showFrame(frame, 1300);
  }
}


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
    displayText(tempOut);
    return true;
  } else {
    Serial.println("\nFailed to connect.");

    char tempOut[20];
    for (int i = 0; i < 3; i++)
      displayText("Wi-Fi Failed to Connect");

    return false;
  }
}

void startAccessPoint() {
  const char *apSSID = "SUBWAY-ESP32";
  const char *apPassword = "";  // no password,
                                //the Wifi AP is only on when the switch is in SETUP,
                                // and with Arduino's Harvard architecture there's very little attack surface for overflows or other such shenanigans

  if (isConnected) {
    // if we're already connected to wifi for some reason, restart so we can start the AP.
    ESP.restart();
  }

  displayText("Connect to SUBWAY-ESP32...");

  WiFi.softAP(apSSID, apPassword);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP started. IP: %s\n", IP.toString().c_str());

  char tempOut[20];
  sprintf(tempOut, "IP %s", IP.toString());

  for (int i = 0; i < 3; i++)
    displayText(tempOut);

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
      displayText("-REBOOT-");
      delay(300);
      ESP.restart();
    }

    displayText("-Setup-");
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
  displayText("andy@maxwell.nyc");
  displayText("=FTRAIN=");
  displayText(" V 2.3");

  // get the stored Wifi credentials
  String ssid = preferences.getString("ssid", DEFAULT_SSID);
  String password = preferences.getString("password", DEFAULT_PASSWORD);


  // If Setup switch is in RUN, try to connect to WiFi using stored creds
  if (digitalRead(SWITCH_PIN) == HIGH && connectToWiFi(ssid.c_str(), password.c_str())) {
    isConnected = true;

    // Kick off NTP so we can turn arrival timestamps into a countdown.
    // Work in UTC (offset 0); the feed's timestamps carry their own offset.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    for (int i = 0; i < 20 && time(nullptr) < 1700000000L; i++) {
      delay(250);
    }
    Serial.printf("NTP epoch: %ld\n", (long)time(nullptr));
  } else {
    isConnected = false;
  }
}

void loop() {

  Serial.println("Start of loop");


  // check to see if the mode switch is set to SETUP
  if (digitalRead(SWITCH_PIN) == LOW) {
    // we're in setup mode

    // so show the web server and handle it.
    displayText("*Setup*");
    startAccessPoint();  // we're not coming back from there.  It starts the wifi access point and web server.
  } else {


    if (WiFi.status() != WL_CONNECTED) {
      // ruh roh.  Not connected to wi-fi.
      displayText("No Wi-fi");
      ESP.restart();  // maybe better luck next time?
    }

    if (isConnected) {

      //        _______________
      //   _____|_[]_[]_[]_[]__|__
      //  |_ East Broadway  uptown |
      //  |_o_______________o______|
      //     O-O         O-O

      // The plan:
      // - every REFETCH_MS, hit the JSON proxy and cache the next few arrival
      //   times as absolute epochs
      // - every pass through loop(), redraw the countdown from that cache so the
      //   minutes tick down without hammering the server

      static long arrivals[NUM_TRAINS];
      static int  arrivalCount = 0;
      static long fetchEpoch = 0;            // our clock at the moment of the last fetch
      static unsigned long lastFetchMs = 0;
      static bool haveData = false;

      if (!haveData || millis() - lastFetchMs >= REFETCH_MS) {
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
            displayText("JSONErr");
            blink(true);
          } else {
            blink(false);

            JsonArray north = doc["data"][0]["N"];  // northbound == uptown

            // "now" from NTP, or fall back to the feed's own update time
            long nowEpoch = (long)time(nullptr);
            if (nowEpoch < 1700000000L) {
              nowEpoch = isoToEpoch(doc["updated"] | "");
            }

            arrivalCount = 0;
            if (!north.isNull()) {
              for (JsonObject t : north) {
                if (arrivalCount >= NUM_TRAINS) break;
                long e = isoToEpoch(t["time"] | "");
                if (e > 0) arrivals[arrivalCount++] = e;
              }
            }

            fetchEpoch = nowEpoch;
            lastFetchMs = millis();
            haveData = true;
          }
        } else {
          Serial.printf("HTTP error: %d\n", httpCode);
          displayText("HTTP " + String(httpCode));
          blink(true);
          ESP.restart();  // oh well
        }
        http.end();
      }

      if (haveData) {
        // current time: NTP if we have it, else the fetch clock plus elapsed
        long nowEpoch = (long)time(nullptr);
        if (nowEpoch < 1700000000L) {
          nowEpoch = fetchEpoch + (long)((millis() - lastFetchMs) / 1000);
        }
        showArrivals(arrivals, arrivalCount, nowEpoch);
      }

    } else {
      Serial.println("Not connected to Wi-Fi.");
      displayText("No Wi-fi");
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
