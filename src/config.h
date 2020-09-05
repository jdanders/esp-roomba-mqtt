#include "secrets.h"

#define HOSTNAME "grond" // e.g. roomba.local
#define BRC_PIN 2
//#define ROOMBA_650_SLEEP_FIX 1
#ifndef ROOMBA_650_SLEEP_FIX
#define ROOMBA_500 1
#endif

#define ADC_VOLTAGE_DIVIDER 44.551316985
//#define ENABLE_ADC_SLEEP

#define MQTT_SERVER "10.112.12.64"
#define MQTT_USER "mqttuser"
#define MQTT_COMMAND_TOPIC "grond/command"
#define MQTT_STATE_TOPIC "grond/state"
#define MQTT_ATTRIBUTES_TOPIC "grond/attributes"
