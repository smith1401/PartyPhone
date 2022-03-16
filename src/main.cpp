#include <Arduino.h>
#include <Bounce2.h>
#include <SoftwareSerial.h>
#include <arduino-timer.h>

#define DEBOUNCE_INTERVAL_MS 5
#define START_CALL_DELAY_MS 4000
#define RINGING_TIME_MS 6000
#define RINGER_ON_MS 2000
#define RINGER_OFF_MS 4000
#define RINGER_PULSE_MS 25
#define RINGER_PIN_COUNT 2
#define NUM_RINGS 1

#define MAX_NUMBER_DIGITS 15
#define SIM800_POLL_TIMOUT_MS 200
#define AT_CMD_BUFF_LEN 64
#define LOCAL_COUNTRY_CODE "+43"

const uint8_t call_pin = 2;
const uint8_t hook_pin = 3;
const uint8_t dial_pin = 5;
const uint8_t number_pin = 6;
const uint8_t ringer_pins[] = {10, 11};

Bounce callSwitch = Bounce(call_pin, INPUT);
Bounce hookSwitch = Bounce(hook_pin, INPUT);
Bounce dialSwitch = Bounce(dial_pin, INPUT);
Bounce numberSwitch = Bounce(number_pin, INPUT);

SoftwareSerial ss(2, 7);

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

char sim800Buffer[AT_CMD_BUFF_LEN] = {0x00};
char *pSIM800 = &sim800Buffer[0];

char internationalNumberBuffer[MAX_NUMBER_DIGITS + 1];

const char* convertNumberToCountryCode(const char * num)
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

    return false; 
  });
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
  char *found = strstr(sim800Buffer, "RING");
  if (found != NULL && state == State::Idle)
  {
    incomingCall = true;
  }
  else if (found != NULL && state != State::Idle)
  {
    SIM800.println(F("ATH"));
  }

  found = strstr(sim800Buffer, "NO CARRIER");
  if (found != NULL && (state == State::Ringing || state == State::Connected))
  {
    timer.cancel();
    state = State::Idle;
    DEBUG.println(F("Other end hung up!"));
    SIM800.println(F("AT+STTONE=1,5,15300000"));
    incomingCall = false;
  }

  found = strstr(sim800Buffer, "BUSY");
  if (found != NULL)
  {
    state = State::Engaged;
    DEBUG.println(F("Other end is busy!"));
    SIM800.println(F("AT+SIMTONE=1,425,500,500,300000"));
  }
}

void receiveSIM800()
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
    // DEBUG.println(sim800Buffer);
    parseSIM800response();
    strcpy(sim800Buffer, "");
  }
}

void pollSIM800()
{
  uint32_t startPoll = millis();
  while (!SIM800.available() && (millis() - startPoll) < SIM800_POLL_TIMOUT_MS) {}
  receiveSIM800();
}

void updateSwitches()
{
  callSwitch.update();
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
    SIM800.println(F("AT+STTONE=0"));
    SIM800.println(F("ATH"));
  }

  switch (state)
  {
  case State::Idle:
  {
    // If the hanset is picked off
    if (hookSwitch.fell())
    {
      state = State::Dialtone;
      // state = State::Ringing;
      SIM800.println(F("AT+STTONE=1,1,15300000"));
      DEBUG.println(F("Dialtone -> beeeeeep"));
    }

    // If a call is received
    // if (callSwitch.fell() && hookSwitch.read() == HIGH)
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
      SIM800.println(F("AT+STTONE=0"));
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
        startCallTask = timer.in(START_CALL_DELAY_MS, [](void *){ state = State::Connecting; return false;});

        // Zero == 10 pulses
        if (pulseCount == 10)
          pulseCount = 0;

        // Add current digit dialed to number
        dialedNumber[currentDigit++] = (char)((char)pulseCount + '0');
        dialedNumber[currentDigit] = '\0';
        // DEBUG.printf("\rNumber: '%s'", dialedNumber);
        DEBUG.print(F("\rNumber: "));
        DEBUG.print(dialedNumber);

        // TODO: Number comparison
        if (strncmp(dialedNumber, "06644386422", 11) == 0)
        {
          DEBUG.println();
          state = State::Connecting;
        }
        else if (strncmp(dialedNumber, "12345", 5) == 0)
        {
          DEBUG.println();
          state = State::Engaged;
        }
        else if (strlen(dialedNumber) > MAX_NUMBER_DIGITS)
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
    DEBUG.print(F("Connecting to "));
    DEBUG.println(number);
    SIM800.print(F("ATD+ "));
    SIM800.print(number);
    SIM800.println(F(";"));
    pollSIM800();
    state = State::Connected;
  }
  break;

  case State::Engaged:
    state = State::Idle;
    DEBUG.println(F("Engaged!"));
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
      SIM800.println(F("ATA"));
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

void setup()
{
  callSwitch.interval(DEBOUNCE_INTERVAL_MS);
  hookSwitch.interval(DEBOUNCE_INTERVAL_MS);
  dialSwitch.interval(DEBOUNCE_INTERVAL_MS);
  numberSwitch.interval(DEBOUNCE_INTERVAL_MS);

  // attachInterrupt(digitalPinToInterrupt(hook_pin), callAnswered, RISING);
  // attachInterrupt(digitalPinToInterrupt(call_pin), [](){ incomingCall = true; }, FALLING);

  for (size_t i = 0; i < RINGER_PIN_COUNT; i++)
  {
    pinMode(ringer_pins[i], OUTPUT);
  }

  // Initial state
  digitalWrite(ringer_pins[0], HIGH);
  digitalWrite(ringer_pins[1], LOW);

  SIM800.begin(57600);
  DEBUG.begin(57600);

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

  // DEBUG.println(__FILE__);
  // DEBUG.println(__DATE__);
  // DEBUG.println(__TIME__);

  SIM800.println(F("AT")); //Once the handshake test is successful, it will back to OK
  pollSIM800();
  SIM800.println(F("AT+CSQ")); //Signal quality test, value range is 0-31 , 31 is the best
  pollSIM800();
  SIM800.println(F("AT+CCID")); //Read SIM information to confirm whether the SIM is plugged
  pollSIM800();
  SIM800.println(F("AT+CREG?")); //Check whether it has registered in the network
  pollSIM800();
  SIM800.println(F("AT+CBC"));
  pollSIM800();

  SIM800.println(F("AT+CALS=0,0"));
  pollSIM800();

  SIM800.println(F("AT+CMIC=0,2"));
  pollSIM800();
  

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

  if (millis() - prevMillis > 1000)
  {
    int avg = duration / loopCount;
    DEBUG.print(state);
    DEBUG.print(F("  "));
    DEBUG.print(avg);
    DEBUG.println(F(" us"));
    prevMillis = millis();
    duration = loopCount = 0;
  }
}