#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <NTPClient.h>
#include <AltSerialGraphicLCD.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <ArduinoOTA.h>

#include "config.h"

// Set web server port number to 80
WiFiServer webServer(80);

// Variable to store the HTTP request
String header;

unsigned int totalFed = 0;
unsigned int toFeed = 0;
unsigned int feedAmount = 2;

unsigned long lastFed = 0;
unsigned long lastScreenUpdate = 0;

const byte numChars = 32;
char receivedChars[numChars]; // an array to store the received data

boolean newData = false;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

#define SERIAL_TX_DPIN   D1
#define SERIAL_RX_DPIN   D2

// Initialize an instance of the SoftwareSerial library
SoftwareSerial serial(SERIAL_RX_DPIN,SERIAL_TX_DPIN);

// Create an instance of the GLCD class named glcd. This instance is used to
// call all the subsequent GLCD functions. The instance is called with a
// reference to the software serial object.
GLCD glcd(serial);

char buffer[22]; // Character buffer for strings

#if defined(DEBUG_TELNET)
WiFiServer  telnetServer(23);
WiFiClient  telnetClient;
#define     DEBUG_PRINT(x)    telnetClient.print(x)
#define     DEBUG_PRINTLN(x)  telnetClient.println(x)
#elif defined(DEBUG_SERIAL)
#define     DEBUG_PRINT(x)    Serial.print(x)
#define     DEBUG_PRINTLN(x)  Serial.println(x)
#else
#define     DEBUG_PRINT(x)
#define     DEBUG_PRINTLN(x)
#endif

#if defined(GRAPHITE)
WiFiUDP graphiteUDP;
unsigned long lastGraphiteUpdate = 0;
#endif

///////////////////////////////////////////////////////////////////////////
//   TELNET
///////////////////////////////////////////////////////////////////////////
/*
   Function called to handle Telnet clients
   https://www.youtube.com/watch?v=j9yW10OcahI
*/
#if defined(DEBUG_TELNET)
void handleTelnet(void) {
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) {
        telnetClient.stop();
      }
      telnetClient = telnetServer.available();
    } else {
      telnetServer.available().stop();
    }
  }
}
#endif

///////////////////////////////////////////////////////////////////////////
//   WiFi
///////////////////////////////////////////////////////////////////////////

/*
   Function called to setup the connection to the WiFi AP
*/
void setupWiFi() {
  updateLine2("WiFi Connecting...    ");

  DEBUG_PRINT("Connecting to ");
  DEBUG_PRINT(WIFI_SSID);
  DEBUG_PRINT("...");

  WiFi.hostname(HA_ID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    DEBUG_PRINT(".");
  }
  DEBUG_PRINTLN("connected");

  // Print local IP address
  DEBUG_PRINT("IP address: ");
  DEBUG_PRINTLN(WiFi.localIP());

  updateLine2("WiFi Connected        ");

  randomSeed(micros());
}

///////////////////////////////////////////////////////////////////////////
//   MQTT
///////////////////////////////////////////////////////////////////////////

String mqttTopic(const char* topic) {
  return String("homeassistant/light/") + String(HA_ID) + String("/") + String(topic);
}

void mqttRegister() {
  StaticJsonBuffer<400> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();

  root["name"] = HA_NAME;
  root["schema"] = "json";
  root["unique_id"] = HA_ID;
  root["command_topic"] = mqttTopic("set");
  root["brightness"] = true;
  root["state_topic"] = mqttTopic("state");

  String output;
  root.printTo(output);

  boolean result = mqttClient.beginPublish(mqttTopic("config").c_str(), output.length(), true);
  if (result) {
    result = mqttClient.write((unsigned char*)output.c_str(), output.length());
  }
  if (result) {
    result = mqttClient.endPublish();
  }
  if (result) {
    DEBUG_PRINTLN("registered");
  } else {
    DEBUG_PRINTLN("registration failed");
  }
}

