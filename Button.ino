/*
 * Tools -> Board -> Boards Manager: esp32 by Espressif Systems 3.3.11 at time of writing
 * https://github.com/espressif/arduino-esp32
 * 
 * Tools -> Board -> ESP32 Arduino -> ESP32C3 Dev Module
 * USB CDC on boot: enabled
 * Flash Mode: DIO
 * JTAG Adapert: Integrated USB JTAG
 * 
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

const char* host = "basestation";
// Set these to your desired credentials.
const char *ssid = "Tractive_XXXXX";
const char *password = "TheSunIsShiningTheWeatherIsSweet";

// gpio: 0, 1, 2, 3-d3, 4-sw1, 5-d2, 6, 7, 8, 9-h4, 10-d1, 11.
const int buttonPin = 4;     // the number of the pushbutton pin
const int ledPin =  10;      // the number of the LED pin

int buttonState = 0;         // variable for reading the pushbutton status

WebServer server(80);

const char* serverIndex =
"<form method='POST' action='/update' enctype='multipart/form-data' id='upload_form'>"
   "<input type='file' name='update'>"
        "<input type='submit' value='Update'>"
    "</form>";

void handleStatus() {
  String html = "<!DOCTYPE html><html>";
  html += "<head><title>ESP32</title></head>";
  html += "<body><h1>Hello from ESP32</h1></body>";
  html +=     temperatureRead();
  html += "<br>";
  html += WiFi.localIP().toString();
  html += "<br>";
  html += "</html>";
  server.send(200, "text/html", html);
}

void setup() {
  pinMode(9, OUTPUT); // on the 8-pin header
  
  // initialize the LED pin as an output:
  pinMode(3, OUTPUT); // right
  pinMode(5, OUTPUT); // middle
  pinMode(ledPin, OUTPUT); // left

  // initialize the pushbutton pin as an input:
  pinMode(buttonPin, INPUT_PULLUP); // INPUT_PULLUP INPUT_PULLDOWN INPUT

  digitalWrite(ledPin, LOW); // off...
     
  if (!WiFi.softAP(ssid, password)) {
    log_e("Soft AP creation failed.");
    while (1);
  }
  digitalWrite(5, HIGH); // 
  
  server.on("/status", handleStatus);
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex);
  });
  /*handling uploading firmware file */
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
 }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.begin();
    

  digitalWrite(ledPin, HIGH); // on, when softAP succeeds
   /*use mdns for host name resolution*/
  if (!MDNS.begin(host)) {
    log_e("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }
  digitalWrite(3, HIGH); // 
}

void loop() {
  // read the state of the pushbutton value:
  buttonState = digitalRead(buttonPin);
  server.handleClient();

  // check if the pushbutton is pressed. If it is, the buttonState is HIGH:
  if (buttonState == HIGH) {
    // turn LED on:
    digitalWrite(3, LOW);
  } else {
    // turn LED off:
    digitalWrite(3, HIGH);
  }

  if (WiFi.softAPgetStationNum() > 0) { 
    digitalWrite(5, HIGH); // when a client is connected to wifi, led on
  } else {
    digitalWrite(5, HIGH); // ... while noone is connected, flash
    delay(1000);
    digitalWrite(5, LOW);
    delay(1000);
  }
}
