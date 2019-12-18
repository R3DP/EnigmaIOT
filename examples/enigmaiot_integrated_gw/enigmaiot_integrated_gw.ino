/**
  * @file enigmaiot_gateway.ino
  * @version 0.6.0
  * @date 17/11/2019
  * @author German Martin
  * @brief Gateway based on EnigmaIoT over ESP-NOW
  *
  * Communicates with a serial to MQTT gateway to send data to any IoT platform
  */

#ifdef ESP32
#include "GwOutput_mqtt.h"
#include <WiFi.h> // Comment to compile for ESP8266
#include <AsyncTCP.h> // Comment to compile for ESP8266
#include <Update.h>
#include <SPIFFS.h>
#include "esp_system.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
//#include <ESPAsyncTCP.h> // Comment to compile for ESP32
#include <Hash.h>
#include <SPI.h>
#include <PubSubClient.h>
#ifdef SECURE_MQTT
#include <WiFiClientSecure.h>
#else
#include <WiFiClient.h>
#endif // SECURE_MQTT
#endif // ESP32


#include <Arduino.h>
#include <CayenneLPP.h>
#include <FS.h>

#include <EnigmaIOTGateway.h>
#include "lib/helperFunctions.h"
#include "lib/debug.h"
#include "espnow_hal.h"

#include <Curve25519.h>
#include <ChaChaPoly.h>
#include <Poly1305.h>
#include <SHA256.h>
#include <CRC32.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include "GwOutput_generic.h"

#ifndef BUILTIN_LED
#define BUILTIN_LED 5
#endif // BUILTIN_LED

#define BLUE_LED BUILTIN_LED
#define RED_LED BUILTIN_LED

#ifdef ESP32
TimerHandle_t connectionLedTimer;
#elif defined(ESP8266)
ETSTimer connectionLedTimer;
#endif // ESP32

const int connectionLed = BUILTIN_LED;
boolean connectionLedFlashing = false;

void flashConnectionLed (void* led) {
	//digitalWrite (*(int*)led, !digitalRead (*(int*)led));
	digitalWrite (BUILTIN_LED, !digitalRead (BUILTIN_LED));
}

void startConnectionFlash (int period) {
#ifdef ESP32
	if (!connectionLedFlashing) {
		connectionLedFlashing = true;
		connectionLedTimer = xTimerCreate ("led_flash", pdMS_TO_TICKS (period), pdTRUE, (void*)0, flashConnectionLed);
		xTimerStart (connectionLedTimer, 0);
	}
#elif defined (ESP8266)
	ets_timer_disarm (&connectionLedTimer);
	if (!connectionLedFlashing) {
		connectionLedFlashing = true;
		ets_timer_arm_new (&connectionLedTimer, period, true, true);
	}
#endif // ESP32
}

void stopConnectionFlash () {
#ifdef ESP32
	if (connectionLedFlashing) {
		connectionLedFlashing = false;
		xTimerStop (connectionLedTimer, 0);
		xTimerDelete (connectionLedTimer, 0);
	}
#elif defined(ESP8266)
	if (connectionLedFlashing) {
		connectionLedFlashing = false;
		ets_timer_disarm (&connectionLedTimer);
		digitalWrite (connectionLed, HIGH);
	}
#endif // ESP32
}

void wifiManagerExit (boolean status) {
	GwOutput.configManagerExit (status);
}

void wifiManagerStarted () {
	GwOutput.configManagerStart (&EnigmaIOTGateway);
}

void processRxControlData (char* macStr, uint8_t* data, uint8_t length) {
	if (data) {
		GwOutput.outputControlSend (macStr, data, length);
	}
}

