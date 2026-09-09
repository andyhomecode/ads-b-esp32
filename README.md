# F @ East Broadway  (+ planes)

I can't see the subway from my window, but I still want to know when to leave for
it. This device shows a live countdown to the next **F trains — both directions,
uptown and downtown** — at the **East Broadway** station on the Lower East Side,
on dual 14-segment LED displays.

And because the hardware started life as an LGA plane spotter: whenever there's an
airliner low over Brooklyn on final into **LaGuardia**, it slips the northern-most
one (the one closest to touchdown) in between subway passes — flight number,
airline, origin airport, aircraft type.

![She may not look like much, but she's got it where it counts, kid.](photo.jpeg)

It's an ESP32-based device that pulls real-time arrival data from the MTA's
GTFS-realtime feed (via the [wheresthefuckingtrain.com](https://wheresthefuckingtrain.com/)
JSON proxy, so no protobuf parsing on the microcontroller) and cycles through
the minutes-to-arrival for the next few trains each way, plus plane data from
[adsb.lol](https://adsb.lol/) and route lookups from [adsbdb.com](https://www.adsbdb.com/).

## Version
 - version 3.0
 - Sep 9, 2026

## Features

- **Next-train countdown, both directions in one list**: a single `E B'WAY`
  station frame, then the next few F trains **either way, sorted soonest-first**.
  Each frame is `nX YYmin` — `n` is the place in line, `X` is `U` (uptown) or
  `D` (downtown / Brooklyn): `1D  2min`, `2U  3min`, `3D  6min`, ... (`nX  NOW`
  when one's basically here, `NO F TRN` when nothing's running). Up to
  `NUM_TRAINS` per direction go into the merge.
- **Plane on approach**: when an `A3` (large / airliner) aircraft is between
  `ADSB_ALT_MIN` and `ADSB_ALT_MAX` feet inside the bounding disc over
  Williamsburg, the display adds a `*PLANE*` block after the trains: callsign
  (`AAL 1389`), airline, aircraft type (`Airbus A321`), altitude, and origin
  (`FROM MIA MIAMI`). Northern-most plane wins — it's the closest to LGA. No
  plane in the area → just the trains, same as before.
- **Fade + scroll transitions**: each frame dims, scrolls the old data out to
  the left while the new data scrolls in from the right, then fades back up to
  full brightness. Plane details scroll horizontally (they're longer than the
  8 columns).
- **Decoupled fetch**: hits the train server about every 30s and adsb.lol about
  every 20s, caches both; the display keeps looping between fetches. A failed or
  empty plane fetch just drops the plane block — the countdown is never affected.
- **NTP time sync**: turns the feed's absolute arrival timestamps into a live
  countdown; falls back to the feed's own `updated` time if NTP doesn't sync.
- **Dual 14-Segment LED Displays**: shows scrolling text and data.
- **WiFi Connectivity**: connects to your WiFi network to fetch live arrival data.
- **Setup Mode**: built-in access point for easy WiFi configuration via web
  interface, selectable by switch.
- **Run Mode**: does its little thing.

## Hardware

- [ESP32-S3 Wroom 1 Dev Board](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/index.html) (or compatible board)
- 2x [Adafruit 14-Segment LED Backpacks, Product ID: 2157](https://www.adafruit.com/product/2157) (I2C addresses 0x70 and 0x71)
- Physical [switch](https://www.nintendo.com/us/gaming-systems/switch-2/) connected to GPIO 13 (for mode selection)
- [USB cable](https://www.amazon.com/dp/B0DF1RRT3N) for power and programming
- 3D printed a case using this cool [OpenSCAD](https://openscad.org/) Ultimate [Box Maker](https://www.thingiverse.com/thing:1264391) on a [Prusa Mini+](https://www.prusa3d.com/category/original-prusa-mini/)

### Pins

- **I2C SDA**: GPIO 9
- **I2C SCL**: GPIO 18
- **Mode Switch**: GPIO 13 (INPUT_PULLUP)

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension recommended, built on Linux)
- USB drivers for ESP32 (usually automatic on Linux)

### Installation

If you're the type to do this, you probably don't need instructions, but...

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

### Case

See links above for info, but you can just print the .STLs under case.  If you'd like to modify them, use OpenSCAD and there is a lovely configurator that the original author wrote!  


- 130mm wide
- 120mm deep
- 45mm tall

LED PCB hole is 12mm higher than the centerline of the LED display. Box is 45 high.  45/2= 22.5 - 12 = 10.5mm down from top

Back of the LED PCB needs to be 25mm behind the back of the plexiglass to accommodate for pins, connectors, and depth of LEDs.

screw holes on the 20mm x 80mm PCBs are 76mm apart

Screw mounts placed in tinkercad rather than OpenSCAD because I like to fiddle.

I cut a piece of red plexiglass to the size/shape of the end cap to be able to see the LED display through. 


More pictures coming

## Usage

### Initial Setup

1. With the mode switch in **SETUP** position (LOW), power on the device.
2. The device creates a WiFi access point named "SUBWAY-ESP32" (no password).
3. Connect your phone/computer to this network.
4. Open a browser and go to `http://192.168.4.1` or whatever IP is shown.  HTTP only. No HTTPS.
5. Enter your WiFi SSID and password, then save.
6. The device will restart and attempt to connect to your WiFi.

### Normal Operation

1. Set the mode switch to **RUN** position (HIGH).
2. The device connects to WiFi, syncs the clock over NTP, and starts fetching arrival data.
3. It cycles: `E B'WAY` (~1s), then the next few F trains either direction,
   soonest-first, ~2s each as `1D  2min`, `2U  3min`, ... . Every frame fades
   down, scrolls the old data out while the new data scrolls in, then fades back
   up.
4. If there's an airliner low over Brooklyn on final into LGA, a `*PLANE*` block
   follows the trains: callsign, airline, aircraft type, altitude, origin
   airport. Otherwise it's trains only.
5. If WiFi fails, it displays "No Wi-fi" and restarts.

### Serial Monitor

- Open the serial monitor in PlatformIO at 9600 baud to see debug output (raw JSON, parsed epoch).
- DOES NOT WORK ON LINUX FOR SOME REASON

## Configuration

- **WiFi Credentials**: Stored in ESP32 flash memory. Reset by entering setup mode.
- **Station**: `STOP_ID` in `main.cpp` is `F16` (East Broadway). Both directions
  — `N` (uptown) and `S` (downtown / Brooklyn) — are merged into one
  soonest-first list. Other stop IDs: hit
  `https://api.wheresthefuckingtrain.com/by-id/<id>` or see the MTA GTFS
  `stops.txt`. To show only one direction, drop the other entry from the `dirs[]`
  array in `loop()`.
- **How many trains**: `NUM_TRAINS` in `main.cpp` (default 3, *per direction* —
  so up to 6 in the merged list).
- **Refresh rate**: `REFETCH_MS` in `main.cpp` (default 30000) — how often it
  re-hits the server; the display loops faster than this off the cache.
- **Brightness / animation feel**: `BRIGHT_FULL` / `BRIGHT_DIM` (HT16K33 levels
  0–15) and the `stepMs` / hold values in `showFrame()` / `showArrivals()`.
- **Plane bounding area**: `ADSB_LAT` / `ADSB_LON` / `ADSB_RADIUS_NM` in
  `main.cpp` — a point and a radius in nautical miles (adsb.lol has no free
  bbox endpoint, so the disc *is* the box). Defaults sit over Williamsburg on
  the LGA approach.
- **Plane altitude band**: `ADSB_ALT_MIN` / `ADSB_ALT_MAX` (feet, default
  800–5000) and `ADSB_CATEGORY` (`A3` = airliner-sized).
- **Plane refresh rate**: `ADSB_REFETCH_MS` (default 20000).
- **User-Agent**: `USER_AGENT` in `main.cpp` — **must** carry real contact info.
  adsb.lol returns `403 "User-Agent too generic; include valid contact info."`
  for a blank or generic UA, which is what silently killed the original
  plane-spotter build (the ESP32 `HTTPClient` default is `ESP32HTTPClient`).

## Troubleshooting

- **No Serial Output**: Ensure correct USB port (`/dev/ttyACM0`) and board selection (`esp32-s3-devkitc-1`). For ESP32-S3 Wroom 1, use the serial monitor for debugging output.
- **WiFi Not Connecting**: Check credentials in setup mode.
- **Displays Not Working**: Verify I2C connections and addresses.
- **Board Not Detected**: Try a different USB port/cable or press the reset button.
- **All trains show `NOW` or wrong minutes**: NTP didn't sync. Check internet
  access; the code falls back to the feed's `updated` timestamp, which can be a
  little stale.

## Code Structure

- `src/main.cpp`: Main Arduino sketch with setup, loop, and display functions.
- `platformio.ini`: PlatformIO configuration for ESP32-S3.
- `include/`: Header files (if any).
- `lib/`: Local libraries (if any).
- `case/`: OpenSCAD file for printing the case.


## API Reference

- **Arrival data**: [`api.wheresthefuckingtrain.com/by-id/F16`](https://api.wheresthefuckingtrain.com/by-id/F16)
  — a free JSON proxy of the MTA's GTFS-realtime BDFM feed. Returns the station
  name plus `N` (northbound) and `S` (southbound) arrays of `{route, time}`.
- **Upstream**: [MTA GTFS-realtime feeds](https://www.mta.info/developers) — the
  BDFM feed at `https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-bdfm`
  is protobuf-encoded and no longer needs an API key, if you'd rather decode it
  on-device.
- **Plane positions**: [`api.adsb.lol/v2/point/{lat}/{lon}/{radius_nm}`](https://api.adsb.lol/docs)
  — free, no key, but **requires a non-generic `User-Agent` with contact info**
  (else `403`). Returns an `ac[]` array; we filter to `category == "A3"` in the
  altitude band and take the highest `lat`.
- **Plane routes**: [`api.adsbdb.com/v0/callsign/{callsign}`](https://www.adsbdb.com/)
  — free callsign → airline + origin/destination airports. Replaces adsb.lol's
  old `/api/0/routeset`, which now returns an empty `201` for any request.
  `404 "unknown callsign"` just means no route on file; the plane still shows
  without an origin.

## License

This project is open-source. See the original repository for licensing details.

## Contributing

Feel free to submit issues or pull requests for improvements!

## Credits

- Arrival data via [wheresthefuckingtrain.com](https://wheresthefuckingtrain.com/) proxying the MTA GTFS-realtime feed.
- Plane data via [adsb.lol](https://adsb.lol/); route/airline lookups via [adsbdb.com](https://www.adsbdb.com/).
- ESP32 code reused from Andy's ADS-B plane spotter, itself reused from Andy's Ping Tester project. https://github.com/andyhomecode/pingtester
- Uses open-source libraries and APIs.
