//     _    ____  ____        ____    _     ____    _    
//    / \  |  _ \/ ___|      | __ )  | |   / ___|  / \   
//   / _ \ | | | \___ \ _____|  _ \  | |  | |  _  / _ \  
//  / ___ \| |_| |___) |_____| |_) | | |__| |_| |/ ___ \ 
// /_/   \_\____/|____/      |____/  |_____\____/_/   \_\

// ads-b-esp32 
// Andy Maxwell | andy@maxwell.nyc
// 2025 12 25
// Find airplanes lining up for landing at LGA
// that I can see out over Brooklyn, out my window

// for the ESP32-S3 Wroom 1 Dev Board,
// use the COM port, not USB

// built on PlatformIO on linux

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>

// LED output
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"

#include <ArduinoJson.h>
#include <map>

#define SWITCH_PIN 13  // GPIO pin for mode switch

#define SDA 9
#define SCL 18

// instantiate the two i2c LED controllers
Adafruit_AlphaNum4 alpha4_1 = Adafruit_AlphaNum4();
Adafruit_AlphaNum4 alpha4_0 = Adafruit_AlphaNum4();

const int displayLength = 8;


// stand up the web server so we can config the settings
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


// global output string to have consistency across runs
String outputText = "<------>";  // the output to show on the display, preseve between loops.

int dpAt = -1;  // position of the decimal point.  -1 to turn off

// how many times since I showed the site address I'm pinging
// it'll show the machine it's pinging that number of pings.
const int siteShowCountMax = 30;
int siteShowCount = siteShowCountMax;

std::map<String, String> airlineLookup;


const char *htmlTemplate =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>ADS-B Toy Config</title>\n"
  "</head>\n"
  "<body>\n"
  "  <h1>Andy's ADS-B Toy Config</h1>\n"
  "  <p><a href=\"https://github.com/andyhomecode/ads-b-esp32\">Github</a></p>\n"
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






void padString(char *str, int maxLength) {
  // straight from ChatGPT, baby.  It's a good Jr Programmer.

  int len = strlen(str);

  // If the string is longer than maxLength, truncate it
  if (len > maxLength) {
    str[maxLength] = '\0';  // Cut off at maxLength
    return;
  }

  // If shorter, pad with spaces
  int padSize = maxLength - len;
  char temp[maxLength + 1];  // Temporary buffer

  // Fill with spaces
  memset(temp, ' ', padSize);

  // Copy original string to the end of the temp buffer
  strcpy(temp + padSize, str);

  // Copy back to original string
  strncpy(str, temp, maxLength);
  str[maxLength] = '\0';  // Ensure null termination
}

//  ____  _           _
// |  _ \(_)___ _ __ | | __ _ _   _
// | | | | / __| '_ \| |/ _` | | | |
// | |_| | \__ \ |_) | | (_| | |_| |
// |____/|_|___/ .__/|_|\__,_|\__, |
//             |_|            |___/


// Function to display a string across two displays
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



void scrollText(String text, int displayWidth, int delayTime) {
  String paddedText = "        " + text + "        ";  // Pad with spaces front and back for smooth intro and exit
  int textLength = paddedText.length();

  for (int i = 0; i <= textLength - displayWidth; i++) {
    String frame = paddedText.substring(i, i + displayWidth);
    displayStringAcrossTwoDisplays(frame);
    delay(delayTime);
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
    scrollText(tempOut, displayLength, 200);
    return true;
  } else {
    Serial.println("\nFailed to connect.");

    char tempOut[20];
    for (int i = 0; i < 3; i++)
      scrollText("Wi-Fi Failed to Connect", displayLength, 200);

    return false;
  }
}

void startAccessPoint() {
  const char *apSSID = "ADSB-Toy";
  const char *apPassword = "";  // no password,
                                //the Wifi AP is only on when the switch is in SETUP,
                                // and with Arduino's Harvard architecture there's very little attack surface for overflows or other such shenanigans

  if (isConnected) {
    // if we're already connected to wifi for some reason, restart so we can start the AP.
    ESP.restart();
  }

  scrollText("Connect to PingToy...", displayLength, 200);

  WiFi.softAP(apSSID, apPassword);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP started. IP: %s\n", IP.toString().c_str());

  char tempOut[20];
  sprintf(tempOut, "IP %s", IP.toString());

  for (int i = 0; i < 3; i++)
    scrollText(tempOut, displayLength, 200);

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

  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      preferences.putString("ssid", server.arg("ssid"));
      preferences.putString("password", server.arg("password"));

      server.send(200, "text/html", "<h1>Credentials Saved.</h1><p>Disconnect from PingToy Wi-Fi</p>");  // add comment for IP saved
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
      displayStringAcrossTwoDisplays("-REBOOT-");
      delay(300);
      ESP.restart();
    }

    displayStringAcrossTwoDisplays("-Setup-");
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

  randomSeed(analogRead(0));

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

  // setup the LED displays

  Wire.begin(SDA, SCL);  // SDA pin 9 and one in on LCD board, SLC pin 18 and rightmost on LCD board

  // setup the LED displays by device number
  alpha4_0.begin(0x70);  // first one
  alpha4_1.begin(0x71);  // 2nd one

  alpha4_0.clear();
  alpha4_1.clear();

  // title screen
  scrollText("andy@maxwell.nyc", displayLength, 200);
  displayStringAcrossTwoDisplays("-=ADS-B=-");

  // get the stored Wifi credentials
  String ssid = preferences.getString("ssid", DEFAULT_SSID);
  String password = preferences.getString("password", DEFAULT_PASSWORD);


  // If Setup switch is in RUN, try to connect to WiFi using stored creds
  if (digitalRead(SWITCH_PIN) == HIGH && connectToWiFi(ssid.c_str(), password.c_str())) {
    isConnected = true;
    // startServer();  // used to setup the destination to ping
  } else {
    isConnected = false;
    // startAccessPoint();  // used ot setup the WiFi connection and destination to ping
  }
}

