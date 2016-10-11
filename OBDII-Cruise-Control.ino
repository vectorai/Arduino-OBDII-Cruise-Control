/*
  Cruise control implementation. Uses PID controller in a closed-loop system.
  This version only controls throtle, not brakes.
  The objective of this program is to keep vehicle's speed as close as the value set by the driver.
  It can also keep RPM as set by the user via Serial, just for fun.
  Commands issued via Serial have precedence over speed set via RF control.

  Commands (via Serial port):
  - s=80    set target SPEED to 80
  - r=1000  set target RPM to 1000
  - p=10    set THROTLE servoPosition to 10 (degrees)
  - d=<any> disable cruise control

  Hardware:
  - Arduino Uno R3
  - HC-05 BlueTooth module (zs-040) (connects to OBDII)
  - RF 315/433 MHz four button sender and receiver pair
  - ELM327 OBDII BlueTooth adapter (OBD to RS232 interpreter v1.5)
  - Servo motor connected to pull car's throtle (TowerPro MG956R 12kg)
  - Switch button on brake pedal
  - Switch button on throtle pedal
  - Automatic transmission vehicle with standard OBDII connection (this is the most expensive hardware)

  Setup:
  - HC-05 config (AT mode). Should be run only once:
  AT+ORGL
  AT+ROLE=1
  AT+CMODE=0
  AT+BIND=0019,5D,24E54A

  Patches:
  - See OBD.h.patch (0001-Allow-use-of-SoftwareSerial-with-OBD.patch for Git)
    Allow use of SoftwareSerial port for BT communication with OBDII leaving Serial free
    to monitor and send commands.

  Author: Rogério Carvalho Schneider <stockrt@gmail.com>

  Tested on a Hyundai HB20 (Oct, 2016)
*/

#include <SoftwareSerial.h>
#include <Servo.h>
#include <Timer.h>
#include <OBD.h>

#define OBD_RxD 2 // Arduino pin connected to Tx of HC-05 (OBDII)
#define OBD_TxD 3 // Arduino pin connected to Rx of HC-05

#define SERVO_PIN 8
#define BUZZER_PIN 9
#define BRAKE_PIN 10
#define THROTLE_PIN 11

// D0 to A0
// D1 to A1
// D2 to A2
// D3 to A3
// VT to A4
#define BUTTON_A_PIN A0
#define BUTTON_B_PIN A1
#define BUTTON_C_PIN A2
#define BUTTON_D_PIN A3
#define BUTTON_ANY_PIN A4

#define MUST_HOLD_BUTTON_TO_ACTIVATE 2000 // ms
#define MUST_HOLD_BUTTON_TO_CHANGE 500 // ms
#define MIN_SPEED_TO_ACTIVATE 0 // Km/h
#define MAX_WAIT_THROTLE_RELEASE 5000 // ms

int currentSPEED = 0;
int targetSPEED = 0;
int currentRPM = 0;
int targetRPM = 0;
int ethanol = -1;

// - Using INPUT_PULLUP semantics and wiring for pedal buttons:
// HIGH: driver is not using this pedal
// LOW: driver is pressing the pedal
int brakePedalState = LOW;
int throtlePedalState = LOW;

// Servo
int servoPosition = 0;

// RF control
// A, B,C, D, ANY (VT)
int buttonPin[] = {BUTTON_A_PIN, BUTTON_B_PIN, BUTTON_C_PIN, BUTTON_D_PIN};
int buttonState[] = {LOW, LOW, LOW, LOW};
int lastButtonState[] = {LOW, LOW, LOW, LOW};
unsigned long startButtonPressed[4];
boolean buttonStateChangeProcessed[] = {false, false, false, false};

// - Control code
// 0: NO CONTROL
// 1: SPEED
// 2: RPM
// 3: THROTLE servoPosition
int controlCode = 0;
boolean releaseControlFeedback = true;

