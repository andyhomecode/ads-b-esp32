// ######                          #######
// #     #  #  #    #   ####          #     ######   ####   #####  ######  #####
// #     #  #  ##   #  #    #         #     #       #         #    #       #    #
// ######   #  # #  #  #              #     #####    ####     #    #####   #    #
// #        #  #  # #  #  ###         #     #            #    #    #       #####
// #        #  #   ##  #    #         #     #       #    #    #    #       #   #
// #        #  #    #   ####          #     ######   ####     #    ######  #    #
//
// Ping Tester
// Andy Maxwell | andy@maxwell.nyc
// 12/4/2024
// Making a gift for some folks around Workday like Sam and Kalan and George
// for the ESP32-S2 Dev Board,
// use the COM port, not USB

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>

// LED output
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"

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


const char *htmlTemplate =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>ADS-B Toy Config</title>\n"
  "</head>\n"
  "<body>\n"
  "  <h1>Andy's ADS-B Toy Config</h1>\n"
  "  <p><a href=\"https://github.com/andyhomecode/pingtester/\">Github</a></p>\n"
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


void egg() {
  // easter egg

  // pop up once every hour or so
  long tempRand = random(preferences.getInt("egg", 1800));

  // TODO, add a button to turn on to make it happen

  //  displayStringAcrossTwoDisplays(String(tempRand));
  //  delay(200);
  if (tempRand != 1) {
    return;
  }

  // you hear the bell, you get a prize!

  const String messages[] = {
    "Hello, world.",
    "DATA IS LIKE PEE IN THE POOL",
    "IT'S ALWAYS DNS",
    "Workday... AND NIGHT",
    "EVERYTHING IS AN OBJECT",
    "ASSETS = LIABILITIES + EQUITY",
    "DEBUGGER?? I USE PRINT",
    "It works on my machine.",
    "Pleasanton Marriott is better than the AC.",
    "Don't take BART from SFO.",
    "LGA, JFA, EWR, LAS, SFO, MIA, PDX, DFW, MCO, ORD",
    "I'm afraid I can't do that, Dave.",
    "Are you Sarah Connor?",
    "I'VE GOT 1099 PROBLEMS, BUT A W-2 AIN'T ONE",
    "V = I x R",
    "F = M x A",
    "sudo make me a sandwich",
    "The bathroom code is 1234*",
    "sudo rm -fr /",
    "[object Object]",
    "There's no place like 127.0.0.1",
    "AI stole my job.",
    "Recursion. See recursion.",
    "Error 418: I'm a teapot.",
    "There are 10 types of people.",
    "Bugs everywhere.",
    "1/0",
    "NaN",
    "#REF",
    "404 Brain Not Found",
    "500 Coffee Required",
    "418 I’m a Teapot",
    "200 OK-ish",
    "302 Found (Under Desk)",
    "451 Redacted by Legal",
    "503 Out to Lunch",
    "520 Unknown Unknown",
    "666 Daemon Not Responding",
    "1337 Hax Detected",
    "[object Object]",
    "NaN is NaN",
    "undefined is not a function",
    "null pointer exception",
    "true == \'true\'",
    "while(true) { }",
    "sudo make me a sandwich",
    "rm -rf /",
    "It compiles! Ship it!",
    "Segmentation fault (core dumped)",
    "0xDEADBEEF",
    "0xC0FFEE",
    "0xBADF00D",
    "0xBADC0DE",
    "0xFEEDFACE",
    "0xDEFEC8ED",
    "0xF00DBABE",
    "0xABADBABE",
    "101010 (42)",
    "0b11111111",
    "Hello, World!",
    "EOF reached",
    "Insert coin",
    "Loading… still",
    "PEBKAC error",
    "ID10T error",
    "Y2K compliant",
    "Works on my machine",
    "Feature, not a bug",
    "Skynet initializing…",
    "All your base are belong to us",
    "Don’t Panic (42)",
    "There is no spoon",
    "Use the Force();",
    "Winter is coding",
    "Hack the planet!",
    "Expecto Patronum();",
    "Open the pod bay doors",
    "Resistance is futile",
    "IP conflict detected",
    "DHCP lease expired",
    "WiFi? Why not?",
    "Ping 127.0.0.1",
    "Timeout exceeded",
    "Packet dropped",
    "DNS not found",
    "ARP! ARP!",
    "Hello from 127.0.0.1",
    "Routing… eventually",
    "RAID rebuild in progress",
    "Backup? What backup?",
    "Kernel panic!",
    "Zombie process detected",
    "Disk full… again",
    "Login as root? Brave.",
    "chmod 777 EVERYTHING",
    "Reboot fixes it",
    "Have you tried turning it off…",
    "…and on again?",
    "ERROR: Coffee empty",
    "WARNING: Sarcasm detected",
    "ALERT: Keyboard not found",
    "Insert brain to continue",
    "BEEP BOOP BEEP",
    "User not in sudoers",
    "Press F to pay respects",
    "SyntaxError: Unexpected token",
    "CTRL+ALT+DEL to continue",
    "System32 deleted successfully",
    "Ping of death",
    "Kernel not included",
    "Infinite loop engaged",
    "127.0.0.1 sweet 127.0.0.1",
    "Console.log('lol')",
    "Commit early, commit often",
    "Merge conflict incoming",
    "Hello from the other side()",
    "printf(\"Oops\\n\")",
    "Error loading error message",
    "Abort, Retry, Fail?",
    "Stack overflow!",
    "Out of cheese error",
    "Divide by cucumber",
    "Schrodinger’s cat alive()",
    "Schrodinger’s cat dead()",
    "sudo rm universe",
    "sudo apt-get happiness",
    "42 is the answer",
    "Insert witty error here"
};

  // Arrays of strings areweird in C so yeah, I'm hard coding the length
  // just be glad I didn't use the weird Harvard Architecture PROGMEM
  const int numMessages = 128;
  displayStringAcrossTwoDisplays("********");
  delay(1000);
  displayStringAcrossTwoDisplays("- ANDY -");
  delay(2000);
  displayStringAcrossTwoDisplays("- SAYS -");
  delay(2000);

  scrollText(messages[random(0, numMessages - 1)], 8, 200);
  displayStringAcrossTwoDisplays("********");
  delay(1000);
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
  Serial.print("Source at:\nhttps://github.com/andyhomecode/pingtester\n");

  randomSeed(analogRead(0));

  // setup the LED displays

  Wire.begin(SDA, SCL);  // SDA pin 9 and one in on LCD board, SLC pin 18 and rightmost on LCD board

  // setup the LED displays by device number
  alpha4_0.begin(0x70);  // first one
  alpha4_1.begin(0x71);  // 2nd one

  alpha4_0.clear();
  alpha4_1.clear();

  // title screen
  scrollText("ADS-B Toy by andy.maxwell@workday.com", displayLength, 200);
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
      http.begin("https://api.adsb.lol/v2/closest/40.6875/-73.9845/3");
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println(payload);
        // For now, just indicate success
        outputText = "ADS-B OK";
        dpAt = -1;
        blink(false);
      } else {
        Serial.printf("HTTP error: %d\n", httpCode);
        outputText = "**Error**";
        dpAt = -1;
        blink(true);
      }
      http.end();

      // show the output
      displayStringAcrossTwoDisplays(outputText, dpAt);

      // let them see it.
      delay(2000);

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