void loop() {

  Serial.println("Start of loop");

  char outputChar[20] = "xxxxxxx";  // local temp output for the loop


  // check to see if the mode switch is set to SETUP
  if (digitalRead(SWITCH_PIN) == LOW) {
    // we're in setup mode

    // so show the web server and handle it.
    displayStringAcrossTwoDisplays("*Setup*");
    startAccessPoint();  // we're not coming back from there.  It starts the wifi access point and web server.
  } else {


    if (WiFi.status() != WL_CONNECTED) {
      // ruh roh.  Not connected to wi-fi.
      displayStringAcrossTwoDisplays("No Wi-fi");
      delay(1000);
      ESP.restart();  // maybe better luck next time?
    }

    if (isConnected) {
      // we're on wifi.  good deal
      Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());

      displayStringAcrossTwoDisplays(outputText, dpAt);  // show the last output

      // Make HTTP GET to ADS-B API
      HTTPClient http;
      http.begin("https://api.adsb.lol/v2/point/40.6875/-73.9845/3");
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println(payload);

        // Parse JSON
        JsonDocument doc; // Adjust size as needed
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
          Serial.print("JSON parse error: ");
          Serial.println(error.c_str());
          outputText = "**JSON**";
          dpAt = -1;
          blink(true);
        } else {
          JsonArray ac = doc["ac"];
          // Find the flight with highest lat, filtered
          float maxLat = -1000;
          JsonObject bestFlight;
          for (JsonObject flight : ac) {
            String category = flight["category"];
            if (category == "A3") {
              float alt = flight["alt_geom"] | 0;
              if (alt >= 1000 && alt <= 5000) {
                float lat = flight["lat"];
                if (lat > maxLat) {
                  maxLat = lat;
                  bestFlight = flight;
                }
              }
            }
          }
          if (!bestFlight.isNull()) {
            String flightId = bestFlight["flight"];
            String alt_geom = bestFlight["alt_geom"];
            Serial.printf("Best flight: %s\n", flightId.c_str());

            // Get route
            JsonDocument postDoc;
            JsonArray planes = postDoc["planes"].to<JsonArray>();
            JsonObject plane = planes.add<JsonObject>();
            flightId.trim();
            plane["callsign"] = flightId;
            plane["lat"] = 0;
            plane["lng"] = 0;
            String postPayload;
            serializeJson(postDoc, postPayload);

            HTTPClient http2;
            http2.begin("https://api.adsb.lol/api/0/routeset");
            http2.addHeader("Content-Type", "application/json");
            int postCode = http2.POST(postPayload);
            String originIata = "";
            String originName = "";
            if (postCode == HTTP_CODE_OK) {
              String routePayload = http2.getString();
              Serial.println(routePayload);
              JsonDocument routeDoc;
              DeserializationError routeError = deserializeJson(routeDoc, routePayload);
              if (!routeError && routeDoc.size() > 0) {
                JsonObject route = routeDoc[0];
                JsonArray airports = route["_airports"];
                if (airports.size() >= 1) {
                  JsonObject origin = airports[0];
                  originIata = origin["iata"] | "";
                  originName = origin["location"] | "";
                  // Simple name cleaning: remove common words
                  originName.replace("International", "");
                  originName.replace("National", "");
                  originName.replace("Ronald Reagan", "");
                  originName.replace("Bergstrom", "");
                  originName.replace("Douglas", "");
                  originName.replace("Hilton Head", "");
                  originName.replace("Hartsfield Jackson", "");
                  originName.replace("Airport", "");
                  originName.replace("Regional", "");
                  originName.replace("Municipal", "");
                  originName.replace("Field", "");
                  originName.trim();
                }
              }
            } else {
              Serial.printf("Route POST error: %d\n", postCode);
            }
            http2.end();

            outputText = flightId;
            dpAt = -1;
            blink(false);

            // show the airline code + flight number
            displayStringAcrossTwoDisplays(outputText, dpAt);
            // let them see it.
            delay(1000);


            // Get airline full name
            String icao = flightId.substring(0, 3);
            String airline = airlineLookup.count(icao) ? airlineLookup[icao] : "Unknown";

            // Display for scroll, dropping flight since shown across 2 displays
            // String displayText = flightId + " " + alt_geom + " ft " + airline + " " ;
            String displayText = alt_geom + " ft " + airline + " " ;
            if (originIata != "") {
              displayText += " " + originIata + " " + originName;
            }
            scrollText(displayText, displayLength, 300); // slow it down for Michele
          } else {
            outputText = "........";
            dpAt = -1;
            blink(false);
          }
        }
      } else {
        Serial.printf("HTTP error: %d\n", httpCode);
        outputText = "**Error**";
        dpAt = -1;
        blink(true);
      }
      http.end();

      // show the output (either flight code or ... or error)
      displayStringAcrossTwoDisplays(outputText, dpAt);

      // let them see it.
      delay(1000);

      blink(false);

      // format for display on LED or servo.
    } else {
      Serial.println("Not connected to Wi-Fi.");
      displayStringAcrossTwoDisplays("No Wi-fi");
      delay(1000);
      ESP.restart();  // maybe better luck next time?
    }
  }
}