// Objects
SoftwareSerial btSerial(OBD_RxD, OBD_TxD);
Servo servo;
Timer timer;
COBD obd;

void setup() {
  // Serial config
  Serial.begin(38400);

  // HC-05 communication to OBDII
  btSerial.begin(38400);
  pinMode(OBD_RxD, INPUT);
  pinMode(OBD_TxD, OUTPUT);

  // Greetings
  Serial.println(F(""));
  Serial.println(F("* Initializing..."));

  // Servo for throtle control
  Serial.println(F("* Initializing throtle servo..."));
  pinMode(SERVO_PIN, OUTPUT);
  servo.attach(SERVO_PIN);
  servo.write(servoPosition);

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);

  // Pedal switches
  Serial.println(F("* Initializing pedal switches..."));
  pinMode(BRAKE_PIN, INPUT_PULLUP);
  pinMode(THROTLE_PIN, INPUT_PULLUP);

  // RF control
  pinMode(BUTTON_A_PIN, INPUT);
  pinMode(BUTTON_B_PIN, INPUT);
  pinMode(BUTTON_C_PIN, INPUT);
  pinMode(BUTTON_D_PIN, INPUT);
  pinMode(BUTTON_ANY_PIN, INPUT);

  // BlueTooth connection should be estabilished automatically if we have configured HC-05 correctly
  Serial.println(F("* Initializing OBDII BlueTooth connection..."));
  waitBT();

  // OBDII
  Serial.println(F("* Initializing OBDII library btSerial connection..."));
  obd.begin();

  // Timers
  Serial.println(F("* Initializing timers..."));
  timer.every(100, readPedals);
  timer.every(300, readPIDs);
  timer.every(300, evaluateControl);
  timer.every(2000, showStatus);

  // Setup done
  tone(BUZZER_PIN, 1000, 100);
  delay(500);
  tone(BUZZER_PIN, 1000, 100);
  delay(500);
  tone(BUZZER_PIN, 1000, 100);
  delay(500);
  Serial.println(F("* System initialized!"));
  Serial.println(F("* You may now issue commands: s=<int> r=<int> p=<int> d=<any>"));
}

// Wait until we can communicate with OBDII adapter via HC-05 BlueTooth module
void waitBT() {
  String btRecv;
  int count;

  while (true) {
    btSerial.flush();
    btSerial.println(F("ATZ"));

    count = 0;
    while (count < 200) { // Wait response from OBDII for two seconds at most
      if (btSerial.available()) {
        break;
      }
      count++;
      delay(10);
    }

    if (btSerial.available()) {
      btRecv = btSerial.readString();

      if (btRecv.indexOf(F("ELM327")) != -1) {
        btSerial.flush();
        Serial.println(F("INFO: OBDII ready"));
        break;
      } else {
        Serial.println(F("WARN: OBDII not ready yet (waiting 1s to retry)"));
      }
    } else {
      Serial.println(F("WARN: No response from OBDII (waiting 1s to retry)"));
    }

    delay(1000);
  }
}

void readPedals() {
  //Serial.println(F(""));
  //Serial.println(F("*** Reading pedals ***"));

  brakePedalState = digitalRead(BRAKE_PIN);
  throtlePedalState = digitalRead(THROTLE_PIN);

  if (brakePedalState == LOW || throtlePedalState == LOW) {
    controlCode = 0;
    evaluateControl();
  }
}

void readPIDs() {
  //Serial.println(F(""));
  //Serial.println(F("*** Reading from ECU via OBDII ***"));

  if (! obd.readPID(PID_SPEED, currentSPEED)) {
    Serial.println(F("ERROR: Could not read SPEED from ECU"));
    controlCode = 0;
    evaluateControl();
  }
  if (! obd.readPID(PID_RPM, currentRPM)) {
    Serial.println(F("ERROR: Could not read RPM from ECU"));
    controlCode = 0;
    evaluateControl();
  }
  if (! obd.readPID(PID_ETHANOL_FUEL, ethanol)) {
    Serial.println(F("ERROR: Could not read ETHANOL_FUEL from ECU"));
    controlCode = 0;
    evaluateControl();
  }
}

