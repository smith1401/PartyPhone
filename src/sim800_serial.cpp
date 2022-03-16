#include <Arduino.h>
#include <SoftwareSerial.h>

SoftwareSerial sim800(A2, A3);

void setup()
{
    Serial.begin(9600);
    sim800.begin(9600);
}

void loop()
{
    while (Serial.available())
        sim800.write(Serial.read());

    while (sim800.available())
        Serial.write(sim800.read());
}