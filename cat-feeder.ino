#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "config.h"

// Set web server port number to 80
WiFiServer server(80);

// Variable to store the HTTP request
String header;

int totalFed = 0;
int toFeed = 0;

const byte numChars = 32;
char receivedChars[numChars]; // an array to store the received data

boolean newData = false;

WiFiClient espClient;
PubSubClient client(espClient);

void publish(const char* topic, const char* msg) {
  client.publish(String(String(topic_base) + topic).c_str(), msg);
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(mqtt_clientid, mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      publish("announce", "hello world");
      // ... and resubscribe
      client.subscribe(String(String(topic_base) + "command").c_str());
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  // feed cat if payload is "ON"
  if (length == 2 && !strncmp((char *)payload, "ON", length)) {
    digitalWrite(BUILTIN_LED, LOW);   // Turn the LED on (Note that LOW is the voltage level
    // but actually the LED is on; this is because
    // it is active low on the ESP-01)
    feed(2);
    digitalWrite(BUILTIN_LED, HIGH);
  } else {
    digitalWrite(BUILTIN_LED, HIGH);  // Turn the LED off by making the voltage HIGH
  }
}

void setup() {
  // put your setup code here, to run once:
  pinMode(inputPin, INPUT_PULLUP); // enable internal pullup

  pinMode(outputPin, OUTPUT);
  digitalWrite(outputPin, LOW);

  Serial.begin(74880);

  delay(100);

  Serial.println("Cat Feeder");
  Serial.println();

  // Connect to Wi-Fi network with SSID and password
  Serial.print("Connecting to ");
  Serial.println(wifi_ssid);

  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  // Print local IP address and start web server
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  server.begin();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  Serial1.begin(115200);

  initScreen();
}

void initScreen() {
  Serial1.write(0x7c);
  Serial1.write((byte)0);

  Serial1.print("Cat Feeder 0.1");

  updateTotalFed();
}

void setX(byte posX) // 0-127 or 0-159 pixels
{
  // Set the X position
  Serial1.write(0x7C);
  Serial1.write(0x18);// CTRL x
  Serial1.write(posX);
}

void setY(byte posY) // 0-63 or 0-127 pixels
{
  // Set the Y position
  Serial1.write(0x7C);
  Serial1.write(0x19);// CTRL y
  Serial1.write(posY);
}

void updateTotalFed() {
  setX(0);
  setY(52);
  Serial1.print("Total Fed: ");
  Serial1.print(totalFed);
  Serial1.print("   ");
}

void updateFeeding(boolean feeding) {
  setX(0);
  setY(40);
  if (feeding) {
    Serial1.print("Feeding...");
  } else {
    Serial1.print("          ");
  }
}

void loop() {
  WiFiClient http_client = server.available();   // Listen for incoming clients

  if (http_client) {                             // If a new client connects,
    handleConnection(http_client);
  }

  /*
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

  if (!client.connected()) {
    reconnect();
  }

  client.loop();

  if (toFeed > 0) {
    feed(toFeed);
    toFeed = 0;
  }
}

void handleConnection(WiFiClient http_client) {
  Serial.println("New Client.");          // print a message out in the serial port
  String currentLine = "";                // make a String to hold incoming data from the client
  while (http_client.connected()) {            // loop while the client's connected
    if (http_client.available()) {             // if there's bytes to read from the client,
      char c = http_client.read();             // read a byte, then
      Serial.write(c);                    // print it out the serial monitor
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
            http_client.println("Content-type:text/html");
            http_client.println("Connection: close");
            http_client.println();

            // Display the HTML web page
            http_client.println("<!DOCTYPE html><html>");
            http_client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
            http_client.println("<link rel=\"icon\" href=\"data:,\">");
            // CSS to style the on/off buttons
            // Feel free to change the background-color and font-size attributes to fit your preferences
            http_client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
            http_client.println(".button { background-color: #195B6A; border: none; color: white; padding: 16px 40px;");
            http_client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
            http_client.println(".button2 {background-color: #77878A;}</style></head>");

            // Web Page Heading
            http_client.println("<body><h1>Cat Feeder</h1>");

            // Display current state, and ON/OFF buttons for GPIO 5
            http_client.println("<p>Feeding</p>");

            http_client.println("</body></html>");

            // The HTTP response ends with another blank line
            http_client.println();
            // Break out of the while loop
            break;
          }

          // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
          // and a content-type so the client knows what's coming, then a blank line:
          http_client.println("HTTP/1.1 200 OK");
          http_client.println("Content-type:text/html");
          http_client.println("Connection: close");
          http_client.println();

          // Display the HTML web page
          http_client.println("<!DOCTYPE html><html>");
          http_client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
          http_client.println("<link rel=\"icon\" href=\"data:,\">");
          // CSS to style the on/off buttons
          // Feel free to change the background-color and font-size attributes to fit your preferences
          http_client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
          http_client.println(".button { background-color: #195B6A; border: none; color: white; padding: 16px 40px;");
          http_client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
          http_client.println(".button2 {background-color: #77878A;}</style></head>");

          // Web Page Heading
          http_client.println("<body><h1>Cat Feeder</h1>");

          // Display current state, and ON/OFF buttons for GPIO 5
          http_client.println("<p>Total Fed " + String(totalFed) + "</p>");
          http_client.println("<p>To Feed " + String(toFeed) + "</p>");
          http_client.println("<p><a href=\"/feed\"><button class=\"button\">FEED</button></a></p>");

          http_client.println("</body></html>");

          // The HTTP response ends with another blank line
          http_client.println();
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

  Serial.println("Client disconnected.");
  Serial.println("");
}

void feed(int numToFeed) {
  int numSegments = 0;
  int prevState = HIGH;
  int currState;

  updateFeeding(true);

  Serial.print("Feeding ");
  Serial.print(numToFeed);
  Serial.println();

  publish("state", "ON");

  digitalWrite(outputPin, HIGH);

  while (true) {
    currState = digitalRead(inputPin);
    if (currState == HIGH && prevState == LOW) {
      totalFed++;
      numSegments++;

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

    if (numSegments >= numToFeed) {
      break;
    }

    delay(10);
    prevState = currState;
  }

  digitalWrite(outputPin, LOW);

  updateFeeding(false);

  Serial.print("Fed ");
  Serial.print(numSegments);
  Serial.println();

  publish("state", "OFF");
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
