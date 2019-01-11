#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <NTPClient.h>
#include <AltSerialGraphicLCD.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>

#include "config.h"

// Set web server port number to 80
WiFiServer webServer(80);

// Variable to store the HTTP request
String header;

unsigned int totalFed = 0;
unsigned int toFeed = 0;
unsigned int feedAmount = 2;

unsigned long lastFed = 0;
unsigned long timeUpdated = 0;
unsigned long lastScreenInit = 0;

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

String mqttTopic(const char* topic) {
  return String("homeassistant/light/") + String(ha_id) + String("/") + String(topic);
}

void mqttRegister() {
  StaticJsonBuffer<400> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();

  root["name"] = ha_name;
  root["platform"] = "mqtt_json";
  root["unique_id"] = ha_id;
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
    Serial.println("registered");
  } else {
    Serial.println("registration failed");
  }
}

void mqttPublish(const char* topic, const char* msg) {
  mqttClient.publish(mqttTopic("state").c_str(), msg);
}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqttClient.connect(mqtt_clientid, mqtt_user, mqtt_pass, mqttTopic("config").c_str(), 1, 1, "")) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      mqttRegister();
      // ... and resubscribe
      mqttClient.subscribe(mqttTopic("set").c_str());
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
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

  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] (");
  Serial.print(p_length);
  Serial.print(") ");
  Serial.print(payload);
  Serial.println();

  // feed cat if payload is "ON"
  StaticJsonBuffer<300> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(p_payload);
  if (!root.success()) {
    Serial.println("ERROR: parseObject() failed");
    return;
  }

  if (root.containsKey("brightness") && root["brightness"].is<int>()) {
    feedAmount = (root["brightness"].as<int>() + 1) / 64;
  }

  if (root.containsKey("state") && root["state"].as<String>() == String("ON")) {
    toFeed += feedAmount;
  }
}

void setup() {
  // put your setup code here, to run once:
  pinMode(inputPin, INPUT_PULLUP); // enable internal pullup

  pinMode(outputPin, OUTPUT);
  digitalWrite(outputPin, LOW);

  delay(100);

  Serial.begin(74880);

  Serial.println("Cat Feeder 0.1");
  Serial.println();

  serial.begin(115200);

  initScreen();

  // Connect to Wi-Fi network with SSID and password
  Serial.print("Connecting to ");
  Serial.print(wifi_ssid);
  Serial.print("...");

  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("connected");

  // Print local IP address
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Initialize a NTPClient to get time
  timeClient.begin();
  // Set offset time in seconds to adjust for your timezone, for example:
  // GMT +1 = 3600
  // GMT +8 = 28800
  // GMT -1 = -3600
  // GMT 0 = 0
  timeClient.setTimeOffset(-5 * 3600);

  webServer.begin();

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
}

String formatTime(unsigned long rawTime) {
  unsigned long hours = (rawTime % 86400L) / 3600;
  String hoursStr = hours < 10 ? "0" + String(hours) : String(hours);

  unsigned long minutes = (rawTime % 3600) / 60;
  String minuteStr = minutes < 10 ? "0" + String(minutes) : String(minutes);

  unsigned long seconds = rawTime % 60;
  String secondStr = seconds < 10 ? "0" + String(seconds) : String(seconds);

  return hoursStr + ":" + minuteStr + ":" + secondStr;
}

void initScreen() {
  glcd.reset();

  updateTitle();
  updateTotalFed();
  updateLastFed();
}

void redrawScreen() {
  glcd.clearScreen();

  updateTitle();
  updateTotalFed();
  updateLastFed();
}

void updateTitle() {
  snprintf(buffer, sizeof(buffer), "Cat Feeder   %s", timeClient.getFormattedTime().c_str());
  glcd.setXY(0, 0);
  glcd.printStr(buffer);
}

void updateTotalFed() {
  snprintf(buffer, sizeof(buffer), "Total Fed:   %d", totalFed);
  glcd.setXY(0, 10);
  glcd.printStr(buffer);
}

