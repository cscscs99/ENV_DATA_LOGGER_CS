Smart Environmental Data Logger
An ESP32 node that measures temperature, humidity and air quality and publishes them to the ESP RainMaker cloud. It is monitored and controlled from the RainMaker Home app (iOS), reports to Espressif Insights, sends push notifications on alarms, answers voice assistants (Alexa / Google Assistant through account linking), updates itself over the air, and recovers Wi-Fi and cloud connectivity on its own after a power loss.

Built with ESP-IDF v6.1 and FreeRTOS on a classic ESP32 (30-pin dev board, 4 MB flash).

Features
Requirement	Implementation
Live monitoring and control	Four RainMaker devices: Temperature Sensor, Humidity Sensor, Air Quality, Alarm. Thresholds and the alarm switch are writable from the app.
Insights	esp_insights over MQTT, with custom metrics temp, hum, air, alarm plus system diagnostics.
Push notifications	esp_rmaker_raise_alert() on high temperature, high humidity, poor air quality and failed OTA. Repeats every 60 s while the condition persists.
Voice control	Standard temperature-sensor device type; link the Alexa or Google Home skill from the RainMaker app.
OTA updates	RainMaker OTA with two app slots and bootloader rollback (the new image is kept only after it reaches the cloud).
Reset / re-handshake button	BOOT button: short press re-handshakes with the AP, 3 s hold resets Wi-Fi (re-provision), 10 s hold factory-resets the node.
Power-loss recovery	Wi-Fi credentials persist in NVS; the node reconnects by itself. A watchdog re-handshakes with the AP if the cloud stays unreachable for 30 s.
Hardware
Part	Pin	Notes
DHT11 (temperature, humidity)	GPIO4 (D4)	3-pin module: + to 3V3, - to GND, OUT to D4. Bare 4-pin sensor needs a 10 k pull-up from data to 3V3.
MQ-135 (air quality), digital output	GPIO23 (D23)	VCC to 3V3, GND to GND, DO to D23. The trimmer pot sets the trip level. DO low = POOR.
Alarm LED	GPIO19 (D19)	LED plus 330 ohm resistor to GND. On only while the air quality is POOR.
Buzzer (optional)	GPIO18 (D18)	Disabled by default (BUZZER_ENABLED 0). Drive through an NPN transistor when enabled.
BOOT button	GPIO0	On the dev board.
If the MQ-135 reads dead on 3V3, power it from VIN and put a 10 k / 20 k divider on DO so the pin never sees more than 3.3 V. Add the parts one at a time and watch the serial log after each; a wrong wire on the DHT11 data line is the most common fault.

Software architecture
Every task is event-driven; none polls with a blocking delay, and none waits for Wi-Fi.

 esp_timer (2 s) --> sensor task --+--> cloud queue --> cloud task  --> RainMaker params, Insights metrics
                                   |
                                   +--> alert queue --> alert task  --> thresholds, push notifications
                                                              |
                                                              +--> control queue --> control task --> LED, buzzer, Alarm status
 RainMaker writes (app, voice, schedules) --> write callback --> control queue / thresholds
 Wi-Fi, MQTT, provisioning events --> event group --> connect task (recovery watchdog)
 BOOT button ISR --> button queue --> button task --> event group --> connect task
 RainMaker OTA events --> ota queue --> ota task
File	Role
main/app_main.c	Startup, RainMaker node and device creation, write callback.
main/app_sensors.c	DHT11 bit-bang read and MQ-135 digital read.
main/app_cloud.c	Reports on change or every 30 s; feeds Insights.
main/app_alert.c	Hysteresis alarms (temperature, humidity), debounced air alarm, notifications.
main/app_control.c	Alarm LED, buzzer, Alarm status text.
main/app_connect.c	Wi-Fi/cloud events, button, recovery watchdog.
main/app_ota.c	OTA progress and failure alert.
main/app.h	Pins, timings, thresholds and build flags.
Thresholds default to 35 C and 80 % RH, with 2 C and 5 % hysteresis. The air alarm clears after five good readings in a row, and the MQ-135 is ignored for the first 30 s while its heater warms up.

Build flags (main/app.h)
Flag	Meaning
BUZZER_ENABLED	1 enables the buzzer.
AIR_DO_ACTIVE_LOW	1 if the MQ-135 module pulls DO low on gas.
BLE_ADDR_REROLL	Offsets the Bluetooth address. Bump it if iOS caches a stale Bluetooth service table and pairing fails.
Build and flash
source ~/.espressif/tools/activate_idf_v6.1.sh
cd env_logger
idf.py set-target esp32
idf.py -p /dev/cu.usbserial-0001 -b 115200 flash monitor
Use erase-flash before flashing to get a fresh node ID and clear stored Wi-Fi credentials. Only one program may hold the serial port at a time, and opening the port resets the board.

Pairing
Sign in to the RainMaker Home app under ESP RainMaker (not Neo).
Tap Add Device and scan the QR code printed in the serial log (or open the link printed under it). The payload is {"ver":"v1","name":"PROV_xxxxxx","username":"wifiprov","pop":"xxxxxxxx","transport":"ble"}.
Choose a 2.4 GHz Wi-Fi network and enter its password.
The node uses BLE provisioning with the sec2 scheme and assisted claiming, so no certificates need to be flashed.

Demonstration checklist
Power the board; show it joining Wi-Fi and the four devices appearing in the app.
Warm the DHT11 with a finger: temperature rises, then the push notification fires above the threshold.
Lower the threshold from the app to trigger the alert without heating anything.
Bring a gas source (a lighter without flame, or alcohol on a swab) near the MQ-135: Air Quality turns POOR, the LED lights, a notification arrives.
Remove it: after five good readings the LED turns off.
Open Insights in the RainMaker dashboard to show the metrics.
Power-cycle the board and show it returning online by itself.
Hold BOOT for 3 s to show the Wi-Fi reset and re-provisioning.
Say "what is the temperature" to the linked voice assistant.
Push a firmware update from the RainMaker dashboard.
