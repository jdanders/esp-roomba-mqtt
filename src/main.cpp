#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <Roomba.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"
extern "C" {
#include "user_interface.h"
}

// Remote debugging over telnet. Just run:
// `telnet roomba.local` OR `nc roomba.local 23`
#if LOGGING
#include <RemoteDebug.h>
#define DLOG(msg, ...) if(Debug.isActive(Debug.DEBUG)){Debug.printf(msg, ##__VA_ARGS__);}
#define VLOG(msg, ...) if(Debug.isActive(Debug.VERBOSE)){Debug.printf(msg, ##__VA_ARGS__);}
RemoteDebug Debug;
#else
#define DLOG(msg, ...)
#endif

// Roomba setup
Roomba roomba(&Serial, Roomba::Baud115200);

// Roomba state
typedef struct {
  // Sensor values
  uint8_t dirt;
  int16_t distance;
  uint8_t chargingState;
  uint16_t voltage;
  int16_t current;
  // Supposedly unsigned according to the OI docs, but I've seen it
  // underflow to ~65000mAh, so I think signed will work better.
  int16_t charge;
  uint16_t capacity;
  int16_t velocity;

  // Derived state
  bool cleaning;
  bool docked;

  int timestamp;
  bool sent;
} RoombaState;

RoombaState roombaState = {};
int16_t total_distance = 0;
int lastSong = 0;

// Roomba sensor packet
uint8_t roombaPacket[100];
uint8_t sensors[] = {
  Roomba::SensorDirtDetect, // PID 15, 1 bytes, 0-255, signed
  Roomba::SensorDistance, // PID 19, 2 bytes, mm, signed
  Roomba::SensorChargingState, // PID 21, 1 byte
  Roomba::SensorVoltage, // PID 22, 2 bytes, mV, unsigned
  Roomba::SensorCurrent, // PID 23, 2 bytes, mA, signed
  Roomba::SensorBatteryCharge, // PID 25, 2 bytes, mAh, unsigned
  Roomba::SensorBatteryCapacity, // PID 26, 2 bytes, mAh, unsigned
  Roomba::SensorVelocity // PID 39, 2 bytes, mm/s, signed
};

// Network setup
WiFiClient wifiClient;
bool OTAStarted;

// MQTT setup
PubSubClient mqttClient(wifiClient);
const PROGMEM char *commandTopic = MQTT_COMMAND_TOPIC;
const PROGMEM char *statusTopic = MQTT_STATE_TOPIC;
const PROGMEM char *attributesTopic = MQTT_ATTRIBUTES_TOPIC;

void set_the_song() {
  //uint8_t jaws[16] = {52,32,53,32,52,32,53,32,52,32,53,32,52,32,53,32};
  //roomba.start();
  //delay(50);
  //roomba.song(0, jaws, 16);
  // LotR thirds:
  // f d e c d Bb c a
  uint8_t dur = 32;
  uint8_t lotr[32] =
    {53, dur, 50, dur, 52, dur, 48, dur, 50, dur, 46, dur, 48, dur, 45, dur,
     53, dur, 50, dur, 52, dur, 48, dur, 50, dur, 46, dur, 48, dur, 45, dur};
  roomba.start();
  delay(50);
  roomba.song(0, lotr, 32);
}

void wakeup() {
#ifndef ROOMBA_500
  DLOG("Wakeup Roomba\n");
  pinMode(BRC_PIN,OUTPUT);
  digitalWrite(BRC_PIN,LOW);
  delay(200);
  pinMode(BRC_PIN,INPUT);
  delay(200);
  roomba.start();
#else
  // test connection to roomba
  uint8_t packetLength;
  bool result = roomba.pollSensors(roombaPacket, sizeof(roombaPacket), &packetLength);
  if (result)  {
    DLOG("No need to wakeup Roomba\n");
  } else {
    DLOG("Wakeup Roomba\n");
    pinMode(BRC_PIN, OUTPUT);
    delay(50);
    digitalWrite(BRC_PIN, LOW);
    delay(50);
    digitalWrite(BRC_PIN, HIGH);
    delay(200);
    digitalWrite(BRC_PIN, LOW);
    delay(50);
    pinMode(BRC_PIN, INPUT);
    delay(50);
    roomba.start();
    delay(200);
  }
  #endif
  set_the_song();
}

