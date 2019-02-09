/* NOTE:
The IF482 Generator needs an high precise 1 Hz clock signal which cannot be
acquired in suitable precision on the ESP32 SoC itself. Additional clocking
hardware is required, ususally the clock signal is generated by external RTC or
GPS which can generate a precise time pulse signal (+/- 2ppm).

In this example code we use a Maxim DS3231 RTC chip, and configure the chips's
interrupt output pin as clock. The clock signal triggers an interrupt on the
ESP32, which controls the realtime output of IF482 telegram. This is why code in
IF482.cpp depends on code in RTCTIME.cpp.
*/

///////////////////////////////////////////////////////////////////////////////

/*
IF482 Generator to control clocks with IF482 telegram input (e.g. BÜRK BU190)
   

Example IF482 telegram: "OAL160806F170400"

IF482 Specification:
http://www.mobatime.com/fileadmin/user_upload/downloads/TE-112023.pdf

The IF 482 telegram is a time telegram, which sends the time and date
information as ASCII characters through the serial interface RS 232 or RS 422.

Communication parameters:

Baud rate: 9600 Bit/s
Data bits 7
Parity: even
Stop bit: 1
Jitter: < 50ms

Interface : RS232 or RS422

Synchronization: Telegram ends at the beginning of the second
specified in the telegram

Cycle: 1 second

Format of ASCII telegram string:

Byte  Meaning             ASCII     Hex
 1    Start of telegram   O         4F
 2    Monitoring*         A         41
 3    Time-Season**       W/S/U/L   57 or 53
 4    Year tens           0 .. 9    30 .. 39
 5    Year unit           0 .. 9    30 .. 39
 6    Month tens          0 or 1    30 or 31
 7    Month unit          0 .. 9    30 .. 39
 8    Day tens            0 .. 3    30 .. 33
 9    Day unit            0 .. 9    30 .. 39
10    Day of week***      1 .. 7    31 .. 37
11    Hours tens          0 .. 2    30 .. 32
12    Hours unit          0 .. 9    30 .. 39
13    Minutes tens        0 .. 5    30 .. 35
14    Minutes unit        0 .. 9    30 .. 39
15    Seconds tens        0 .. 5    30 .. 35
16    Seconds unit        0 .. 9    30 .. 39
17    End of telegram     CR        0D

*) Monitoring:
With a correctly received time in the sender unit, the ASCII character 'A' is
issued. If 'M' is issued, this indicates that the sender was unable to receive
any time signal for over 12 hours (time is accepted with ‘A’ and ‘M’).

**) Season:
W: Standard time,
S: Season time,
U: UTC time (not supported by all systems),
L: Local Time

***) Day of week:
not evaluated by model BU-190

*/
///////////////////////////////////////////////////////////////////////////////

#ifdef HAS_IF482

#ifdef HAS_DCF77
#error "You must define at most one of IF482 or DCF77"
#endif

#include "if482.h"

// Local logging tag
static const char TAG[] = "main";

#define IF482_FRAME_SIZE (17)
#define IF482_PULSE_DURATION (1000)

// select internal / external clock
#if defined RTC_INT && defined RTC_CLK
#define PPS RTC_CLK
#elif defined GPS_INT && defined GPS_CLK
#define PPS GPS_CLK
#else
#define PPS IF482_PULSE_DURATION
#endif


HardwareSerial IF482(2); // use UART #2 (note: #1 may be in use for serial GPS)

// initialize and configure IF482 Generator
int if482_init(void) {

  // open serial interface
  IF482.begin(HAS_IF482);

  // start if482 serial output feed task
  xTaskCreatePinnedToCore(if482_loop,  // task function
                          "if482loop", // name of task
                          2048,        // stack size of task
                          (void *)1,   // parameter of the task
                          3,           // priority of the task
                          &ClockTask,  // task handle
                          0);          // CPU core

  assert(ClockTask); // has clock task started?

  pps_init(PPS); // setup pulse
  pps_start();   // start pulse

  return 1; // success
} // if482_init

String IF482_Out(time_t tt) {

  time_t t = myTZ.toLocal(tt);
  char mon, buf[14], out[IF482_FRAME_SIZE];

  switch (timeStatus()) { // indicates if time has been set and recently synced
  case timeSet:           // time is set and is synced
    mon = 'A';
    break;
  case timeNeedsSync: // time had been set but sync attempt did not succeed
    mon = 'M';
    break;
  default: // time not set, no valid time
    mon = '?';
    break;
  } // switch

  // do we have confident time/date?
  if ((timeStatus() == timeSet) || (timeStatus() == timeNeedsSync))
    snprintf(buf, sizeof(buf), "%02u%02u%02u%1u%02u%02u%02u", year(t) - 2000,
             month(t), day(t), weekday(t), hour(t), minute(t), second(t));
  else
    snprintf(buf, sizeof(buf), "000000F000000"); // no confident time/date

  // output IF482 telegram
  snprintf(out, sizeof(out), "O%cL%s\r", mon, buf);
  ESP_LOGD(TAG, "IF482 = %s", out);
  return out;
}

void if482_loop(void *pvParameters) {

  configASSERT(((uint32_t)pvParameters) == 1); // FreeRTOS check

  TickType_t wakeTime;
  const TickType_t timeOffset =
      pdMS_TO_TICKS(IF482_OFFSET); // duration of telegram transmit
  const TickType_t startTime = xTaskGetTickCount(); // now

  sync_clock(now());  // wait until begin of a new second
  BitsPending = true; // start blink in display

  // take timestamp at moment of start of new second
  const TickType_t shotTime = xTaskGetTickCount() - startTime - timeOffset;

  // task remains in blocked state until it is notified by isr
  for (;;) {
    xTaskNotifyWait(
        0x00,           // don't clear any bits on entry
        ULONG_MAX,      // clear all bits on exit
        &wakeTime,      // receives moment of call from isr
        portMAX_DELAY); // wait forever (missing error handling here...)

// select clock scale
#if (PPS == IF482_PULSE_DURATION) // we don't need clock rescaling
    // wait until it's time to start transmit telegram for next second
    vTaskDelayUntil(&wakeTime, shotTime); // sets waketime to moment of shot
    IF482.print(IF482_Out(now() + 1));

#elif (PPS > IF482_PULSE_DURATION) // we need upclocking
    for (uint8_t i = 1; i <= PPS / IF482_PULSE_DURATION; i++) {
      vTaskDelayUntil(&wakeTime, shotTime); // sets waketime to moment of shot
      IF482.print(IF482_Out(now() + 1));
    }

#elif (PPS < IF482_PULSE_DURATION) // we need downclocking
    IF482.print(IF482_Out(now() + 1));
    vTaskDelayUntil(&wakeTime,
                    shotTime - PPS); // sets waketime to moment of shot
#endif
  }
} // if482_loop()

#endif // HAS_IF482