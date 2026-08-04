/*
 * secrets.h
 *
 * The four wificom.dev values come from registering an account and adding
 * a device at https://wificom.dev. They are issued by the site; you cannot
 * generate them locally.
 */

#ifndef SECRETS_H
#define SECRETS_H

// MQTT_USERNAME is your wificom.dev site username. The reference firmware
// lowercases it before using it for both auth and topic names, so store it
// lowercase here to match.
#define MQTT_USERNAME   "[wificom.dev username]"
#define MQTT_PASSWORD   "[wificom.dev generated password]"
#define USER_UUID       "[wificon.dev generated USER_UUID]"
#define DEVICE_UUID     "[wificom.dev generated DEVICE_UUID]"

// Broker. Port 8883 is MQTT over TLS.
#define MQTT_BROKER     "mqtt.wificom.dev"
#define MQTT_PORT       8883

#endif
