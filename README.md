# ADS-B ESP32

Shows info on airline flights on approach to La Guardia that fly over Williamsburg, Brooklyn on approach to LGA that I can see out my window.

An ESP32-based device that displays real-time ADS-B flight data on dual 14-segment LED displays. It fetches aircraft data from the ADS-B API and shows information like closest flights, altitudes, aircraft types (mapped from ICAO codes), airlines, and origin airports.

## Version
 - version 1.1
 - Dec 29, 2025

## Features

- **Flight Filtering**: Displays flights over Brooklyn at altitudes between 1000-5000 feet on approach to LGA.
- **Code -> English lookups**: converts ICAC and airline codes to english
- **Dual 14-Segment LED Displays**: Shows scrolling text and data.
- **WiFi Connectivity**: Connects to your WiFi network to fetch live ADS-B data.
- **Setup Mode**: Built-in access point for easy WiFi configuration via web interface selectable by switch.
- **Run Mode**: does it's little thing. 

## Hardware Requirements

- ESP32-S3 Wroom 1 Dev Board (or compatible board)
- 2x Adafruit 14-Segment LED Backpacks (I2C addresses 0x70 and 0x71)
- Physical switch connected to GPIO 13 (for mode selection)
- USB cable for power and programming

### Pin Connections

- **I2C SDA**: GPIO 9
- **I2C SCL**: GPIO 18
- **Mode Switch**: GPIO 13 (INPUT_PULLUP)

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension recommended, built on Linux)
- USB drivers for ESP32 (usually automatic on Linux)

### Installation

1. Clone or download this project.
2. Open in PlatformIO (or VS Code with PlatformIO extension).
3. Connect your ESP32 board via USB.
4. Build and upload the firmware:
   - Click the "Upload" button in PlatformIO, or run `platformio run --target upload --environment freenove_esp32_s3_wroom`

### Dependencies

The project uses the following libraries (automatically installed via PlatformIO):
- Adafruit GFX Library
- Adafruit LED Backpack Library
- ArduinoJson

## Usage

### Initial Setup

1. With the mode switch in **SETUP** position (LOW), power on the device.
2. The device creates a WiFi access point named "ADSB-ESP32" (no password).
3. Connect your phone/computer to this network.
4. Open a browser and go to `http://192.168.4.1` or whatever IP is shown.  HTTP only. No HTTPS.
5. Enter your WiFi SSID and password, then save.
6. The device will restart and attempt to connect to your WiFi.

### Normal Operation

1. Set the mode switch to **RUN** position (HIGH).
2. The device will connect to WiFi and start fetching ADS-B data.
3. Flight information scrolls on the LED displays, showing flight number, altitude, aircraft type, airline, and origin airport.
4. If WiFi fails, it displays "No Wi-fi" and restarts.

### Serial Monitor

- Open the serial monitor in PlatformIO at 9600 baud to see debug output.
- DOES NOT WORK ON LINUX FOR SOME REASON

## Configuration

- **WiFi Credentials**: Stored in ESP32 flash memory. Reset by entering setup mode.
- **API Endpoint**: Currently fetches closest flights to coordinates (40.6875, -73.9845). Modify in `main.cpp` for different locations.

## Troubleshooting

- **No Serial Output**: Ensure correct USB port (`/dev/ttyACM0`) and board selection (`esp32-s3-devkitc-1`). For ESP32-S3 Wroom 1, use the serial monitor for debugging output.
- **WiFi Not Connecting**: Check credentials in setup mode.
- **Displays Not Working**: Verify I2C connections and addresses.
- **Board Not Detected**: Try a different USB port/cable or press the reset button.

## Code Structure

- `src/main.cpp`: Main Arduino sketch with setup, loop, and display functions.
- `platformio.ini`: PlatformIO configuration for ESP32-S3.
- `include/`: Header files (if any).
- `lib/`: Local libraries (if any).

## API Reference

- **ADS-B Data Source**: 
  - [api.adsb.lol/v2/point/](https://api.adsb.lol/v2/point/) - Fetches closest aircraft data.
  - [api.adsb.lol/api/0/routeset](https://api.adsb.lol/api/0/routeset) - Retrieves flight route information.
  - [api.adsb.lol](https://api.adsb.lol/) Open ADS-B data API docs. 

## License

This project is open-source. See the original repository for licensing details.

## Contributing

Feel free to submit issues or pull requests for improvements!

## Credits

- test harness in python to develop API calls: https://github.com/andyhomecode/adsb-lga
- ESP32 code reused from Andy's Ping Tester project. https://github.com/andyhomecode/pingtester
- Uses open-source libraries and APIs.