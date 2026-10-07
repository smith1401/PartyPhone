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
// Worst case: country code + every dialed digit but the leading 0 + terminator
#define INTERNATIONAL_NUMBER_BUFF_LEN (MAX_NUMBER_DIGITS + sizeof(LOCAL_COUNTRY_CODE))

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
char internationalNumberBuffer[INTERNATIONAL_NUMBER_BUFF_LEN];

// Prototypes
const char *convertNumberToCountryCode(const char *num);
bool ring(void *);
void startRinging();
void stopRinging();
void parseSIM800response();
bool receiveSIM800(bool debug_out = false);
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
  SIM800.println(SIM800_ECHO_OFF_CMD); // echoed commands would only add noise to the parser
  pollSIM800();
  SIM800.println(SIM800_VERBOSE_ERRORS_CMD); // report "+CME ERROR: <text>" instead of a bare "ERROR"
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

void loop()
{
  updateSwitches();
  updateTickers();
  updateSIM800();
  updateStateMachine();
}

const char *convertNumberToCountryCode(const char *num)
{
  memset(internationalNumberBuffer, 0, INTERNATIONAL_NUMBER_BUFF_LEN);

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

void stopRinging()
{
  timer.cancel();
  ringCount = 0;
}

// Set by parseSIM800response() when a line terminates a command ("OK" / "ERROR")
bool finalResponseSeen = false;

// Compare the received line with a response stored in flash
static bool lineIs(const __FlashStringHelper *response)
{
  return strcmp_P(sim800Buffer, (const char *)response) == 0;
}

// Parses ONE complete line (without line ending) stored in sim800Buffer
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

  if (lineIs(SIM800_RESP_OK))
  {
    finalResponseSeen = true;
  }
  else if (lineIs(SIM800_RESP_RING))
  {
    // RING is repeated every few seconds while the call is ringing
    if (state == State::Idle)
    {
      incomingCall = true;
    }
    else if (state != State::Ringing)
    {
      // Already busy with another call -> reject the incoming one
      SIM800.println(SIM800_HANGUP_CALL_CMD);
    }
  }
  else if (lineIs(SIM800_RESP_NO_CARRIER))
  {
    if (state == State::Ringing || state == State::Connected)
    {
      timer.cancel();
      DEBUG.println(F("Other end hung up!"));
      // Only play the tone when someone is listening
      if (state == State::Connected)
        SIM800.println(SIM800_HUNG_UP_TONE);
      state = State::Idle;
      incomingCall = false;
    }
  }
  else if (lineIs(SIM800_RESP_BUSY))
  {
    state = State::Engaged;
    DEBUG.println(F("Other end is busy!"));
    SIM800.println(SIM800_BUSY_TONE);
  }
  else if (lineIs(SIM800_RESP_NO_DIALTONE) || lineIs(SIM800_RESP_NO_ANSWER) ||
           strstr_P(sim800Buffer, (const char *)SIM800_RESP_ERROR) != NULL)
  {
    finalResponseSeen = true;

    // A failing command during a call (e.g. a tone command) must not silently
    // abandon the call - the handset is off-hook and would never recover
    if (state != State::Connected && state != State::Ringing)
      state = State::Idle;

    DEBUG.print(F("SIM800 responded with an error: "));
    const char *error = strchr(sim800Buffer, ':');
    DEBUG.println(error != NULL ? error + 1 : sim800Buffer);
  }
}

// Collects incoming bytes and parses every complete line.
// Returns true if at least one complete line has been processed.
bool receiveSIM800(bool debug_out)
{
  bool lineProcessed = false;

  while (SIM800.available())
  {
    char c = SIM800.read();

    if (c == '\n')
    {
      // Terminate the line (and drop the trailing '\r') - empty lines are ignored
      if (pSIM800 > &sim800Buffer[0] && *(pSIM800 - 1) == '\r')
        pSIM800--;
      *pSIM800 = '\0';

      if (sim800Buffer[0] != '\0')
      {
        if (debug_out)
          DEBUG.println(sim800Buffer);

        parseSIM800response();
        lineProcessed = true;
      }

      pSIM800 = &sim800Buffer[0];
    }
    else if (c != '\0' && pSIM800 < &sim800Buffer[SIM800_AT_CMD_BUFF_LEN - 1])
    {
      // Keep one byte for the terminating '\0' - drop anything beyond that
      (*pSIM800++) = c;
    }
  }

  return lineProcessed;
}

// Waits for the response to a command: returns on "OK"/"ERROR" or after the timeout
void pollSIM800(bool debug_out)
{
  finalResponseSeen = false;

  uint32_t startPoll = millis();
  while (!finalResponseSeen && (millis() - startPoll) < SIM800_POLL_TIMOUT_MS)
  {
    receiveSIM800(debug_out);
  }
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
  receiveSIM800();
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
    // The user is turning the dial again -> do not start the call yet
    if (dialSwitch.fell())
    {
      timer.cancel(startCallTask);
    }

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
                               { if (state == State::Dialling) state = State::Connecting; return false; });

      // Zero == 10 pulses
      if (pulseCount == 10)
        pulseCount = 0;

      // Too many digits dialed -> the number cannot be valid
      if (currentDigit >= MAX_NUMBER_DIGITS)
      {
        timer.cancel(startCallTask);
        pulseCount = 0;
        state = State::InvalidNumber;
      }
      else
      {
        // Add current digit dialed to number
        dialedNumber[currentDigit++] = (char)((char)pulseCount + '0');
        dialedNumber[currentDigit] = '\0';
        DEBUG.print(F("\rNumber: "));
        DEBUG.print(dialedNumber);

        pulseCount = 0;
      }
    }
  }
  break;

  case State::Connecting:
  {
    // TODO: Connect to real phone
    const char *number = convertNumberToCountryCode(dialedNumber);

    // The number could not be converted - do not dial an empty number
    if (state == State::InvalidNumber)
      break;

    DEBUG.print(F("\r\nConnecting to "));
    DEBUG.println(number);
    SIM800.print(SIM800_DIAL_NUMBER_CMD);
    SIM800.print(number);
    SIM800.println(F(";"));
    pollSIM800();

    // A failed dial command resets the state - do not override it
    if (state == State::Connecting)
    {
      SIM800.println(SIM800_RINGING_TONE); // Ringing tone
      state = State::Connected;
    }
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
      stopRinging(); // the bell must not keep ringing during the call
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