void wakeOnDock() {
#ifndef ROOMBA_500
  DLOG("Wakeup Roomba on dock\n");
  wakeup();
#ifdef ROOMBA_650_SLEEP_FIX
  // Some black magic from @AndiTheBest to keep the Roomba awake on the dock
  // See https://github.com/johnboiles/esp-roomba-mqtt/issues/3#issuecomment-402096638
  delay(10);
  Serial.write(135); // Clean
  delay(150);
  Serial.write(143); // Dock
#endif
#endif
}

void wakeOffDock() {
  DLOG("Wakeup Roomba off Dock\n");
  Serial.write(131); // Safe mode
  delay(300);
  Serial.write(130); // Passive mode
}

// stop current action
void roombaStop() {
  if (roombaState.cleaning) {
    DLOG("Stopping\n");
    roomba.start();
    delay(50);
    roomba.safeMode();
    delay(50);
    roomba.start();
    delay(50);
  }
}

bool performCommand(const char *cmdchar) {
  wakeup();

  // Char* string comparisons dont always work
  String cmd(cmdchar);

  // MQTT protocol commands
  if (cmd == "turn_on") {
    DLOG("Turning on\n");
    roombaStop();
    roomba.cover();
    roombaState.cleaning = true;
  } else if (cmd == "turn_off") {
    DLOG("Turning off\n");
    roomba.power();
    roombaState.cleaning = false;
  } else if (cmd == "toggle" || cmd == "start" || cmd == "start_pause") {
    DLOG("Toggling\n");
    roomba.cover();
    roombaState.cleaning = !roombaState.cleaning;
  } else if (cmd == "stop") {
#ifdef ROOMBA_500
    roombaStop();
    roombaState.cleaning = false;
#else
    if (roombaState.cleaning) {
      DLOG("Stopping\n");
      roomba.cover();
    } else {
      DLOG("Not cleaning, can't stop\n");
    }
#endif
  } else if (cmd == "clean_spot") {
    DLOG("Cleaning Spot\n");
    roombaState.cleaning = true;
    roombaStop();
    roomba.spot();
  } else if (cmd == "locate") {
    DLOG("Locating\n");
    roomba.safeMode();
    delay(50);
    roomba.playSong(0);
    lastSong = millis();
  } else if (cmd == "return_to_base") {
    DLOG("Returning to Base\n");
    roombaStop();
    roombaState.cleaning = true;
    roomba.dock();
  } else {
    return false;
  }
  return true;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  DLOG("Received mqtt callback for topic %s\n", topic);
  if (strcmp(commandTopic, topic) == 0) {
    // turn payload into a null terminated string
    char *cmd = (char *)malloc(length + 1);
    memcpy(cmd, payload, length);
    cmd[length] = 0;

    if(!performCommand(cmd)) {
      DLOG("Unknown command %s\n", cmd);
    }
    free(cmd);
  }
}

#ifdef ENABLE_ADC_SLEEP
float readADC(int samples) {
  // Basic code to read from the ADC
  int adc = 0;
  for (int i = 0; i < samples; i++) {
    delay(1);
    adc += analogRead(A0);
  }
  adc = adc / samples;
  float mV = adc * ADC_VOLTAGE_DIVIDER;
  VLOG("ADC for %d is %.1fmV with %d samples\n", adc, mV, samples);
  return mV;
}
#endif