void processRxData (uint8_t* mac, uint8_t* buffer, uint8_t length, uint16_t lostMessages, bool control) {
	uint8_t *addr = mac;
	const int capacity = JSON_ARRAY_SIZE (25) + 25 * JSON_OBJECT_SIZE (4);
	DynamicJsonDocument jsonBuffer (capacity);
	//StaticJsonDocument<capacity> jsonBuffer;
	JsonArray root = jsonBuffer.createNestedArray ();
	CayenneLPP *cayennelpp = new CayenneLPP(MAX_DATA_PAYLOAD_SIZE);

	char mac_str[18];
	mac2str (addr, mac_str);
	if (control) {
		processRxControlData (mac_str, buffer, length);
		return;
	}

	char* netName = EnigmaIOTGateway.getNetworkName ();
	const int PAYLOAD_SIZE = 512;
	char* payload = (char*)malloc (PAYLOAD_SIZE);

	cayennelpp->decode ((uint8_t *)buffer, length, root);
	cayennelpp->CayenneLPP::~CayenneLPP();
	free (cayennelpp);
	
	size_t pld_size = serializeJson (root, payload, PAYLOAD_SIZE);
	GwOutput.outputDataSend (mac_str, payload, pld_size);
	DEBUG_INFO ("Published data message from %s: %s", mac_str, payload);
	if (lostMessages > 0) {
		pld_size = snprintf (payload, PAYLOAD_SIZE, "%u", lostMessages);
		GwOutput.outputDataSend (mac_str, payload, pld_size, GwOutput_data_type::lostmessages);
		DEBUG_INFO ("Published MQTT from %s: %s", mac_str, payload);
	}
	pld_size = snprintf (payload, PAYLOAD_SIZE, "{\"per\":%e,\"lostmessages\":%u,\"totalmessages\":%u,\"packetshour\":%.2f}",
			  EnigmaIOTGateway.getPER ((uint8_t*)mac),
			  EnigmaIOTGateway.getErrorPackets ((uint8_t*)mac),
			  EnigmaIOTGateway.getTotalPackets ((uint8_t*)mac),
			  EnigmaIOTGateway.getPacketsHour ((uint8_t*)mac));
	GwOutput.outputDataSend (mac_str, payload, pld_size, GwOutput_data_type::status);
	DEBUG_INFO ("Published MQTT from %s: %s", mac_str, payload);

	free (payload);
}

control_message_type_t checkMsgType (String data) {
	if (data.indexOf (GET_VERSION) != -1) {
		return control_message_type::VERSION;
	} else
	if (data.indexOf (GET_SLEEP) != -1) {
		return control_message_type::SLEEP_GET;
	} else
	if (data.indexOf (SET_SLEEP) != -1) {
		return control_message_type::SLEEP_SET;
	} else
	if (data.indexOf (SET_OTA) != -1) {
		return control_message_type::OTA;
	} else
	if (data.indexOf (SET_IDENTIFY) != -1) {
		DEBUG_WARN ("IDENTIFY MESSAGE %s", data.c_str ());
		return control_message_type::IDENTIFY;
	} else
	if (data.indexOf (SET_RESET_CONFIG) != -1) {
		DEBUG_WARN ("RESET CONFIG MESSAGE %s", data.c_str ());
		return control_message_type::RESET;
	}
	if (data.indexOf (GET_RSSI) != -1) {
		DEBUG_INFO ("GET RSSI MESSAGE %s", data.c_str ());
		return control_message_type::RSSI_GET;
	}
	return control_message_type::USERDATA;
}

// TODO
void onDownlinkData (const char* topic, size_t topic_len, char* payload, size_t len){
	DEBUG_INFO ("DL Topic: %.*s", topic_len, topic);
	//DEBUG_INFO ("DL Payload: %.*s", len, payload);
}

void onSerial (String message) {
	uint8_t addr[6];

	DEBUG_VERBOSE ("Downlink message: %s", message.c_str ());
	String addressStr = message.substring (message.indexOf ('/') + 1, message.indexOf ('/', 2));
	DEBUG_INFO ("Downlink message to: %s", addressStr.c_str ());
	if (!str2mac (addressStr.c_str (), addr)) {
		DEBUG_ERROR ("Not a mac address");
		return;
	}
	String dataStr = message.substring (message.indexOf ('/', 2) + 1);
	dataStr.trim ();

	control_message_type_t msgType = checkMsgType (dataStr);

	// Add end of string to all control messages
	if (msgType != control_message_type::USERDATA) {
		dataStr = dataStr.substring (dataStr.indexOf (';') + 1);
		dataStr += '\0';
	}

	DEBUG_VERBOSE ("Message %s", dataStr.c_str ());
	DEBUG_INFO ("Message type %d", msgType);
	DEBUG_INFO ("Data length %d", dataStr.length ());
	if (!EnigmaIOTGateway.sendDownstream (addr, (uint8_t*)dataStr.c_str (), dataStr.length (), msgType)) {
		DEBUG_ERROR ("Error sending esp_now message to %s", addressStr.c_str ());
	}
	else {
		DEBUG_DBG ("Esp-now message sent or queued correctly");
	}
}