void mqttPublish(const char* topic, const char* msg) {
  mqttClient.publish(mqttTopic("state").c_str(), msg);
}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    DEBUG_PRINT("Attempting MQTT connection...");
    // Attempt to connect
    if (mqttClient.connect(MQTT_CLIENTID, MQTT_USER, MQTT_PASS, mqttTopic("config").c_str(), 1, 1, "")) {
      DEBUG_PRINTLN("connected");
      // Once connected, publish an announcement...
      mqttRegister();
      // ... and resubscribe
      mqttClient.subscribe(mqttTopic("set").c_str());
    } else {
      DEBUG_PRINT("failed, rc=");
      DEBUG_PRINT(mqttClient.state());
      DEBUG_PRINTLN(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* p_payload, unsigned int p_length) {
  String payload;
  for (uint8_t i = 0; i < p_length; i++) {
    payload.concat((char)p_payload[i]);
  }

  DEBUG_PRINT("Message arrived [");
  DEBUG_PRINT(topic);
  DEBUG_PRINT("] (");
  DEBUG_PRINT(p_length);
  DEBUG_PRINT(") ");
  DEBUG_PRINTLN(payload);

  // feed cat if payload is "ON"
  StaticJsonBuffer<300> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(p_payload);
  if (!root.success()) {
    DEBUG_PRINTLN("ERROR: parseObject() failed");
    return;
  }

  if (root.containsKey("brightness") && root["brightness"].is<int>()) {
    feedAmount = (root["brightness"].as<int>() + 1) / 64;
  }

  if (root.containsKey("state") && root["state"].as<String>() == String("ON")) {
    toFeed += feedAmount;
  }
}

///////////////////////////////////////////////////////////////////////////
//   OTA
///////////////////////////////////////////////////////////////////////////
#if defined(OTA)
/*
   Function called to setup OTA updates
*/
void setupOTA() {
  ArduinoOTA.setHostname(HA_ID);
  DEBUG_PRINT(F("INFO: OTA hostname sets to: "));
  DEBUG_PRINTLN(HA_ID);

#if defined(OTA_PORT)
  ArduinoOTA.setPort(OTA_PORT);
  DEBUG_PRINT(F("INFO: OTA port sets to: "));
  DEBUG_PRINTLN(OTA_PORT);
#endif

#if defined(OTA_PASSWORD)
  ArduinoOTA.setPassword((const char *)OTA_PASSWORD);
  DEBUG_PRINT(F("INFO: OTA password sets to: "));
  DEBUG_PRINTLN(OTA_PASSWORD);
#endif

  ArduinoOTA.onStart([]() {
    DEBUG_PRINTLN(F("INFO: OTA starts"));
  });
  ArduinoOTA.onEnd([]() {
    DEBUG_PRINTLN(F("INFO: OTA ends"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    DEBUG_PRINT(F("INFO: OTA progresses: "));
    DEBUG_PRINT(progress / (total / 100));
    DEBUG_PRINTLN(F("%"));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    DEBUG_PRINT(F("ERROR: OTA error: "));
    DEBUG_PRINTLN(error);
    if (error == OTA_AUTH_ERROR)
      DEBUG_PRINTLN(F("ERROR: OTA auth failed"));
    else if (error == OTA_BEGIN_ERROR)
      DEBUG_PRINTLN(F("ERROR: OTA begin failed"));
    else if (error == OTA_CONNECT_ERROR)
      DEBUG_PRINTLN(F("ERROR: OTA connect failed"));
    else if (error == OTA_RECEIVE_ERROR)
      DEBUG_PRINTLN(F("ERROR: OTA receive failed"));
    else if (error == OTA_END_ERROR)
      DEBUG_PRINTLN(F("ERROR: OTA end failed"));
  });
  ArduinoOTA.begin();
}

/*
   Function called to handle OTA updates
*/
void handleOTA() {
  ArduinoOTA.handle();
}
#endif

///////////////////////////////////////////////////////////////////////////
//   LCD
///////////////////////////////////////////////////////////////////////////

String formatTime(unsigned long rawTime) {
  unsigned long localTime = rawTime + (TZ_OFFSET * 3600);

  unsigned long hours = (localTime % 86400L) / 3600;
  String hoursStr = hours < 10 ? "0" + String(hours) : String(hours);

  unsigned long minutes = (localTime % 3600) / 60;
  String minuteStr = minutes < 10 ? "0" + String(minutes) : String(minutes);

  unsigned long seconds = localTime % 60;
  String secondStr = seconds < 10 ? "0" + String(seconds) : String(seconds);

  return hoursStr + ":" + minuteStr + ":" + secondStr;
}

void initScreen(const char* msg) {
  serial.begin(115200);

  glcd.reset();

  updateTitle();
  updateLine2(msg);
}

void redrawScreen() {
  glcd.clearScreen();

  updateTitle();
  updateTime();
  updateTotalFed();
  updateLastFed();
}

void updateTitle() {
  glcd.setString(64, 0, GLCD_FONT_CENTER, HA_NAME);
  // snprintf(buffer, sizeof(buffer), "%s", HA_NAME);
  // glcd.setXY(0, 0);
  // glcd.printStr(buffer);
}

void updateTime() {
  snprintf(buffer, sizeof(buffer), "Time:        %s", formatTime(timeClient.getEpochTime()).c_str());
  glcd.setXY(0, 10);
  glcd.printStr(buffer);
}

void updateLine2(const char* content) {
  snprintf(buffer, sizeof(buffer), "%s", content);
  glcd.setXY(0, 10);
  glcd.printStr(buffer);
}

void updateLastFed() {
  const char* tmpl = "Last Fed:    %d%s ago    ";
  if (lastFed > 0) {
    unsigned long ago = timeClient.getEpochTime() - lastFed;
    if (ago < 60 * 60 * 20) {
      snprintf(buffer, sizeof(buffer), "Last Fed:    %s    ", formatTime(lastFed).c_str());
    } else if (ago < 60 * 60 * 48) {
      snprintf(buffer, sizeof(buffer), tmpl, ago / 60 / 60, "h");
    } else {
      snprintf(buffer, sizeof(buffer), tmpl, ago / 60 / 60 / 24, "d");
    }
  } else {
    snprintf(buffer, sizeof(buffer), "Not Fed Yet            ");
  }

  glcd.setXY(0, 20);
  glcd.printStr(buffer);
}

void updateTotalFed() {
  snprintf(buffer, sizeof(buffer), "Total Fed:   %d", totalFed);
  glcd.setXY(0, 30);
  glcd.printStr(buffer);
}

void updateFeeding(boolean feeding) {
  glcd.setXY(0, 40);
  if (feeding) {
    glcd.printStr("Feeding...");
  } else {
    glcd.printStr("          ");
  }
}

void updateError() {
  glcd.setXY(0, 40);
  glcd.printStr("ERROR!    ");
}

///////////////////////////////////////////////////////////////////////////
//   HTTP
///////////////////////////////////////////////////////////////////////////

void sendPageHeader(WiFiClient http_client) {
  // Display the HTML web page
  http_client.println("<!DOCTYPE html><html>");
  http_client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  http_client.println("<title>Cat Feeder</title>");
  http_client.println("<link rel=\"icon\" href=\"data:,\">");
  // CSS to style the on/off buttons
  // Feel free to change the background-color and font-size attributes to fit your preferences
  http_client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
  http_client.println(".button { background-color: #195B6A; border: none; color: white; padding: 16px 40px;");
  http_client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
  http_client.println(".button2 {background-color: #77878A;}</style></head>");

  // Web Page Heading
  http_client.println("<body><h1>Cat Feeder</h1>");
}

void sendPageFooter(WiFiClient http_client) {
  http_client.println("</body></html>");

  // The HTTP response ends with another blank line
  http_client.println();
}

void handleConnection(WiFiClient http_client) {
  // Serial.println("New Client.");          // print a message out in the serial port
  String currentLine = "";                // make a String to hold incoming data from the client
  while (http_client.connected()) {            // loop while the client's connected
    if (http_client.available()) {             // if there's bytes to read from the client,
      char c = http_client.read();             // read a byte, then
      // Serial.write(c);                    // print it out the serial monitor
      header += c;
      if (c == '\n') {                    // if the byte is a newline character
        // if the current line is blank, you got two newline characters in a row.
        // that's the end of the client HTTP request, so send a response:
        if (currentLine.length() == 0) {
          // turns the GPIOs on and off
          if (header.indexOf("GET /feed ") >= 0) {
            DEBUG_PRINTLN("Requested Feed");
            toFeed++;

            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            http_client.println("HTTP/1.1 302 Redirect");
            http_client.println("Location: /");
            http_client.println("Content-type: text/html");
            http_client.println("Connection: close");
            http_client.println();

            sendPageHeader(http_client);

            http_client.println("<p>Feeding</p>");

            sendPageFooter(http_client);

            // Break out of the while loop
            break;
          }

          if (header.indexOf("GET /reset ") >= 0) {
            DEBUG_PRINTLN("Requested Reset");

            lastFed = 0;
            SPIFFS.remove("/lastFed.txt");
            updateLastFed();

            totalFed = 0;
            SPIFFS.remove("/totalFed.txt");
            updateTotalFed();

            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            http_client.println("HTTP/1.1 302 Redirect");
            http_client.println("Location: /");
            http_client.println("Content-type: text/html");
            http_client.println("Connection: close");
            http_client.println();

            sendPageHeader(http_client);

            http_client.println("<p>Reset</p>");

            sendPageFooter(http_client);

            // Break out of the while loop
            break;
          }

          if (header.indexOf("GET /metrics ") >= 0) {
            DEBUG_PRINTLN("Requested Metrics");

            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            http_client.println("HTTP/1.1 200 OK");
            http_client.println("Content-type: text/plain; version=0.0.4");
            http_client.println("Connection: close");
            http_client.println();

            char mbuffer[100];

            snprintf(mbuffer, sizeof(mbuffer), "cat_feeder_total_fed{name=\"%s\"} %d", HA_ID, totalFed);
            http_client.println(mbuffer);

            snprintf(mbuffer, sizeof(mbuffer), "cat_feeder_last_fed{name=\"%s\"} %d", HA_ID, lastFed);
            http_client.println(mbuffer);

            // Break out of the while loop
            break;
          }

          // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
          // and a content-type so the client knows what's coming, then a blank line:
          http_client.println("HTTP/1.1 200 OK");
          http_client.println("Content-type: text/html");
          http_client.println("Connection: close");
          http_client.println();

          sendPageHeader(http_client);

          // Display current state, and FEED button
          http_client.println("<p>Total Fed " + String(totalFed) + "</p>");
          http_client.println("<p>Last Feed " + formatTime(lastFed) + "</p>");
          http_client.println("<p><a href=\"/feed\"><button class=\"button\">FEED</button></a></p>");

          sendPageFooter(http_client);

          // Break out of the while loop
          break;
        } else { // if you got a newline, then clear currentLine
          currentLine = "";
        }
      } else if (c != '\r') {  // if you got anything else but a carriage return character,
        currentLine += c;      // add it to the end of the currentLine
      }
    }
  }

  // Clear the header variable
  header = "";

  // Close the connection
  http_client.stop();

  // Serial.println("Client disconnected.");
  // Serial.println("");
}

///////////////////////////////////////////////////////////////////////////
//   Feeder
///////////////////////////////////////////////////////////////////////////

void feed(int numToFeed) {
  int numSegments = 0;
  int prevState = digitalRead(PIN_INPUT);
  int currState;

  updateFeeding(true);

  DEBUG_PRINT("Feeding ");
  DEBUG_PRINTLN(numToFeed);

  mqttPublish("state", (String("{\"state\":\"ON\",\"brightness\":") + String(numToFeed * 64) + String("}")).c_str());

  digitalWrite(PIN_OUTPUT, HIGH);

  unsigned long startTime = millis();

  while (true) {
    currState = digitalRead(PIN_INPUT);
    if (currState == LOW && prevState == HIGH) {
      totalFed++;
      numSegments++;

      if (numSegments >= numToFeed) {
        break;
      }

      updateTotalFed();
    }

    /*
    Serial.print("Prev ");
    Serial.print(prevState);
    Serial.print(" Curr ");
    Serial.print(currState);
    Serial.print(" Num ");
    Serial.print(numSegments);
    Serial.println();
    */

    // if the motor has been running too long without triggering the switch, assume there is a fault
    if (millis() - startTime > numToFeed * 1800) {
      digitalWrite(PIN_OUTPUT, LOW);

      updateError();

      DEBUG_PRINTLN("Error");

      return;
    }

    prevState = currState;

    delay(10);
  }

  digitalWrite(PIN_OUTPUT, LOW);

  lastFed = timeClient.getEpochTime();

  File f = SPIFFS.open("/lastFed.txt", "w");
  if (f) {
    DEBUG_PRINT("Updating lastFed.txt: ");
    DEBUG_PRINTLN(lastFed);
    f.print(lastFed);
    f.close();
  }

  f = SPIFFS.open("/totalFed.txt", "w");
  if (f) {
    DEBUG_PRINT("Updating totalFed.txt: ");
    DEBUG_PRINTLN(totalFed);
    f.print(totalFed);
    f.close();
  }

  redrawScreen();

  DEBUG_PRINT("Fed ");
  DEBUG_PRINTLN(numSegments);

  mqttPublish("state", "{\"state\":\"OFF\"}");
}

///////////////////////////////////////////////////////////////////////////
//   Setup
///////////////////////////////////////////////////////////////////////////

void setup() {
  pinMode(PIN_INPUT, INPUT_PULLUP); // enable internal pullup

  pinMode(PIN_OUTPUT, OUTPUT);
  digitalWrite(PIN_OUTPUT, LOW);

  delay(100);

  Serial.begin(74880);

#if defined(DEBUG_TELNET)
  telnetServer.begin();
  telnetServer.setNoDelay(true);
#endif

  DEBUG_PRINTLN("Cat Feeder 0.1");
  DEBUG_PRINTLN();

  initScreen("Starting...");

  yield();

  setupWiFi();

#if defined(OTA)
  setupOTA();
#endif

  yield();

  // Initialize a NTPClient to get time
  timeClient.begin();

  // update time
  while(!timeClient.update()) {
    yield();
    timeClient.forceUpdate();
  }

  yield();

  webServer.begin();

  yield();

  mqttClient.setServer(MQTT_SERVER, 1883);
  mqttClient.setCallback(mqttCallback);

  yield();

  SPIFFS.begin();

  yield();

  File f;

  f = SPIFFS.open("/lastFed.txt", "r");
  if (f) {
    DEBUG_PRINT("Reading lastFed.txt: ");
    lastFed = (unsigned long)f.parseFloat();
    f.close();
    DEBUG_PRINTLN(lastFed);
  }

  yield();

  f = SPIFFS.open("/totalFed.txt", "r");
  if (f) {
    DEBUG_PRINT("Reading totalFed.txt: ");
    totalFed = (unsigned int)f.parseInt();
    f.close();
    DEBUG_PRINTLN(totalFed);
  }

  yield();

  redrawScreen();
}

///////////////////////////////////////////////////////////////////////////
//   Loop
///////////////////////////////////////////////////////////////////////////

void loop() {
#if defined(OTA)
  handleOTA();
  yield();
#endif

#if defined(DEBUG_TELNET)
  // handle the Telnet connection
  handleTelnet();
  yield();
#endif

  // Handle web requests
  WiFiClient http_client = webServer.available();
  if (http_client) {
    handleConnection(http_client);
  }

  yield();

  // handle mqtt
  if (!mqttClient.connected()) {
    mqttReconnect();
  }

  yield();

  mqttClient.loop();

  yield();

  // update time
  while(!timeClient.update()) {
    yield();
    timeClient.forceUpdate();
  }

  yield();

  // update screen
  if (millis() / 1000 > lastScreenUpdate / 1000) {
    lastScreenUpdate =  millis();
    redrawScreen();
  }

  yield();

#if defined(GRAPHITE)
  unsigned long currTime = timeClient.getEpochTime();
  if (currTime / GRAPHITE_INTERVAL > lastGraphiteUpdate / GRAPHITE_INTERVAL) {
    graphiteUDP.beginPacket(GRAPHITE_HOST, GRAPHITE_PORT);
    char msgBuffer[100];
    snprintf(msgBuffer, sizeof(msgBuffer), "%s.%s.%s %d %d\n", GRAPHITE_PREFIX, HA_ID, "total_fed", totalFed, currTime);
    graphiteUDP.write(msgBuffer);
    snprintf(msgBuffer, sizeof(msgBuffer), "%s.%s.%s %d %d\n", GRAPHITE_PREFIX, HA_ID, "last_fed", lastFed, currTime);
    graphiteUDP.write(msgBuffer);
    graphiteUDP.endPacket();
    lastGraphiteUpdate = currTime;
  }
#endif

  // feed cat
  if (toFeed > 0) {
    feed(toFeed);
    toFeed = 0;
  }

  delay(10);
}