void debugCallback() {
  String cmd = Debug.getLastCommand();

  // Debugging commands via telnet
  if (performCommand(cmd.c_str())) {
  } else if (cmd == "quit") {
    DLOG("Stopping Roomba\n");
    Serial.write(173);
  } else if (cmd == "dstart") {
    Serial.write(128);
    delay(50);
    Serial.write(131);
    delay(50);
    Serial.write(135);
    mqttClient.publish("grond/status", "Cleaning");
  } else if (cmd == "set115200") {
    roomba.baud(Roomba::Baud115200);
    DLOG("Set Roomba baud to 115200\n");
  } else if (cmd == "rreset") {
    DLOG("Resetting Roomba\n");
    roomba.reset();
  } else if (cmd == "setsong") {
    uint8_t jaws[16] = {40,32,41,32,40,32,41,32,40,32,41,32,40,32,41,32};
    roomba.safeMode();
    delay(50);
    roomba.song(0, jaws, 16);
    DLOG("Roomba can sing\n");
  } else if (cmd == "setsong1") {
    uint8_t jaws[16] = {52,32,53,32,52,32,53,32,52,32,53,32,52,32,53,32};
    roomba.start();
    delay(50);
    roomba.song(0, jaws, 16);
    DLOG("Roomba can sing\n");
  } else if (cmd == "setsong2") {
    uint8_t dur = 32;
    uint8_t lotr[32] =
      {53, dur, 50, dur, 52, dur, 48, dur, 50, dur, 46, dur, 48, dur, 45, dur,
       53, dur, 50, dur, 52, dur, 48, dur, 50, dur, 46, dur, 48, dur, 45, dur};
    roomba.start();
    delay(50);
    roomba.song(0, lotr, 32);
    DLOG("Roomba can sing\n");
  } else if (cmd == "playsong") {
    DLOG("Singing song\n");
    roomba.safeMode();
    delay(50);
    roomba.playSong(0);
    lastSong = millis();
  } else if (cmd == "mqtthello") {
    mqttClient.publish("grond/hello", "hello there");
  } else if (cmd == "version") {
    const char compile_date[] = __DATE__ " " __TIME__;
    DLOG("Compiled on: %s\n", compile_date);
  } else if (cmd == "baud115200") {
    DLOG("Setting baud to 115200\n");
    Serial.begin(115200);
    delay(100);
  } else if (cmd == "baud19200") {
    DLOG("Setting baud to 19200\n");
    Serial.begin(19200);
    delay(100);
  } else if (cmd == "baud57600") {
    DLOG("Setting baud to 57600\n");
    Serial.begin(57600);
    delay(100);
  } else if (cmd == "baud38400") {
    DLOG("Setting baud to 38400\n");
    Serial.begin(38400);
    delay(100);
  } else if (cmd == "sleep5") {
    DLOG("Going to sleep for 5 seconds\n");
    delay(100);
    ESP.deepSleep(5e6);
  } else if (cmd == "wake") {
    DLOG("Toggle BRC pin\n");
    wakeup();
  } else if (cmd == "readadc") {
#ifdef ENABLE_ADC_SLEEP
    float adc = readADC(10);
    DLOG("ADC voltage is %.1fmV\n", adc);
#endif
  } else if (cmd == "streamresume") {
    DLOG("Resume streaming\n");
    roomba.streamCommand(Roomba::StreamCommandResume);
  } else if (cmd == "streampause") {
    DLOG("Pause streaming\n");
    roomba.streamCommand(Roomba::StreamCommandPause);
  } else if (cmd == "stream") {
    DLOG("Requesting stream\n");
    roomba.stream(sensors, sizeof(sensors));
  } else if (cmd == "streamreset") {
    DLOG("Resetting stream\n");
    roomba.stream({}, 0);
  } else {
    DLOG("Unknown command %s\n", cmd.c_str());
  }
}

void sleepIfNecessary() {
#ifdef ENABLE_ADC_SLEEP
  // Check the battery, if it's too low, sleep the ESP (so we don't murder the battery)
  float mV = readADC(10);
  // According to this post, you want to stop using NiMH batteries at about 0.9V per cell
  // https://electronics.stackexchange.com/a/35879 For a 12 cell battery like is in the Roomba,
  // That's 10.8 volts.
  if (mV < 10800) {
    // Fire off a quick message with our most recent state, if MQTT is connected
    DLOG("Battery voltage is low (%.1fV). Sleeping for 10 minutes\n", mV / 1000);
    if (mqttClient.connected()) {
      StaticJsonBuffer<200> jsonBuffer;
      JsonObject& root = jsonBuffer.createObject();
      root["battery_level"] = 0;
      root["cleaning"] = false;
      root["docked"] = false;
      root["charging"] = false;
      root["voltage"] = mV / 1000;
      root["charge"] = 0;
      String jsonStr;
      root.printTo(jsonStr);
      mqttClient.publish(statusTopic, jsonStr.c_str(), true);
    }
    delay(200);

    // Sleep for 10 minutes
    ESP.deepSleep(600e6);
  }
#endif
}

