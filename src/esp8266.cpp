#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Bounce2.h>
#include <ESP8266WiFi.h>
#include <WiFiServer.h>
#include <Ticker.h>

#define DEBOUNCE_INTERVAL_MS 5
#define RINGING_TIME_MS 3000
#define RINGER_ON_MS 400
#define RINGER_OFF_MS 200
#define RINGER_PULSE_MS 20
#define RINGER_PIN_COUNT 2
#define MAX_NUMBER_DIGITS 9
#define NUM_RINGS 2

const char *ssid = "NotYourWifi";
const char *passwd = "Interrail_09";

WiFiServer server(8888);
WiFiClient console;

// D0, D7, D8 not working

const uint8_t hook_pin = D2;
const uint8_t dial_pin = D3;
const uint8_t number_pin = D4;
const uint8_t ringer_pins[] = {D5, D6};

Bounce hookSwitch = Bounce(hook_pin, INPUT_PULLUP);
Bounce dialSwitch = Bounce(dial_pin, INPUT_PULLUP);
Bounce numberSwitch = Bounce(number_pin, INPUT_PULLUP);

Ticker ringerTicker = Ticker();
Ticker pinChangeTicker = Ticker();

typedef enum
{
  Idle,
  Dialtone,
  Dialling,
  InvalidNumber,
  Connecting,
  Connected,
  Ringing,
  Engaged,
  Disconnected
} State;

String dialedNumber = "";
uint8_t currentDigit = 0;
uint8_t pulseCount = 0;
uint32_t lastRingTime = 0;
bool incomingCall = false;

volatile State state = State::Idle;
volatile uint8_t ringCount = 0;

void updateSwitches()
{
  hookSwitch.update();
  dialSwitch.update();
  numberSwitch.update();
}

void ring()
{
  for (size_t i = 0; i < RINGER_PIN_COUNT; i++)
  {
    digitalWrite(ringer_pins[i], !digitalRead(ringer_pins[i]));
  }
}

void startRinging()
{
  // Attach ticker for ringer pulses
  pinChangeTicker.attach_ms(RINGER_PULSE_MS, ring);

  // Attach ticker for ringer on/off period
  ringerTicker.once_ms(RINGER_ON_MS, []()
                       {
    pinChangeTicker.detach();

    if (++ringCount < NUM_RINGS)
      ringerTicker.once_ms(RINGER_OFF_MS, startRinging);
    else
      ringCount = 0; });
}

IRAM_ATTR void callAnswered()
{
  ringerTicker.detach();
  pinChangeTicker.detach();
  ringCount = 0;
  state = State::Idle;
}

void UpdateConsole()
{
  if (server.hasClient())
  {
    if (console.connected())
    {
      Serial.println("Connection rejected");
      server.available().stop();
    }
    else
    {
      console = server.available();
      Serial.printf("Connection accepted from %s\r\n", console.remoteIP().toString().c_str());
      console.println("Partyphone ready :)");
    }
  }
}

uint8_t pins[] = {16,5,4,0,2,1,4,12,13,15};

void setup()
{
  // for (size_t i = 0; i < 8; i++)
  // {
  //   pinMode(pins[i], INPUT_PULLUP);
  // }

  // return;
  
  hookSwitch.interval(DEBOUNCE_INTERVAL_MS);
  dialSwitch.interval(DEBOUNCE_INTERVAL_MS);
  numberSwitch.interval(DEBOUNCE_INTERVAL_MS);

  // attachInterrupt(digitalPinToInterrupt(hook_pin), callAnswered, FALLING);

  for (size_t i = 0; i < RINGER_PIN_COUNT; i++)
  {
    pinMode(ringer_pins[i], OUTPUT);
  }

  // Initial state
  digitalWrite(ringer_pins[0], HIGH);
  digitalWrite(ringer_pins[1], LOW);

  Serial.begin(74880);
  Serial.println();

  WiFi.begin(ssid, passwd);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());

  server.begin();
  ArduinoOTA.begin();

  Serial.println(__FILE__);
  Serial.printf("Compiled: %s %s\r\n", __DATE__, __TIME__);
}

State prevState = State::Idle;

void loop()
{
  updateSwitches();
  UpdateConsole();
  ArduinoOTA.handle();

  if (hookSwitch.rose())
  {
    console.println("Handset replaced!");
    state = State::Idle;
    dialedNumber = "";
    currentDigit = 0;
    pulseCount = 0;
  }

  if (state != prevState)
    Serial.println(state);

  prevState = state;

  switch (state)
  {
  case State::Idle:
    // If the hanset is picked off
    if (hookSwitch.fell())
    {
      state = State::Dialtone;
      console.println("Dialtone -> beeeeeep");
    }

    // If a call is received
    if (incomingCall)
    {
      state = State::Ringing;
      console.println("Incoming call!");
    }
    break;

  case State::Dialtone:
    // If the user starts sialling
    if (dialSwitch.fell())
    {
      state = State::Dialling;
      console.println("Started dialling");
    }
    break;
  case State::Dialling:
    // If a pulse has been detected
    if (numberSwitch.rose())
    {
      pulseCount++;
    }

    // If the dial has returned to its initial position
    if (dialSwitch.rose())
    {
      // Zero => 10 pulses
      if (pulseCount == 10)
        pulseCount = 0;

      // console.printf("Pulses: %d\n", pulseCount);

      // Add current digit dialed to number
      dialedNumber += (char)((char)pulseCount + '0');
      console.printf("\rNumber: '%s'", dialedNumber.c_str());
      currentDigit++;

      // TODO: Number comparison
      if (dialedNumber.equals("911"))
      {
        state = State::Connecting;
        console.print("\nConnecting to 911 ... ");
      }
      else if (dialedNumber.equals("12345"))
      {
        state = State::Engaged;
        console.println("\nEngaged");
      }
      else if (dialedNumber.length() > MAX_NUMBER_DIGITS)
      {
        state = State::InvalidNumber;
        console.println("\nInvalid number");
        dialedNumber = "";
      }
      else
      {
        pulseCount = 0;
      }
    }
    break;
  case State::Connecting:
    // TODO: Connect to real phone
    delay(3000);
    state = State::Connected;
    console.println("connected!");
    break;
  case State::Engaged:
    incomingCall = true;
    state = State::Idle;
    break;

  case State::InvalidNumber:
    state = State::Idle;
    break;
  case State::Connected:
    state = State::Idle;
    break;
  case State::Ringing:
    if (millis() - lastRingTime > RINGING_TIME_MS)
    {
      startRinging();
      lastRingTime = millis();
    }

    if (hookSwitch.fell())
    {
      state = State::Connected;
      console.println("Call answered!");
      incomingCall = false;
    }
    break;
  default:
    break;
  }
}