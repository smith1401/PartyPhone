/*
  MIT License

  Copyright (c) 2022 Christoph Schmied

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <Arduino.h>
#include <Bounce2.h>
#include <SoftwareSerial.h>
#include <arduino-timer.h>
#include "sim800_defines.h"

#define DEBOUNCE_INTERVAL_MS 5
#define START_CALL_DELAY_MS 4000
#define RINGING_TIME_MS 6000
#define RINGER_ON_MS 2000
#define RINGER_OFF_MS 4000
#define RINGER_PULSE_MS 25
#define RINGER_PIN_COUNT 2
#define NUM_RINGS 1
#define MAX_NUMBER_DIGITS 15
#define LOCAL_COUNTRY_CODE "+43"

const uint8_t hook_pin = 3;
const uint8_t dial_pin = 5;
const uint8_t number_pin = 6;
const uint8_t ringer_pins[] = {10, 11};

Bounce hookSwitch = Bounce(hook_pin, INPUT);
Bounce dialSwitch = Bounce(dial_pin, INPUT);
Bounce numberSwitch = Bounce(number_pin, INPUT);

SoftwareSerial ss(SIM800_RX_PIN, SIM800_TX_PIN);

#define DEBUG Serial
#define SIM800 ss

Timer<> timer = timer_create_default();
Timer<>::Task ringerTask;
Timer<>::Task pinChangeTask;
Timer<>::Task startCallTask;

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

char dialedNumber[MAX_NUMBER_DIGITS + 1] = {0x00};
uint8_t currentDigit = 0;
uint8_t pulseCount = 0;
uint32_t lastRingTime = 0;
bool incomingCall = false;

State state = State::Idle;
uint8_t ringCount = 0;

// Buffers
char sim800Buffer[SIM800_AT_CMD_BUFF_LEN] = {0x00};
char *pSIM800 = &sim800Buffer[0];
char internationalNumberBuffer[MAX_NUMBER_DIGITS + 1];

// Prototypes
const char *convertNumberToCountryCode(const char *num);
bool ring(void *);
void startRinging();
void callAnswered();
void parseSIM800response();
void receiveSIM800(bool debug_out = false);
void pollSIM800(bool debug_out = false);
void updateSwitches();
void updateTickers();
void updateSIM800();
void updateStateMachine();

void setup()
{
  // Setup switch debounce intervals
  hookSwitch.interval(DEBOUNCE_INTERVAL_MS);
  dialSwitch.interval(DEBOUNCE_INTERVAL_MS);
  numberSwitch.interval(DEBOUNCE_INTERVAL_MS);

  // Configure initial state of ringer pins (must be different)
  for (size_t i = 0; i < RINGER_PIN_COUNT; i++)
  {
    pinMode(ringer_pins[i], OUTPUT);
  }

  digitalWrite(ringer_pins[0], HIGH);
  digitalWrite(ringer_pins[1], LOW);

  // Setup hardware and software serial ocmmunication
  SIM800.begin(57600);
  DEBUG.begin(57600);

  delay(1000);

  DEBUG.println(F("Start of Partyphone. Have fun :)"));

  // If the hook switch is LOW during startup, switch so serial-ping-pong mode
  if (hookSwitch.read() == LOW)
  {
    DEBUG.println(F("Changing to Serial mode."));

    while (hookSwitch.read() == LOW)
    {
      while (DEBUG.available())
        SIM800.write(DEBUG.read());

      while (SIM800.available())
        DEBUG.write(SIM800.read());

      hookSwitch.update();
    }

    DEBUG.println(F("Exit Serial mode."));
  }

  // Initialize SIM800 module and get basic information
  SIM800.println(SIM800_HANDSHAKE_CMD); // Once the handshake test is successful, it will back to OK
  pollSIM800();
  SIM800.println(SIM800_FACTORY_RESET); // sometimes the modle just stops working :/
  pollSIM800();
  SIM800.println(SIM800_SIGNAL_QUALITY_CMD); // Signal quality test, value range is 0-31 , 31 is the best
  pollSIM800(true);
  // SIM800.println(SIM800_SIM_INFO_CMD); // Read SIM information to confirm whether the SIM is plugged
  // pollSIM800(true);
  SIM800.println(SIM800_REGISTERED_CMD); // Check whether it has registered in the network
  pollSIM800(true);
  SIM800.println(SIM800_BATTERY_STATUS_CMD);
  pollSIM800(true);
  SIM800.println(SIM800_DISABLE_RINGER_CMD);
  pollSIM800();
  SIM800.println(SIM800_MIC_GAIN_CMD(5));
  pollSIM800(true);

  DEBUG.println(F("ready"));
}

State prevState = State::Idle;
uint32_t prevMillis = millis();
uint32_t duration = 0;
int loopCount = 0;

void loop()
{
  uint32_t start = micros();

  updateSwitches();
  updateTickers();
  updateSIM800();
  updateStateMachine();

  uint32_t stop = micros();
  duration += (stop - start);
  loopCount++;

  // Averge rate is above 10kHz - fast enough!
  // if (millis() - prevMillis > 1000)
  // {
  //   int avg = duration / loopCount;
  //   DEBUG.print(state);
  //   DEBUG.print(F("  "));
  //   DEBUG.print(avg);
  //   DEBUG.println(F(" us"));
  //   prevMillis = millis();
  //   duration = loopCount = 0;
  // }
}

const char *convertNumberToCountryCode(const char *num)
{
  memset(internationalNumberBuffer, 0, MAX_NUMBER_DIGITS + 1);

  // Is it an international number
  if (strncmp(num, "00", 2) == 0)
  {
    strcat(internationalNumberBuffer, "+");
    strcat(internationalNumberBuffer, &num[2]);
  }
  // Is it a local number
  else if (strncmp(num, "0", 1) == 0)
  {
    strcat(internationalNumberBuffer, LOCAL_COUNTRY_CODE);
    strcat(internationalNumberBuffer, &num[1]);
  }
  else
  {
    state = State::InvalidNumber;
  }

  return internationalNumberBuffer;
}

bool ring(void *)
{
  // Switch the polarity of the ringer pins
  for (size_t i = 0; i < RINGER_PIN_COUNT; i++)
  {
    digitalWrite(ringer_pins[i], !digitalRead(ringer_pins[i]));
  }

  return true;
}

void startRinging()
{
  // Attach ticker for ringer pulses
  pinChangeTask = timer.every(RINGER_PULSE_MS, ring);

  // Attach ticker for ringer on/off period
  ringerTask = timer.in(RINGER_ON_MS, [](void *) -> bool
                        { 
    // Cancel pinChangeTask and start new ringerTask
    timer.cancel(pinChangeTask);
    ringerTask = timer.in(RINGER_OFF_MS, [](void *) -> bool
    {
        if (++ringCount < NUM_RINGS)
          startRinging();
        else
          ringCount = 0;

        return false;
    });

    return false; });
}

void callAnswered()
{
  timer.cancel();
  ringCount = 0;
  state = State::Idle;
}

void parseSIM800response()
{
  /*
  OK 	          Acknowledges execution of a Command
  CONNECT 	    A connection has been established; the DCE is moving from Command state to online data state
  RING 	        The DCE has detected an incoming call signal from network
  NO CARRIER 	  The connection has been terminated or the attempt to establish a connection failed
  ERROR 	      Command not recognized, Command line maximum length exceeded, parameter value invalid, or other problem with processing the Command line
  NO DIALTONE 	No dial tone detected
  BUSY 	        Engaged (busy) signal detected
  NO ANSWER 	  "@" (Wait for Quiet Answer) dial modifier was used, but remote ringing followed by five seconds of silence was not detected before expiration of the connection timer (S7)
  PROCEEDING 	  An AT command is being processed
  */

  // Check if someone is calling
  char *found = strstr_P(sim800Buffer, (const char *)SIM800_RESP_RING);
  if (found != NULL && state == State::Idle)
  {
    incomingCall = true;
  }
  else if (found != NULL && state != State::Idle)
  {
    SIM800.println(SIM800_ANSWER_CALL_CMD);
  }

  found = strstr_P(sim800Buffer, (const char *)SIM800_RESP_NO_CARRIER);
  if (found != NULL && (state == State::Ringing || state == State::Connected))
  {
    timer.cancel();
    state = State::Idle;
    DEBUG.println(F("Other end hung up!"));
    SIM800.println(SIM800_HUNG_UP_TONE);
    incomingCall = false;
  }

  found = strstr_P(sim800Buffer, (const char *)SIM800_RESP_BUSY);
  if (found != NULL)
  {
    state = State::Engaged;
    DEBUG.println(F("Other end is busy!"));
    SIM800.println(SIM800_BUSY_TONE);
  }
}