bool parseRoombaStateFromStreamPacket(uint8_t *packet, int length, RoombaState *state) {
  state->timestamp = millis();
  int i = 0;
  while (i < length) {
    switch(packet[i]) {
      case Roomba::Sensors7to26: // 0
        i += 27;
        break;
      case Roomba::Sensors7to16: // 1
        i += 11;
        break;
      case Roomba::SensorVirtualWall: // 13
        i += 2;
        break;
      case Roomba::SensorDirtDetect: // 15
        state->dirt = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorDistance: // 19
        state->distance = packet[i+1] * 256 + packet[i+2];
        total_distance += state->distance;
        i += 3;
        break;
      case Roomba::SensorChargingState: // 21
        state->chargingState = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorVoltage: // 22
        state->voltage = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorCurrent: // 23
        state->current = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBatteryCharge: // 25
        state->charge = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBatteryCapacity: //26
        state->capacity = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorVelocity: //39
        state->velocity = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBumpsAndWheelDrops: // 7
        i += 2;
        break;
      case 128: // Unknown
        i += 2;
        break;
      default:
        VLOG("Unhandled Packet ID %d\n", packet[i]);
        return false;
        break;
    }
  }
  return true;
}

void verboseLogPacket(uint8_t *packet, uint8_t length) {
    VLOG("Packet: ");
    for (int i = 0; i < length; i++) {
      VLOG("%d ", packet[i]);
    }
    VLOG("\n");
}

void readSensorPacket() {
  uint8_t packetLength;
  bool received = roomba.pollSensors(roombaPacket, sizeof(roombaPacket), &packetLength);
  if (received) {
    RoombaState rs = {};
    bool parsed = parseRoombaStateFromStreamPacket(roombaPacket, packetLength, &rs);
    verboseLogPacket(roombaPacket, packetLength);
    if (parsed) {
      roombaState = rs;
      VLOG("Got Packet of len=%d! Dirt:%d Distance:%dmm Velocity:%dmm/s ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh TotalDistance:%dmm\n", packetLength, roombaState.dirt, roombaState.distance, roombaState.velocity, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity, total_distance);
      roombaState.cleaning = false;
      roombaState.docked = false;
      if (roombaState.current < -400) {
        roombaState.cleaning = true;
      } else if (roombaState.current > -50) {
        roombaState.docked = true;
      }
    } else {
      VLOG("Failed to parse packet\n");
    }
  }
}

void onOTAStart() {
  DLOG("Starting OTA session\n");
  DLOG("Pause streaming\n");
  roomba.streamCommand(Roomba::StreamCommandPause);
  OTAStarted = true;
}