void newNodeConnected (uint8_t * mac) {
	char macstr[18];
	mac2str (mac, macstr);
	//Serial.printf ("New node connected: %s\n", macstr);

	if (!GwOutput.newNodeSend (macstr)) {
		DEBUG_WARN ("Error senfing new node %s", macstr);
	} else {
		DEBUG_DBG ("New node %s message sent", macstr);
	}
	//Serial.printf ("~/%s/hello\n", macstr);
}

void nodeDisconnected (uint8_t * mac, gwInvalidateReason_t reason) {
	char macstr[18];
	mac2str (mac, macstr);
	//Serial.printf ("Node %s disconnected. Reason %u\n", macstr, reason);
	if (!GwOutput.nodeDisconnectedSend (macstr, reason)) {
		DEBUG_WARN ("Error sending node disconnected %s reason %d", macstr, reason);
	} else {
		DEBUG_DBG ("Node %s disconnected message sent. Reason %d", macstr, reason);
	}
	//Serial.printf ("~/%s/bye;{\"reason\":%u}\n", macstr, reason);
}

#ifdef ESP32
void EnigmaIOTGateway_handle (void * param) {
	for (;;) {
		EnigmaIOTGateway.handle ();
		vTaskDelay (0);
	}
}

TaskHandle_t xEnigmaIOTGateway_handle = NULL;
#endif // ESP32

void setup () {
	Serial.begin (115200); Serial.println (); Serial.println ();
#ifdef ESP8266
	ets_timer_setfn (&connectionLedTimer, flashConnectionLed, (void*)&connectionLed);
#elif defined ESP32
	
#endif
	pinMode (BUILTIN_LED, OUTPUT);
	digitalWrite (BUILTIN_LED, HIGH);
	startConnectionFlash (100);


	if (!GwOutput.loadConfig ()) {
		DEBUG_WARN ("Error reading config file");
	}

	EnigmaIOTGateway.setRxLed (BLUE_LED);
	EnigmaIOTGateway.setTxLed (RED_LED);
	EnigmaIOTGateway.onNewNode (newNodeConnected);
	EnigmaIOTGateway.onNodeDisconnected (nodeDisconnected);
	EnigmaIOTGateway.onWiFiManagerStarted (wifiManagerStarted);
	EnigmaIOTGateway.onWiFiManagerExit (wifiManagerExit);
	EnigmaIOTGateway.onDataRx (processRxData);
	EnigmaIOTGateway.begin (&Espnow_hal);

	WiFi.mode (WIFI_AP_STA);
	WiFi.begin ();

	EnigmaIOTGateway.configWiFiManager ();

	WiFi.softAP (EnigmaIOTGateway.getNetworkName (), EnigmaIOTGateway.getNetworkKey());
	stopConnectionFlash ();

	DEBUG_INFO ("STA MAC Address: %s", WiFi.macAddress ().c_str());
	DEBUG_INFO ("AP MAC Address: %s", WiFi.softAPmacAddress().c_str ());
	DEBUG_INFO ("BSSID Address: %s", WiFi.BSSIDstr().c_str ());
	   
	DEBUG_INFO ("IP address: %s", WiFi.localIP ().toString ().c_str ());
	DEBUG_INFO ("WiFi Channel: %d", WiFi.channel ());
	DEBUG_INFO ("WiFi SSID: %s", WiFi.SSID ().c_str ());
	DEBUG_INFO ("Network Name: %s", EnigmaIOTGateway.getNetworkName ());

	GwOutput.setDlCallback (onDownlinkData);
	GwOutput.begin ();

#ifdef ESP32
	//xTaskCreate (EnigmaIOTGateway_handle, "handle", 10000, NULL, 1, &xEnigmaIOTGateway_handle);
	xTaskCreatePinnedToCore (EnigmaIOTGateway_handle, "handle", 4096, NULL, 1, &xEnigmaIOTGateway_handle, 1);
	//xTaskCreatePinnedToCore (client_reconnect, "reconnect", 10000, NULL, 1, &xClient_reconnect, 1);
#endif
	}

void loop () {
	//delay (1);
#ifdef ESP8266
	client.loop ();
#endif

	//String message;
	//if (client.connected ()) {
	//}

#ifdef ESP8266
	EnigmaIOTGateway.handle ();

	if (!client.connected ()) {
		DEBUG_INFO ("reconnect");
		reconnect ();
	}
#endif

	/*while (Serial.available () != 0) {
		message = Serial.readStringUntil ('\n');
		message.trim ();
		if (message[0] == '%') {
			onSerial (message);
		}
	}*/

}