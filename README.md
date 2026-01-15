Outdoor LED Clock (ESP8266)
A fully‑featured outdoor LED clock built on the ESP8266 platform.
It displays time, temperature, and date, includes a web‑based configuration interface, supports OTA firmware updates, and features a customizable temperature‑based color system.
This project was created with the assistance of Microsoft Copilot.

Overview
This project is a standalone outdoor LED clock designed for long‑term, reliable operation.
It uses a WS2812B LED matrix to render large, bright digits visible both day and night.
The device connects to Wi‑Fi, synchronizes time via NTP, and exposes a full Web UI for configuration and diagnostics.

The system is optimized for stability, low memory usage, and smooth visual transitions.
It is suitable for outdoor installations, workshops, garages, or any environment where a bright, readable LED clock is needed.

Hardware Components
Microcontroller
ESP8266 NodeMCU  
Provides Wi‑Fi connectivity, Web UI, OTA updates, and controls the LED matrix.

LED Display
118 × WS2812B individually addressable RGB LEDs

Arranged as a custom matrix behind a black acrylic diffuser

GRB color order

Powered from a 5V supply (3–5A recommended depending on brightness)

Sensors
DS18B20 temperature sensor  
Measures ambient temperature with high accuracy.

LDR (Light‑Dependent Resistor)  
Used for automatic brightness adjustment based on ambient light.

DS3231 RTC module  
Maintains accurate time even when Wi‑Fi is unavailable or power is lost (battery‑backed).

Buttons
Reset / Mode button

3–5 seconds → enter Wi‑Fi AP setup mode

10 seconds → factory reset

Display Construction
The LED matrix is mounted behind a black acrylic front panel, which acts as a diffuser.
This creates smooth, uniform digits and improves visibility in daylight.
The enclosure can be 3D‑printed, CNC‑cut, or custom‑built depending on installation needs.

Key Features
Time & Date
Large 4‑digit time display (HH:MM)

Blinking seconds indicator

Automatic NTP synchronization

DST (Daylight Saving Time) support for EU rules

RTC fallback when Wi‑Fi is unavailable

Temperature
DS18B20 sensor reading

Adjustable temperature offset

Configurable display cycle (period + duration)

24‑hour temperature history graph (SVG in Web UI)

Temperature‑Based Color System (v1.5)
Fully configurable color zones (6 intervals)

Smooth or zoned transitions

Adjustable temperature step (1/2/5/10 °C)

Real‑time color preview in Web UI

Brightness Control
Automatic brightness via LDR

Manual brightness mode

Night mode with start/end time

Over‑temperature brightness limiting

Web Interface
Full configuration panel

Real‑time data table

Color pickers with live preview

Temperature graph

Login system with server‑side session timeout

AJAX heartbeat to maintain session security

OTA Updates
Web OTA (upload firmware via browser)

ArduinoOTA (upload from Arduino IDE over Wi‑Fi)

Diagnostics & Tools
LED test modes

Night mode test

RTC battery status

Factory reset

System reboot

Wi‑Fi Modes
AP Mode (Setup)
Activated when:

No Wi‑Fi credentials are stored

Or the button is held for 3–5 seconds

Default AP:

Code
SSID: Laikrodis-Setup
Password: Laikrodis2026
STA Mode (Normal Operation)
Connects to your home Wi‑Fi and enables:

Login‑protected Web UI

NTP synchronization

OTA updates

Project Status
This project is actively developed and evolving.
Version v1.5 introduces a major upgrade to the color system and session handling.

Acknowledgements
This project was created with the assistance of Microsoft Copilot, which helped design, debug, and refine both the firmware and documentation.