void releaseControl() {
  servoPosition = 0;
  servo.write(servoPosition);
  servo.detach();

  if (releaseControlFeedback) {
    releaseControlFeedback = false;
    tone(BUZZER_PIN, 200, 100);
    delay(200);
    tone(BUZZER_PIN, 200, 100);
    delay(200);
    tone(BUZZER_PIN, 200, 100);
    delay(200);
  }
}

void waitThrotleReleaseThenActivate() {
  unsigned long startWaitThrotleRelease = millis();

  while (digitalRead(THROTLE_PIN) == LOW && (millis() - startWaitThrotleRelease < MAX_WAIT_THROTLE_RELEASE)) {}

  releaseControlFeedback = true;

  if (millis() - startWaitThrotleRelease < MAX_WAIT_THROTLE_RELEASE) {
    Serial.println(F("*** Cruise control activated! ***"));
    delay(500);
    tone(BUZZER_PIN, 1500, 50);
    delay(100);
    tone(BUZZER_PIN, 1500, 50);
    delay(100);
    controlCode = 1;
    evaluateControl();
  } else {
    Serial.println(F("ERROR: Wait for too long before releasing THROTLE pedal. Command cancelled!"));
    controlCode = 0;
    evaluateControl();
  }
}

void evaluateControl() {
  //Serial.println(F(""));
  //Serial.println(F("*** Evaluating control code ***"));

  switch (controlCode) {
    case 0: // NO CONTROL
      releaseControl();
      break;
    case 1: // SPEED
      releaseControlFeedback = true;
      servo.write(servoPosition);
      servo.attach(SERVO_PIN);
      break;
    case 2: // RPM
      releaseControlFeedback = true;
      servo.write(servoPosition);
      servo.attach(SERVO_PIN);
      break;
    case 3: // THROTLE servoPosition
      releaseControlFeedback = true;
      servo.write(servoPosition);
      servo.attach(SERVO_PIN);
    default: // NO CONTROL
      releaseControl();
      break;
  }
}

void showStatus() {
  Serial.println(F(""));
  Serial.println(F("*** STATUS ***"));

  Serial.print(F("CONTROL code: "));
  Serial.print(String(controlCode));
  Serial.println(F(""));

  Serial.print(F("BRAKE pedal state: "));
  Serial.print(String(brakePedalState));
  Serial.println(F(""));

  Serial.print(F("THROTLE pedal state: "));
  Serial.print(String(throtlePedalState));
  Serial.println(F(""));

  Serial.print(F("SERVO position: "));
  Serial.print(String(servoPosition));
  Serial.println(F(""));

  Serial.print(F("SPEED (current/target): "));
  Serial.print(String(currentSPEED));
  Serial.print(F("/"));
  Serial.print(String(targetSPEED));
  Serial.println(F(""));

  Serial.print(F("RPM (current/target): "));
  Serial.print(String(currentRPM));
  Serial.print(F("/"));
  Serial.print(String(targetRPM));
  Serial.println(F(""));

  Serial.print(F("ETHANOL: "));
  Serial.print(String(ethanol));
  Serial.print(F("%"));
  Serial.println(F(""));
}

