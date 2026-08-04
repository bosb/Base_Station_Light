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

// Set these to your desired credentials.
const char *ssid = "Tractive_XXXXX";
const char *password = "TheSunIsShiningTheWeatherIsSweet";

// gpio: 1, 2, 3-d3, 4-sw1, 5-d2, 6, 7, 8, 9-h4, 10-d1, 11.
const int buttonPin = 4;     // the number of the pushbutton pin
const int ledPin =  10;      // the number of the LED pin

int buttonState = 0;         // variable for reading the pushbutton status

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
  digitalWrite(ledPin, HIGH); // on, when softAP succeeds
}

void loop() {
  // read the state of the pushbutton value:
  buttonState = digitalRead(buttonPin);

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