void updateLastFed() {
  const char* tmpl = "Last Feed:   %d%s ago";
  if (lastFed > 0) {
    unsigned long ago = timeClient.getEpochTime() - lastFed;
    if (ago < 60 * 60 * 20) {
      snprintf(buffer, sizeof(buffer), "Last Feed:   %s", formatTime(lastFed).c_str());
    } else if (ago < 60 * 60 * 48) {
      snprintf(buffer, sizeof(buffer), tmpl, ago / 60 / 60, "h");
    } else {
      snprintf(buffer, sizeof(buffer), tmpl, ago / 60 / 60 / 24, "d");
    }
  } else {
    snprintf(buffer, sizeof(buffer), "Not Fed Yet");
  }

  glcd.setXY(0, 20);
  glcd.printStr(buffer);
}

void updateFeeding(boolean feeding) {
  glcd.setXY(0, 30);
  if (feeding) {
    glcd.printStr("Feeding...");
  } else {
    glcd.printStr("          ");
  }
}

void updateError() {
  glcd.setXY(0, 30);
  glcd.printStr("ERROR!    ");
}

void loop() {
  // Handle web requests
  WiFiClient http_client = webServer.available();
  if (http_client) {
    handleConnection(http_client);
  }

  /*
  // Handle serial data
  recvLine();
  if (newData) {
    toFeed = String(receivedChars).toInt();
    if (toFeed == 0) {
      Serial.print("Ignoring invalid input ");
      Serial.print(receivedChars);
      Serial.println();
    }
    newData = false;
    Serial.print("total fed ");
    Serial.print(totalFed);
    Serial.println();
  }
  */

  // handle mqtt requests
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();

  // update time
  while(!timeClient.update()) {
    timeClient.forceUpdate();
  }

  // redraw entire screen once a minute to fix any corruption
  if (millis() / 60000 > lastScreenInit / 60000) {
    lastScreenInit = timeUpdated =  millis();
    redrawScreen();
  }
  // redraw title
  else if (millis() / 1000 > timeUpdated / 1000) {
    timeUpdated = millis();
    updateTitle();
  }

  // feed cat
  if (toFeed > 0) {
    feed(toFeed);
    toFeed = 0;
  }

  delay(10);
}

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
          if (header.indexOf("GET /feed") >= 0) {
            Serial.println("Requested Feed");
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

void feed(int numToFeed) {
  int numSegments = 0;
  int prevState = digitalRead(inputPin);
  int currState;

  updateFeeding(true);

  Serial.print("Feeding ");
  Serial.print(numToFeed);
  Serial.println();

  mqttPublish("state", (String("{\"state\":\"ON\",\"brightness\":") + String(numToFeed * 64) + String("}")).c_str());

  digitalWrite(outputPin, HIGH);

  unsigned long startTime = millis();

  while (true) {
    currState = digitalRead(inputPin);
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
      digitalWrite(outputPin, LOW);

      updateError();

      Serial.print("Error");

      return;
    }

    prevState = currState;

    delay(10);
  }

  digitalWrite(outputPin, LOW);

  lastFed = timeClient.getEpochTime();

  redrawScreen();

  Serial.print("Fed ");
  Serial.print(numSegments);
  Serial.println();

  mqttPublish("state", "{\"state\":\"OFF\"}");
}

void recvLine() {
 static byte ndx = 0;
 char endMarker = '\n';
 char rc;

 // if (Serial.available() > 0) {
 while (Serial.available() > 0 && newData == false) {
   rc = Serial.read();

   if (rc != endMarker) {
     receivedChars[ndx] = rc;
     ndx++;
     if (ndx >= numChars) {
       ndx = numChars - 1;
     }
   } else {
     receivedChars[ndx] = '\0'; // terminate the string
     ndx = 0;
     newData = true;
   }
 }
}
