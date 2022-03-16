#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <WiFiServer.h>

const char *ssid = "NotYourWifi";
const char *passwd = "Interrail_09";

WiFiServer server(8888);
WiFiClient console;

void UpdateConsole()
{
    if (server.hasClient())
    {
        if (console.connected())
        {
            Serial.println("AT");
            server.available().stop();
        }
        else
        {
            console = server.available();
            console.println("SIM800 ready!");
        }
    }
}

void setup()
{
    pinMode(D2, OUTPUT);
    digitalWrite(D2, LOW);
    delay(100);
    pinMode(D2, INPUT_PULLUP);
    
    Serial.begin(115200);
    WiFi.begin(ssid, passwd);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
    }

    server.begin();
    ArduinoOTA.begin();
}

void loop()
{
    UpdateConsole();
    ArduinoOTA.handle();

    if (console.available())
    {
        while (console.available())
            Serial.write(console.read());
    }

    if (Serial.available())
    {
        while (Serial.available())
            console.write(Serial.read());
    }
}