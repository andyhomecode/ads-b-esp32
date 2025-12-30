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


// lookup tables for airline  and aircraft type long names
std::map<String, String> airlineLookup;
std::map<String, String> icacoLookup;

// HTML template for the configuration page
const char *htmlTemplate =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>ADS-B-ESP32 Config</title>\n"
  "</head>\n"
  "<body>\n"
  "  <h1>Andy's ADS-B-ESP32 WIFI Config</h1>\n"
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



void displayText(String text, int dpLocation = -1) {
  if (text.length() <= 8) {
    displayStringAcrossTwoDisplays(text, dpLocation);
    delay(2000);  // Wait 2 seconds
  } else {
    // Show first 8 characters
    displayStringAcrossTwoDisplays(text.substring(0, 8), dpLocation);
    delay(2000);  // Wait 2 seconds

    // Scroll until the last character is in the right-most position
    for (int i = 1; i <= text.length() - 8; i++) {
      String frame = text.substring(i, i + 8);
      displayStringAcrossTwoDisplays(frame, -1);  // No decimal point during scroll
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
  const char *apSSID = "ADSB-ESP32";
  const char *apPassword = "";  // no password,
                                //the Wifi AP is only on when the switch is in SETUP,
                                // and with Arduino's Harvard architecture there's very little attack surface for overflows or other such shenanigans

  if (isConnected) {
    // if we're already connected to wifi for some reason, restart so we can start the AP.
    ESP.restart();
  }

  displayText("Connect to ADSB-ESP32...");

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

      server.send(200, "text/html", "<h1>Credentials Saved.</h1><p>Disconnect from setup Wi-Fi and flip switch to RUN. And watch some planes!</p>"); 
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

  // setup the LED displays

  Wire.begin(SDA, SCL);  // SDA pin 9 and one in on LCD board, SLC pin 18 and rightmost on LCD board

  // setup the LED displays by device number
  alpha4_0.begin(0x70);  // first one
  alpha4_1.begin(0x71);  // 2nd one

  alpha4_0.clear();
  alpha4_1.clear();

  // title screen
  displayText("andy@maxwell.nyc");
  displayText("=ADS-B=");
  displayText(" V 1.0");

  // get the stored Wifi credentials
  String ssid = preferences.getString("ssid", DEFAULT_SSID);
  String password = preferences.getString("password", DEFAULT_PASSWORD);


  // If Setup switch is in RUN, try to connect to WiFi using stored creds
  if (digitalRead(SWITCH_PIN) == HIGH && connectToWiFi(ssid.c_str(), password.c_str())) {
    isConnected = true;
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
                                      
      // __|__
      // \___/
      //  | |
      //  | |
      // _|_|______________
      //         /|\
      //       */ | \*
      //       / -+- \
      //   ---o--(_)--o---
      //     /  0 " 0  \
      //   */     |     \*
      //   /      |      \
      // */       |       \*

      // Here is the meat of the program
      // - call the APIs
      // - format the output
      // - show it

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
          displayText("**JSON Error**");
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
            String icaco = bestFlight["t"];
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
                if (airports.size() >= 2) {
                  JsonObject origin;
                  if (airports.size() == 3) {
                    origin = airports[1];  // middle airport for round-trip routes
                  } else {
                    origin = airports[0];  // first airport for direct routes
                  }
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

            // Separate airline code and flight number with space
            String airlineCodePart = flightId.substring(0, 3);
            String flightNum = flightId.substring(3);
            String flightText = airlineCodePart + " " + flightNum;

            blink(false);
            // show the airline code + flight number
            displayText(flightText);


            // Get airline full name
            String airlineCode = flightId.substring(0, 3);
            String airline = airlineLookup.count(airlineCode) ? airlineLookup[airlineCode] : "Unknown";
            
            displayText(airline);
            
            
            String aircraftType = icacoLookup.count(icaco) ? icacoLookup[icaco] : "Plane";

            displayText(aircraftType);

            displayText(alt_geom + " ft");

            if (originIata != "" && originIata != "LGA") {
              displayText(originIata + " " + originName);
            }

            displayText(flightText); // show it again before loading the next flight.

          } else {
            // no planes :(
            displayText("........");
            blink(false);
          }
        }
      } else {
        Serial.printf("HTTP error: %d\n", httpCode);
        displayText("HTTP error: " + String(httpCode));
        blink(true);
        ESP.restart(); // oh well
      }
      http.end();

    } else {
      Serial.println("Not connected to Wi-Fi.");
      displayText("No Wi-fi");
      ESP.restart();  
      //
      //
      //
      //  .-------------------.              ___
      // ( Have we landed yet? )            /  /]
      //  `-------------.   ,-'            /  / ]
      //                 \ |      _____,. '  /__]
      //              )   \|   ,-'             _>
      //                (  ` _/  N-ANDY   ,. '`
      //               )    / |     _,. '`
      //               (   /. /    |
      //                ) ,  /`  ./
      //               (  \_/   //_ _
      //                ) /    //  (_)
      //              _,~'#   (/.
      // ~~~~~~~~~~~~~~~#~~#~~~~~~~~~~~~~~~~~~~~~~~~~~~
      //
    }
  }
}