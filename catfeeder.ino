#include <dummy.h>

byte inputPin = D5;
byte outputPin = D6;

int totalFed = 0;

const byte numChars = 32;
char receivedChars[numChars]; // an array to store the received data

boolean newData = false;

void setup() {
  // put your setup code here, to run once:
  pinMode(inputPin, INPUT_PULLUP); // enable internal pullup

  pinMode(outputPin, OUTPUT);
  digitalWrite(outputPin, LOW);

  delay(10);
  
  Serial.begin(74880);
  Serial.println("Cat Feeder");
  Serial.println();
}

void loop() {
  recvLine();

  if (newData) {
    int toFeed = String(receivedChars).toInt();
    if (toFeed == 0) {
      Serial.print("Ignoring invalid input ");
      Serial.print(receivedChars);
      Serial.println();
    } else {
      feed(toFeed);
    }
    newData = false;
    Serial.print("total fed ");
    Serial.print(totalFed);
    Serial.println();
  }
}

void feed(int numToFeed) {
  int numSegments = 0;
  int prevState = HIGH;
  int currState;

  Serial.print("Feeding ");
  Serial.print(numToFeed);
  Serial.println();

  digitalWrite(outputPin, HIGH);

  while (true) {
    currState = digitalRead(inputPin);
    if (currState == HIGH && prevState == LOW) {
      totalFed++;
      numSegments++;
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

  Serial.print("Fed ");
  Serial.print(numSegments);
  Serial.println();
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