void receiveSIM800(bool debug_out)
{
  int count = 0;
  while (SIM800.available())
  {
    char c = SIM800.read();

    if (c != '\0')
      (*pSIM800++) = c;

    count++;
  }

  if (sim800Buffer[0] != '\0')
  {
    *pSIM800 = '\0';
    pSIM800 = &sim800Buffer[0];
    parseSIM800response();

    if (debug_out)
      DEBUG.println(pSIM800);

    strcpy(sim800Buffer, "");
  }
}

void pollSIM800(bool debug_out)
{
  uint32_t startPoll = millis();
  while (!SIM800.available() && (millis() - startPoll) < SIM800_POLL_TIMOUT_MS)
  {
  }
  receiveSIM800(debug_out);
}

void updateSwitches()
{
  hookSwitch.update();
  dialSwitch.update();
  numberSwitch.update();
}

void updateTickers()
{
  timer.tick<void>();
}

void updateSIM800()
{
  if (SIM800.available())
  {
    receiveSIM800();
  }
}

void updateStateMachine()
{
  if (hookSwitch.rose())
  {
    DEBUG.println(F("Handset replaced!"));
    state = State::Idle;
    strcpy(dialedNumber, "");
    currentDigit = ringCount = pulseCount = 0;
    timer.cancel();
    SIM800.println(SIM800_TONE_STOP);
    SIM800.println(SIM800_HANGUP_CALL_CMD);
  }

  switch (state)
  {
  case State::Idle:
  {
    // If the hanset is picked off
    if (hookSwitch.fell())
    {
      state = State::Dialtone;
      SIM800.println(SIM800_DIAL_TONE);
      DEBUG.println(F("Dialtone -> beeeeeep"));
    }

    // If a call is received
    if (incomingCall)
    {
      state = State::Ringing;
      DEBUG.println(F("Incoming call!"));
    }
  }
  break;

  case State::Dialtone:
  {
    // // If the user starts dialling
    if (dialSwitch.fell())
    {
      state = State::Dialling;
      SIM800.println(SIM800_TONE_STOP);
      DEBUG.println(F("Started dialling"));
    }
  }
  break;

  case State::Dialling:
  {
    // If a pulse has been detected
    if (numberSwitch.rose())
    {
      pulseCount++;
    }

    // If the dial has returned to its initial position
    if (dialSwitch.rose())
    {
      // Start new task to wait for no input in order to start a call
      timer.cancel(startCallTask);
      startCallTask = timer.in(START_CALL_DELAY_MS, [](void *)
                               { state = State::Connecting; return false; });

      // Zero == 10 pulses
      if (pulseCount == 10)
        pulseCount = 0;

      // Add current digit dialed to number
      dialedNumber[currentDigit++] = (char)((char)pulseCount + '0');
      dialedNumber[currentDigit] = '\0';
      DEBUG.print(F("\rNumber: "));
      DEBUG.print(dialedNumber);

      if (strlen(dialedNumber) > MAX_NUMBER_DIGITS)
      {
        state = State::InvalidNumber;
      }
      else
      {
        pulseCount = 0;
      }
    }
  }
  break;

  case State::Connecting:
  {
    // TODO: Connect to real phone
    const char *number = convertNumberToCountryCode(dialedNumber);
    DEBUG.print(F("\r\nConnecting to "));
    DEBUG.println(number);
    SIM800.print(SIM800_DIAL_NUMBER_CMD);
    SIM800.print(number);
    SIM800.println(F(";"));
    SIM800.println(SIM800_RINGING_TONE); // Ringing tone

    pollSIM800();
    state = State::Connected;
  }
  break;

  case State::Engaged:
    DEBUG.println(F("Engaged!"));
    state = State::Idle;
    break;

  case State::InvalidNumber:
    DEBUG.println(F("InvalidNumber!"));
    state = State::Idle;
    break;

  case State::Connected:
    break;

  case State::Ringing:
  {
    if (millis() - lastRingTime > RINGING_TIME_MS)
    {
      startRinging();
      lastRingTime = millis();
    }

    if (hookSwitch.fell())
    {
      state = State::Connected;
      SIM800.println(SIM800_ANSWER_CALL_CMD);
      DEBUG.println(F("Call answered!"));
      incomingCall = false;
    }
  }
  break;

  default:
  {
    state = State::Idle;
    DEBUG.println(F("Default"));
  }
  break;
  }
}