void loop() {
  timer.update();

  // Commands from RF
  int i;
  unsigned long buttonHeldMillis;
  for (i = 0; i < sizeof(buttonPin) / sizeof(int); i++) { // for each button
    if (buttonPin[i] == BUTTON_A_PIN || buttonPin[i] == BUTTON_C_PIN) {
      buttonHeldMillis = MUST_HOLD_BUTTON_TO_ACTIVATE;
    } else {
      buttonHeldMillis = MUST_HOLD_BUTTON_TO_CHANGE;
    }
    if (digitalRead(buttonPin[i]) && digitalRead(BUTTON_ANY_PIN)) {
      // Button pressed
      buttonState[i] = HIGH;
      if (buttonState[i] != lastButtonState[i]) {
        lastButtonState[i] = buttonState[i];
        startButtonPressed[i] = millis();
      }
      if ((millis() - startButtonPressed[i] > buttonHeldMillis) && ! buttonStateChangeProcessed[i]) {
        buttonStateChangeProcessed[i] = true;
        switch (buttonPin[i]) {
          case BUTTON_A_PIN:
            if (currentSPEED >= MIN_SPEED_TO_ACTIVATE) {
              targetSPEED = currentSPEED;
              Serial.println(F("ACK Button A pressed (activate/set cruise control)"));
              tone(BUZZER_PIN, 1500, 50);
              delay(100);
              waitThrotleReleaseThenActivate();
            }
            break;
          case BUTTON_C_PIN:
            if (currentSPEED >= MIN_SPEED_TO_ACTIVATE && targetSPEED > 0) {
              Serial.println(F("ACK Button C pressed (reactivate/reset cruise control)"));
              tone(BUZZER_PIN, 1500, 50);
              delay(100);
              waitThrotleReleaseThenActivate();
            }
            break;
          case BUTTON_B_PIN:
            if (controlCode == 1) {
              Serial.println(F("ACK Button B pressed (increase target SPEED by 5)"));
              tone(BUZZER_PIN, 1500, 50);
              delay(100);
              targetSPEED += 5;
            }
            break;
          case BUTTON_D_PIN:
            if (controlCode == 1) {
              Serial.println(F("ACK Button D pressed (decrease target SPEED by 5)"));
              tone(BUZZER_PIN, 1500, 50);
              delay(100);
              targetSPEED -= 5;
            }
            break;
        }
      }
    } else {
      // Button not pressed
      buttonState[i] = LOW;
      lastButtonState[i] = LOW;
      buttonStateChangeProcessed[i] = false;
    }
  }

  // Commands from Serial
  String serialRecv;
  serialRecv = "empty";
  if (Serial.available()) {
    serialRecv = Serial.readString();
    serialRecv.trim();
  }
  if (serialRecv == F("empty")) {
    //Serial.print(F("No command received"));
  } else if (serialRecv.startsWith(F("s="))) {
    targetSPEED = serialRecv.substring(2).toInt();
    Serial.print(F("ACK command (target SPEED set): "));
    Serial.print(String(targetSPEED));
    Serial.print(F(" ("));
    Serial.print(serialRecv);
    Serial.print(F(")"));
    controlCode = 1;
    evaluateControl();
  } else if (serialRecv.startsWith(F("r="))) {
    targetRPM = serialRecv.substring(2).toInt();
    Serial.print(F("ACK command (target RPM set): "));
    Serial.print(String(targetRPM));
    Serial.print(F(" ("));
    Serial.print(serialRecv);
    Serial.print(F(")"));
    controlCode = 2;
    evaluateControl();
  } else if (serialRecv.startsWith(F("p="))) {
    servoPosition = serialRecv.substring(2).toInt();
    Serial.print(F("ACK command (THROTLE servoPosition set): "));
    Serial.print(String(servoPosition));
    Serial.print(F(" ("));
    Serial.print(serialRecv);
    Serial.print(F(")"));
    controlCode = 3;
    evaluateControl();
  } else if (serialRecv.startsWith(F("d="))) {
    Serial.print(F("ACK command (disable control): "));
    Serial.print(F("true"));
    Serial.print(F(" ("));
    Serial.print(serialRecv);
    Serial.print(F(")"));
    controlCode = 0;
    evaluateControl();
  } else {
    Serial.print(F("ERROR: Unknown command: "));
    Serial.print(serialRecv);
    controlCode = 0;
    evaluateControl();
  }
}