void setup() {
  // High-impedence on the BRC_PIN
  pinMode(BRC_PIN, INPUT);

  // Sleep immediately if ENABLE_ADC_SLEEP and the battery is low
  sleepIfNecessary();

  // Set Hostname.
  String hostname(HOSTNAME);
  WiFi.hostname(hostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  ArduinoOTA.setHostname((const char *)hostname.c_str());
  ArduinoOTA.begin();
  ArduinoOTA.onStart(onOTAStart);

  mqttClient.setServer(MQTT_SERVER, 1883);
  mqttClient.setCallback(mqttCallback);

  #if LOGGING
  Debug.begin((const char *)hostname.c_str());
  Debug.setResetCmdEnabled(true);
  Debug.setCallBackProjectCmds(debugCallback);
  Debug.setSerialEnabled(false);
  #endif

  roomba.start();
  delay(100);

  // Reset stream sensor values
  roomba.stream({}, 0);
  delay(100);

  delay(1000);
  set_the_song();
  // Request sensor stream
  roomba.stream(sensors, sizeof(sensors));
}

void reconnect() {
  DLOG("Attempting MQTT connection...\n");
  // Attempt to connect
  if (mqttClient.connect(HOSTNAME, MQTT_USER, MQTT_PASSWORD)) {
    DLOG("MQTT connected\n");
    mqttClient.subscribe(commandTopic);
  } else {
    DLOG("MQTT failed rc=%d try again in 5 seconds\n", mqttClient.state());
  }
}

int now_battery = 0;
int last_battery = 0;
int last_current = 0;
RoombaState lastState = {};
void sendStatus() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending status\n");
    return;
  }
  DLOG("Reporting packet Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  now_battery = (roombaState.charge * 100)/ (roombaState.capacity != 0
                                             ? roombaState.capacity : 2696);
  if (roombaState.cleaning) {
    root["state"] = "cleaning";
    if (!lastState.cleaning) total_distance = 0;
  } else if (roombaState.chargingState == Roomba::ChargeStateFullCharging) {
    root["state"] = "returning";
  } else if (roombaState.docked) {
    root["state"] = "docked";
  } else if ( roombaState.chargingState == Roomba::ChargeStateReconditioningCharging
              || roombaState.chargingState == Roomba::ChargeStateTrickleCharging) {
    root["state"] = "docked";
  } else {
    root["state"] = "idle";
  }
  root["battery_level"] = now_battery;
  if ((last_battery != now_battery) ||
      // Absolute change in current of at least 200
      (last_current - roombaState.current > 200) ||
      (roombaState.current - last_current > 200) ||
      (lastState.cleaning != roombaState.cleaning) ||
      (lastState.chargingState != roombaState.chargingState) ||
      (lastState.docked != roombaState.docked))
  {
    String jsonStr;
    root.printTo(jsonStr);
    mqttClient.publish(statusTopic, jsonStr.c_str());
    JsonObject& attr = jsonBuffer.createObject();
    attr["dirt"] = roombaState.dirt;
    attr["voltage"] = roombaState.voltage;
    attr["current"] = roombaState.current;
    attr["charge"] = roombaState.charge;
    attr["distance"] = roombaState.distance;
    attr["total_distance"] = total_distance;
    switch(roombaState.chargingState) {
      case Roomba::ChargeStateNotCharging: // 0
        attr["chargingState"] = "Not Charging";
        break;

      case Roomba::ChargeStateReconditioningCharging: // 0
        attr["chargingState"] = "Recondition Charging";
        break;

      case Roomba::ChargeStateFullCharging: // 0
        attr["chargingState"] = "Full Charging";
        break;

      case Roomba::ChargeStateTrickleCharging: // 0
        attr["chargingState"] = "Trickle Charging";
        break;

      case Roomba::ChargeStateWaiting: // 0
        attr["chargingState"] = "Waiting";
        break;

      case Roomba::ChargeStateFault: // 0
        attr["chargingState"] = "Charging Fault";
        break;

      default:
        VLOG("Unhandled Charging State %d\n", roombaState.chargingState);
        attr["chargingState"] = "Unknown";
        break;
    }
    attr["capacity"] = roombaState.capacity;
    attr["velocity"] = roombaState.velocity;
    String jsonStr2;
    attr.printTo(jsonStr2);
    mqttClient.publish(attributesTopic, jsonStr2.c_str());
  }
  last_battery = now_battery;
  last_current = roombaState.current;
  lastState.cleaning = roombaState.cleaning;
  lastState.chargingState = roombaState.chargingState;
  lastState.docked = roombaState.docked;
}

long lastStateMsgTime = 0;
long lastWakeupTime = 0;
long lastConnectTime = 0;
#ifdef ROOMBA_500
  // Wake up every 5 hours. Just to update battery level.
  long wakeupInterval = 18e7;
#else
  long wakeupInterval = 50000;
#endif

void loop() {
  // Important callbacks that _must_ happen every cycle
  ArduinoOTA.handle();
  yield();
  Debug.handle();

  // Skip all other logic if we're running an OTA update
  if (OTAStarted) {
    return;
  }

  long now = millis();
  // If MQTT client can't connect to broker, then reconnect
  if (!mqttClient.connected() && (now - lastConnectTime) > 5000) {
    DLOG("Reconnecting MQTT\n");
    lastConnectTime = now;
    reconnect();
  }
  // Check after singing
  if (lastSong > 0 && now - lastSong > 17000) {
    roomba.start();
    lastSong = 0;
  }
  // Wakeup the roomba at fixed intervals
  if (now - lastWakeupTime > wakeupInterval) {
    lastWakeupTime = now;
    if (!roombaState.cleaning) {
      if (roombaState.docked) {
        wakeOnDock();
      } else {
        // wakeOffDock();
        wakeup();
      }
    } else {
      wakeup();
    }
  }
  // Report the status over mqtt at fixed intervals
  if (now - lastStateMsgTime > 10000) {
    lastStateMsgTime = now;
    if (now - roombaState.timestamp > 30000 || roombaState.sent) {
      DLOG("Roomba state already sent (%.1fs old)\n", (now - roombaState.timestamp)/1000.0);
      DLOG("Request stream\n");
      roomba.stream(sensors, sizeof(sensors));
    } else {
      sendStatus();
      roombaState.sent = true;
    }
    sleepIfNecessary();
  }

  readSensorPacket();
  mqttClient.loop();
}
