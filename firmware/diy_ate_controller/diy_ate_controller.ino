/**
 * @file diy_ate_controller.ino
 * @brief CD4071 Quad OR Gate Automatic Test Equipment (ATE) Controller Firmware
 * 
 * This firmware operates a 14-relay matrix to test the CD4071 IC. It implements
 * safe capacitor charging/discharging algorithms to manage RC input filters,
 * performs output loading/current measurements, and hosts a Serial CLI for debugging.
 */

#include <Arduino.h>

// --- Relay Types & Structure ---
enum RelayType {
  RELAY_INPUT,
  RELAY_OUTPUT,
  RELAY_POWER
};

struct Relay {
  int id;                  // 1-indexed (1 to 14)
  int controlPin;          // Relay coil control (Active-LOW)
  RelayType type;
  int digitalPin;          // NO/NC digital endpoint (e.g. 44, 36) (-1 for power)
  int analogSense1;        // First analog pin (A_sense1 or A_input) (-1 for power)
  int analogSense2;        // Second analog pin (A_sense2) (-1 if input or power)
  const char* label;       // Descriptive label
};

// --- Complete 14-Relay Matrix Map ---
const Relay RELAYS[] = {
  // id, ctrl, type,          dig, sense1, sense2, label
  {1,  9,    RELAY_INPUT,   44,  A3,      -1,     "Input A1"},
  {2,  8,    RELAY_INPUT,   42,  A2,      -1,     "Input B1"},
  {3,  16,   RELAY_OUTPUT,  36,  A4,      A5,     "Output Q1"},
  {4,  17,   RELAY_OUTPUT,  34,  A6,      A7,     "Output Q2"},
  {5,  7,    RELAY_INPUT,   40,  A12,     -1,     "Input A2"},
  {6,  6,    RELAY_INPUT,   38,  A13,     -1,     "Input B2"},
  {7,  19,   RELAY_POWER,   -1,  -1,      -1,     "GND Pin of IC"},
  {8,  18,   RELAY_POWER,   -1,  -1,      -1,     "VDD Pin of IC"},
  {9,  5,    RELAY_INPUT,   28,  A0,      -1,     "Input A3"},
  {10, 4,    RELAY_INPUT,   26,  A1,      -1,     "Input B3"},
  {11, 14,   RELAY_OUTPUT,  32,  A8,      A9,     "Output Q3"},
  {12, 15,   RELAY_OUTPUT,  30,  A10,     A11,    "Output Q4"},
  {13, 3,    RELAY_INPUT,   24,  A14,     -1,     "Input A4"},
  {14, 2,    RELAY_INPUT,   22,  A15,     -1,     "Input B4"}
};
const int NUM_RELAYS = sizeof(RELAYS) / sizeof(RELAYS[0]);

// --- Global State ---
bool verboseMode = true;

// --- Function Prototypes ---
const Relay* getRelay(int id);
void setPowerState(bool normalPower);
void dischargeCapacitor(int relayId);
void chargeCapacitor(int relayId, float targetVoltage);
void readInputVoltage(int relayId);
void readOutput(int relayId, bool useNC, bool loadToGND);
void runGate1Test();
void runGate2Test();
void runGate2ThresholdTest();
void testGateThresholds(int gateNum, float &vtRise, float &vtFall, float &vOutRise, float &vOutFall, float &iSource, float &iSink);
void runICTest(int batch, int icNum);
void processCommand(String commandLine);
void printHelp();
void runLiveTest(int gateNum);
float sweepLiveThreshold(int gateNum, bool sweepA, bool sweepB, bool sweepUp, bool initialAHigh, bool initialBHigh);
void printField(const char* str, int width);
void printRow(const char* param, const char* testNo, const char* vdd, const char* vin1, const char* vin2, const char* vo, const char* monitor, const char* mon, float measured, const char* minVal, const char* typVal, const char* maxVal, const char* unit, const char* remarks);

// --- Setup ---
void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ; // Wait for serial port to connect
  }

  Serial.println(F("\n========================================="));
  Serial.println(F("       CD4071 ATE CONTROLLER BOOT        "));
  Serial.println(F("========================================="));

  // Initialize all relay control pins
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(RELAYS[i].controlPin, OUTPUT);
    digitalWrite(RELAYS[i].controlPin, HIGH); // Start with all relays INACTIVE (HIGH)
    
    // Set digital endpoints to high-impedance input by default
    if (RELAYS[i].digitalPin != -1) {
      pinMode(RELAYS[i].digitalPin, INPUT);
    }
  }

  // Set power state to normal by default (Relay 7 NC = GND, Relay 8 NC = 5V)
  setPowerState(true);
  
  Serial.println(F("System initialized in safe normal power mode."));
  Serial.println(F("Type 'help' to see the command list."));
  Serial.print(F("ATE> "));
}

// --- Main Loop ---
void loop() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      processCommand(cmd);
      Serial.print(F("ATE> "));
    }
  }
}

// --- Helper Functions ---

const Relay* getRelay(int id) {
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (RELAYS[i].id == id) {
      return &RELAYS[i];
    }
  }
  return nullptr;
}

// Controls Relays 7 (GND) and 8 (VDD)
void setPowerState(bool normalPower) {
  const Relay* rGnd = getRelay(7);
  const Relay* rVdd = getRelay(8);
  
  if (normalPower) {
    // Normal: NC connected. (Relay coil off = HIGH)
    digitalWrite(rGnd->controlPin, HIGH);
    digitalWrite(rVdd->controlPin, HIGH);
    if (verboseMode) {
      Serial.println(F("[POWER] Set to NORMAL (VDD = 5V, GND = GND)"));
    }
  } else {
    // Reversed/Off: NO connected. (Relay coil on = LOW)
    digitalWrite(rGnd->controlPin, LOW);
    digitalWrite(rVdd->controlPin, LOW);
    if (verboseMode) {
      Serial.println(F("[POWER] Set to REVERSED/OFF (VDD = GND, GND = 5V)"));
    }
  }
}

// Safely discharges the capacitor through the 10k resistor
void dischargeCapacitor(int relayId) {
  const Relay* r = getRelay(relayId);
  if (!r || r->type != RELAY_INPUT) {
    Serial.print(F("Error: Relay ")); Serial.print(relayId); Serial.println(F(" is not an Input Relay."));
    return;
  }

  if (verboseMode) {
    Serial.print(F("Discharging capacitor on Relay "));
    Serial.print(relayId);
    Serial.print(F(" ("));
    Serial.print(r->label);
    Serial.println(F(")..."));
  }

  // 1. Close input relay (LOW) to connect RC filter to digital pin
  digitalWrite(r->controlPin, LOW);
  delay(10); // Wait for relay contact travel

  // 2. Set digital pin to OUTPUT LOW to sink charge through the 10k resistor
  pinMode(r->digitalPin, OUTPUT);
  digitalWrite(r->digitalPin, LOW);

  unsigned long startTime = millis();
  float voltage = 5.0;
  int steps = 0;

  // 3. Monitor voltage until it drops below 0.05V (with 3 second timeout)
  while (voltage > 0.05 && (millis() - startTime < 3000)) {
    int raw = analogRead(r->analogSense1);
    voltage = raw * (5.0 / 1023.0);
    
    if (verboseMode && (steps % 20 == 0)) {
      Serial.print(F("  Voltage: ")); Serial.print(voltage); Serial.println(F(" V"));
    }
    steps++;
    delay(10);
  }

  // 4. Open relay (HIGH) and set digital pin back to High-Impedance Input
  digitalWrite(r->controlPin, HIGH);
  pinMode(r->digitalPin, INPUT);

  int finalRaw = analogRead(r->analogSense1);
  float finalVolt = finalRaw * (5.0 / 1023.0);
  if (verboseMode) {
    Serial.print(F("Discharge complete. Final Voltage: "));
    Serial.print(finalVolt);
    Serial.println(F(" V"));
  }
}

// Step-sweep charges the capacitor to target voltage using relay pulses
void chargeCapacitor(int relayId, float targetVoltage) {
  const Relay* r = getRelay(relayId);
  if (!r || r->type != RELAY_INPUT) {
    Serial.print(F("Error: Relay ")); Serial.print(relayId); Serial.println(F(" is not an Input Relay."));
    return;
  }

  if (targetVoltage < 0.0 || targetVoltage > 5.0) {
    Serial.println(F("Error: Target voltage must be between 0.0V and 5.0V."));
    return;
  }

  if (verboseMode) {
    Serial.print(F("Charging capacitor on Relay "));
    Serial.print(relayId);
    Serial.print(F(" ("));
    Serial.print(r->label);
    Serial.print(F(") to "));
    Serial.print(targetVoltage);
    Serial.println(F(" V..."));
  }

  // 1. Configure the charging source pin to OUTPUT HIGH
  pinMode(r->digitalPin, OUTPUT);
  digitalWrite(r->digitalPin, HIGH);

  float currentVolt = analogRead(r->analogSense1) * (5.0 / 1023.0);
  if (verboseMode) {
    Serial.print(F("  Initial Voltage: ")); Serial.print(currentVolt); Serial.println(F(" V"));
  }

  if (currentVolt >= targetVoltage) {
    if (verboseMode) {
      Serial.println(F("  Target voltage already met or exceeded."));
    }
    pinMode(r->digitalPin, INPUT);
    return;
  }

  int stepCount = 0;
  unsigned long totalStart = millis();

  // 2. Perform step-sweep charging loop
  while (currentVolt < targetVoltage && (millis() - totalStart < 15000)) {
    stepCount++;
    
    // Close the relay (active-LOW) for 15ms pulse
    digitalWrite(r->controlPin, LOW);
    delay(15);
    
    // Open the relay (HIGH) to trap charge
    digitalWrite(r->controlPin, HIGH);
    
    // Wait for mechanical release and signal settling
    delay(20);
    
    // Read current voltage
    int raw = analogRead(r->analogSense1);
    currentVolt = raw * (5.0 / 1023.0);

    if (verboseMode) {
      Serial.print(F("  Step ")); Serial.print(stepCount);
      Serial.print(F(": Voltage = ")); Serial.print(currentVolt);
      Serial.println(F(" V"));
    }

    if (currentVolt >= targetVoltage) {
      break;
    }

    // Delay between steps to slow down the charge rate (10k resistor allows controlled rise)
    delay(20); 
  }

  // 3. Set digital pin back to High-Impedance Input
  pinMode(r->digitalPin, INPUT);

  if (verboseMode) {
    Serial.print(F("Charge sweep complete. Steps: "));
    Serial.print(stepCount);
    Serial.print(F(". Final Voltage: "));
    Serial.print(currentVolt);
    Serial.println(F(" V"));
  }
}

// Reads and prints input capacitor voltage
void readInputVoltage(int relayId) {
  const Relay* r = getRelay(relayId);
  if (!r || r->type != RELAY_INPUT) {
    Serial.print(F("Error: Relay ")); Serial.print(relayId); Serial.println(F(" is not an Input Relay."));
    return;
  }
  int raw = analogRead(r->analogSense1);
  float volt = raw * (5.0 / 1023.0);
  Serial.print(r->label);
  Serial.print(F(" (Relay ")); Serial.print(relayId);
  Serial.print(F(") Voltage: "));
  Serial.print(volt);
  Serial.println(F(" V"));
}

// Reads output state either via NC (current & voltage sense) or NO (direct digital read)
void readOutput(int relayId, bool useNC, bool loadToGND) {
  const Relay* r = getRelay(relayId);
  if (!r || r->type != RELAY_OUTPUT) {
    Serial.print(F("Error: Relay ")); Serial.print(relayId); Serial.println(F(" is not an Output Relay."));
    return;
  }

  Serial.print(F("Reading Output on Relay "));
  Serial.print(relayId);
  Serial.print(F(" ("));
  Serial.print(r->label);
  Serial.print(F(") using "));
  Serial.println(useNC ? F("NC (Analog Sense)") : F("NO (Direct Digital)"));

  if (useNC) {
    // 1. Select NC port: set control pin HIGH (coil inactive)
    digitalWrite(r->controlPin, HIGH);
    
    // 2. Set digital endpoint configuration
    if (loadToGND) {
      pinMode(r->digitalPin, OUTPUT);
      digitalWrite(r->digitalPin, LOW); // Pull the 5k resistor network to GND (loads the gate)
    } else {
      pinMode(r->digitalPin, INPUT);     // High impedance endpoint
    }
    
    delay(20); // Settle time
    
    // 3. Read Analog Pins
    int raw1 = analogRead(r->analogSense1); // A_sense1 (IC pin output side)
    int raw2 = analogRead(r->analogSense2); // A_sense2 (digital endpoint side)
    
    float vOut = raw1 * (5.0 / 1023.0);
    float vLoad = raw2 * (5.0 / 1023.0);
    
    // Current in mA: I = (V_out - V_load) / R
    float currentmA = (vOut - vLoad) / 5000.0 * 1000.0;
    
    Serial.print(F("  IC Pin Output Voltage (V1): ")); Serial.print(vOut); Serial.println(F(" V"));
    Serial.print(F("  Endpoint Voltage (V2):      ")); Serial.print(vLoad); Serial.println(F(" V"));
    Serial.print(F("  Calculated Output Current:   ")); Serial.print(currentmA, 4); Serial.println(F(" mA"));
    
    // Clean up
    pinMode(r->digitalPin, INPUT);
    
  } else {
    // 1. Select NO port: set control pin LOW (coil active)
    digitalWrite(r->controlPin, LOW);
    
    // 2. Configure digital endpoint as input
    pinMode(r->digitalPin, INPUT);
    
    delay(20); // Settle time
    
    // 3. Read Digital State
    int state = digitalRead(r->digitalPin);
    
    Serial.print(F("  Direct Digital State: ")); Serial.println(state == HIGH ? F("HIGH (1)") : F("LOW (0)"));
    
    // Clean up
    digitalWrite(r->controlPin, HIGH); // Revert to NC
  }
}

// Performs a full truth table validation on Gate 1 (Inputs A1, B1 -> Output Q1)
void runGate1Test() {
  Serial.println(F("\n========================================="));
  Serial.println(F("     RUNNING CD4071 GATE 1 OR TEST       "));
  Serial.println(F("========================================="));

  setPowerState(true); // Ensure power is normal
  delay(100);
  
  bool pass = true;
  float outV, loadV, current;
  int digState;
  
  // Disable verbose mode during automated sweeps to clean up logs
  bool oldVerbose = verboseMode;
  verboseMode = false;

  // --- CASE 1: 0 OR 0 = 0 ---
  Serial.println(F("Test Case 1: Inputs [A1=0V, B1=0V] -> Expected: Q1 = LOW"));
  dischargeCapacitor(1); // Discharge A1
  dischargeCapacitor(2); // Discharge B1
  delay(200);
  
  // Read using NC (analog load)
  digitalWrite(16, HIGH); // Select NC for Relay 3
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW); // Load to GND
  delay(50);
  outV = analogRead(A4) * (5.0 / 1023.0);
  loadV = analogRead(A5) * (5.0 / 1023.0);
  current = (outV - loadV) / 5.0; // mA
  
  // Read using NO (direct digital)
  digitalWrite(16, LOW); // Select NO
  pinMode(36, INPUT);
  delay(50);
  digState = digitalRead(36);
  digitalWrite(16, HIGH); // Return to NC
  
  Serial.print(F("  Results: Digital Output: ")); Serial.print(digState);
  Serial.print(F(" | Analog Vout: ")); Serial.print(outV); Serial.print(F(" V"));
  Serial.print(F(" | Current: ")); Serial.print(current, 3); Serial.println(F(" mA"));
  
  if (digState == HIGH || outV > 0.8) {
    Serial.println(F("  >> CASE 1 FAILED!"));
    pass = false;
  } else {
    Serial.println(F("  >> CASE 1 PASSED"));
  }
  Serial.println();

  // --- CASE 2: 0 OR 1 = 1 ---
  Serial.println(F("Test Case 2: Inputs [A1=0V, B1=5V] -> Expected: Q1 = HIGH"));
  dischargeCapacitor(1); // Discharge A1
  chargeCapacitor(2, 4.5); // Charge B1 to HIGH
  delay(200);
  
  // Read using NC (analog load)
  digitalWrite(16, HIGH);
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW);
  delay(50);
  outV = analogRead(A4) * (5.0 / 1023.0);
  loadV = analogRead(A5) * (5.0 / 1023.0);
  current = (outV - loadV) / 5.0; // mA
  
  // Read using NO
  digitalWrite(16, LOW);
  pinMode(36, INPUT);
  delay(50);
  digState = digitalRead(36);
  digitalWrite(16, HIGH);
  
  Serial.print(F("  Results: Digital Output: ")); Serial.print(digState);
  Serial.print(F(" | Analog Vout: ")); Serial.print(outV); Serial.print(F(" V"));
  Serial.print(F(" | Current: ")); Serial.print(current, 3); Serial.println(F(" mA"));
  
  if (digState == LOW || outV < 4.0) {
    Serial.println(F("  >> CASE 2 FAILED!"));
    pass = false;
  } else {
    Serial.println(F("  >> CASE 2 PASSED"));
  }
  Serial.println();

  // --- CASE 3: 1 OR 0 = 1 ---
  Serial.println(F("Test Case 3: Inputs [A1=5V, B1=0V] -> Expected: Q1 = HIGH"));
  chargeCapacitor(1, 4.5); // Charge A1
  dischargeCapacitor(2);   // Discharge B1
  delay(200);
  
  // Read using NC (analog load)
  digitalWrite(16, HIGH);
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW);
  delay(50);
  outV = analogRead(A4) * (5.0 / 1023.0);
  loadV = analogRead(A5) * (5.0 / 1023.0);
  current = (outV - loadV) / 5.0; // mA
  
  // Read using NO
  digitalWrite(16, LOW);
  pinMode(36, INPUT);
  delay(50);
  digState = digitalRead(36);
  digitalWrite(16, HIGH);
  
  Serial.print(F("  Results: Digital Output: ")); Serial.print(digState);
  Serial.print(F(" | Analog Vout: ")); Serial.print(outV); Serial.print(F(" V"));
  Serial.print(F(" | Current: ")); Serial.print(current, 3); Serial.println(F(" mA"));
  
  if (digState == LOW || outV < 4.0) {
    Serial.println(F("  >> CASE 3 FAILED!"));
    pass = false;
  } else {
    Serial.println(F("  >> CASE 3 PASSED"));
  }
  Serial.println();

  // --- CASE 4: 1 OR 1 = 1 ---
  Serial.println(F("Test Case 4: Inputs [A1=5V, B1=5V] -> Expected: Q1 = HIGH"));
  chargeCapacitor(1, 4.5); // Charge A1
  chargeCapacitor(2, 4.5); // Charge B1
  delay(200);
  
  // Read using NC (analog load)
  digitalWrite(16, HIGH);
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW);
  delay(50);
  outV = analogRead(A4) * (5.0 / 1023.0);
  loadV = analogRead(A5) * (5.0 / 1023.0);
  current = (outV - loadV) / 5.0; // mA
  
  // Read using NO
  digitalWrite(16, LOW);
  pinMode(36, INPUT);
  delay(50);
  digState = digitalRead(36);
  digitalWrite(16, HIGH);
  
  Serial.print(F("  Results: Digital Output: ")); Serial.print(digState);
  Serial.print(F(" | Analog Vout: ")); Serial.print(outV); Serial.print(F(" V"));
  Serial.print(F(" | Current: ")); Serial.print(current, 3); Serial.println(F(" mA"));
  
  if (digState == LOW || outV < 4.0) {
    Serial.println(F("  >> CASE 4 FAILED!"));
    pass = false;
  } else {
    Serial.println(F("  >> CASE 4 PASSED"));
  }
  Serial.println();

  // Restore verbose mode
  verboseMode = oldVerbose;

  Serial.println(F("========================================="));
  if (pass) {
    Serial.println(F("     OVERALL RESULT: GATE 1 OR PASSED    "));
  } else {
    Serial.println(F("     OVERALL RESULT: GATE 1 OR FAILED    "));
  }
  Serial.println(F("========================================="));

  // Safe State Cleanup: Discharge capacitors
  Serial.println(F("Performing safety discharge of input capacitors..."));
  dischargeCapacitor(1);
  dischargeCapacitor(2);
  Serial.println(F("Safety discharge complete. Ready for next command."));
}

// Performs a full truth table validation on Gate 2 (Inputs A2, B2 -> Output Q2)
void runGate2Test() {
  Serial.println(F("\n========================================="));
  Serial.println(F("     RUNNING CD4071 GATE 2 OR TEST       "));
  Serial.println(F("========================================="));

  setPowerState(true); // Ensure power is normal
  delay(100);
  
  bool pass = true;
  float outV, loadV, current;
  int digState;
  
  // Disable verbose mode during automated sweeps to clean up logs
  bool oldVerbose = verboseMode;
  verboseMode = false;

  // --- CASE 1: 0 OR 0 = 0 ---
  Serial.println(F("Test Case 1: Inputs [A2=0V, B2=0V] -> Expected: Q2 = LOW"));
  dischargeCapacitor(5); // Discharge A2 (Relay 5)
  dischargeCapacitor(6); // Discharge B2 (Relay 6)
  delay(200);
  
  // Read using NC (analog load)
  digitalWrite(17, HIGH); // Select NC for Relay 4
  pinMode(34, OUTPUT);
  digitalWrite(34, LOW); // Load to GND
  delay(50);
  outV = analogRead(A6) * (5.0 / 1023.0);
  loadV = analogRead(A7) * (5.0 / 1023.0);
  current = (outV - loadV) / 5.0; // mA
  
  // Read using NO (direct digital)
  digitalWrite(17, LOW); // Select NO
  pinMode(34, INPUT);
  delay(50);
  digState = digitalRead(34);
  digitalWrite(17, HIGH); // Return to NC
  
  Serial.print(F("  Results: Digital Output: ")); Serial.print(digState);
  Serial.print(F(" | Analog Vout: ")); Serial.print(outV); Serial.print(F(" V"));
  Serial.print(F(" | Current: ")); Serial.print(current, 3); Serial.println(F(" mA"));
  
  if (digState == HIGH || outV > 0.8) {
    Serial.println(F("  >> CASE 1 FAILED!"));
    pass = false;
  } else {
    Serial.println(F("  >> CASE 1 PASSED"));
  }
  Serial.println();

  // --- CASE 2: 0 OR 1 = 1 ---
  Serial.println(F("Test Case 2: Inputs [A2=0V, B2=5V] -> Expected: Q2 = HIGH"));
  dischargeCapacitor(5); // Discharge A2
  chargeCapacitor(6, 4.5); // Charge B2 to HIGH
  delay(200);
  
  // Read using NC (analog load)
  digitalWrite(17, HIGH);
  pinMode(34, OUTPUT);
  digitalWrite(34, LOW);
  delay(50);
  outV = analogRead(A6) * (5.0 / 1023.0);
  loadV = analogRead(A7) * (5.0 / 1023.0);
  current = (outV - loadV) / 5.0; // mA
  
  // Read using NO
  digitalWrite(17, LOW);
  pinMode(34, INPUT);
  delay(50);
  digState = digitalRead(34);
  digitalWrite(17, HIGH);
  
  Serial.print(F("  Results: Digital Output: ")); Serial.print(digState);
  Serial.print(F(" | Analog Vout: ")); Serial.print(outV); Serial.print(F(" V"));
  Serial.print(F(" | Current: ")); Serial.print(current, 3); Serial.println(F(" mA"));
  
  if (digState == LOW || outV < 4.0) {
    Serial.println(F("  >> CASE 2 FAILED!"));
    pass = false;
  } else {
    Serial.println(F("  >> CASE 2 PASSED"));
  }
  Serial.println();

  // --- CASE 3: 1 OR 0 = 1 ---
  Serial.println(F("Test Case 3: Inputs [A2=5V, B2=0V] -> Expected: Q2 = HIGH"));
  chargeCapacitor(5, 4.5); // Charge A2
  dischargeCapacitor(6);   // Discharge B2
  delay(200);
  
  // Read using NC (analog load)
  digitalWrite(17, HIGH);
  pinMode(34, OUTPUT);
  digitalWrite(34, LOW);
  delay(50);
  outV = analogRead(A6) * (5.0 / 1023.0);
  loadV = analogRead(A7) * (5.0 / 1023.0);
  current = (outV - loadV) / 5.0; // mA
  
  // Read using NO
  digitalWrite(17, LOW);
  pinMode(34, INPUT);
  delay(50);
  digState = digitalRead(34);
  digitalWrite(17, HIGH);
  
  Serial.print(F("  Results: Digital Output: ")); Serial.print(digState);
  Serial.print(F(" | Analog Vout: ")); Serial.print(outV); Serial.print(F(" V"));
  Serial.print(F(" | Current: ")); Serial.print(current, 3); Serial.println(F(" mA"));
  
  if (digState == LOW || outV < 4.0) {
    Serial.println(F("  >> CASE 3 FAILED!"));
    pass = false;
  } else {
    Serial.println(F("  >> CASE 3 PASSED"));
  }
  Serial.println();

  // --- CASE 4: 1 OR 1 = 1 ---
  Serial.println(F("Test Case 4: Inputs [A2=5V, B2=5V] -> Expected: Q2 = HIGH"));
  chargeCapacitor(5, 4.5); // Charge A2
  chargeCapacitor(6, 4.5); // Charge B2
  delay(200);
  
  // Read using NC (analog load)
  digitalWrite(17, HIGH);
  pinMode(34, OUTPUT);
  digitalWrite(34, LOW);
  delay(50);
  outV = analogRead(A6) * (5.0 / 1023.0);
  loadV = analogRead(A7) * (5.0 / 1023.0);
  current = (outV - loadV) / 5.0; // mA
  
  // Read using NO
  digitalWrite(17, LOW);
  pinMode(34, INPUT);
  delay(50);
  digState = digitalRead(34);
  digitalWrite(17, HIGH);
  
  Serial.print(F("  Results: Digital Output: ")); Serial.print(digState);
  Serial.print(F(" | Analog Vout: ")); Serial.print(outV); Serial.print(F(" V"));
  Serial.print(F(" | Current: ")); Serial.print(current, 3); Serial.println(F(" mA"));
  
  if (digState == LOW || outV < 4.0) {
    Serial.println(F("  >> CASE 4 FAILED!"));
    pass = false;
  } else {
    Serial.println(F("  >> CASE 4 PASSED"));
  }
  Serial.println();

  // Restore verbose mode
  verboseMode = oldVerbose;

  Serial.println(F("========================================="));
  if (pass) {
    Serial.println(F("     OVERALL RESULT: GATE 2 OR PASSED    "));
  } else {
    Serial.println(F("     OVERALL RESULT: GATE 2 OR FAILED    "));
  }
  Serial.println(F("========================================="));

  // Safe State Cleanup: Discharge capacitors
  Serial.println(F("Performing safety discharge of input capacitors..."));
  dischargeCapacitor(5);
  dischargeCapacitor(6);
  Serial.println(F("Safety discharge complete. Ready for next command."));
}

// Verbose ramp and logic threshold detection for Gate 2
void runGate2ThresholdTest() {
  Serial.println(F("\n========================================="));
  Serial.println(F("   CD4071 GATE 2 THRESHOLD & RAMP TEST   "));
  Serial.println(F("========================================="));

  setPowerState(true); // Ensure VDD = 5V, GND = GND
  delay(100);

  // 1. Initial safety discharge on both inputs
  Serial.println(F("Discharging both inputs to 0V..."));
  dischargeCapacitor(5); // A2
  dischargeCapacitor(6); // B2
  delay(200);

  // 2. Configure output Q2 (Relay 4) to NC (analog measurement) and D34 to high impedance
  digitalWrite(17, HIGH); // NC
  pinMode(34, INPUT);     // High impedance, no output load

  // 3. Configure Input A2 charging pin (Relay 5 digital NO)
  pinMode(40, OUTPUT);
  digitalWrite(40, HIGH); // 5V charging source

  Serial.println(F("\nStarting verbose input ramp sweep on Input A2 (Relay 5). B2 is tied to 0V."));
  Serial.println(F("Logging input vs output state in real-time..."));
  Serial.println(F("----------------------------------------------------------------------"));

  float lastInputV = 0.0;
  bool thresholdReached = false;
  int transitionStep = 0;
  float transitionV = 0.0;

  for (int step = 1; step <= 150; step++) {
    // Pulse Relay 5 closed (active-LOW) for 15ms
    digitalWrite(7, LOW);
    delay(15);
    digitalWrite(7, HIGH); // Trap charge
    
    delay(20); // Settle time
    
    // Read input A2 voltage (A12)
    float inputV = analogRead(A12) * (5.0 / 1023.0);
    
    // Read output Q2 voltage (A6)
    float outputV = analogRead(A6) * (5.0 / 1023.0);
    
    // Read digital logic state of D34
    int logicState = digitalRead(34);

    Serial.print(F("Step "));
    if (step < 10) Serial.print(F(" "));
    Serial.print(step);
    Serial.print(F(": Input A2 = "));
    Serial.print(inputV, 3);
    Serial.print(F(" V | Output Q2 = "));
    Serial.print(outputV, 3);
    Serial.print(F(" V | Logic State = "));
    Serial.println(logicState == HIGH ? F("HIGH (1)") : F("LOW (0)"));

    // Detect logic transition
    if (logicState == HIGH && !thresholdReached) {
      thresholdReached = true;
      transitionStep = step;
      transitionV = inputV;
      Serial.println(F("----------------------------------------------------------------------"));
      Serial.print(F(">>> LOGIC TRANSITION DETECTED at Step "));
      Serial.print(transitionStep);
      Serial.print(F("! Input Voltage: "));
      Serial.print(transitionV, 3);
      Serial.println(F(" V <<<"));
      Serial.println(F("----------------------------------------------------------------------"));
    }

    if (inputV >= 4.70) {
      Serial.println(F("Target voltage reached. Ending sweep."));
      break;
    }

    // Slow down the sweep to 100ms per step for readable real-time streaming
    delay(100);
  }

  // --- START RAMP DOWN SEQUENCE ---
  Serial.println(F("\nStarting verbose input ramp down sweep on Input A2 (Relay 5)..."));
  Serial.println(F("Logging input vs output state in real-time..."));
  Serial.println(F("----------------------------------------------------------------------"));

  // Configure Input A2 discharging pin (Relay 5 digital NO) to output LOW
  pinMode(40, OUTPUT);
  digitalWrite(40, LOW); // 0V discharging sink

  bool rampDownTransition = false;
  int transitionStepDown = 0;
  float transitionVDown = 0.0;

  for (int step = 1; step <= 150; step++) {
    // Pulse Relay 5 closed (active-LOW) for 15ms to drain charge
    digitalWrite(7, LOW);
    delay(15);
    digitalWrite(7, HIGH); // Trap remaining charge
    
    delay(20); // Settle time
    
    // Read input A2 voltage (A12)
    float inputV = analogRead(A12) * (5.0 / 1023.0);
    
    // Read output Q2 voltage (A6)
    float outputV = analogRead(A6) * (5.0 / 1023.0);
    
    // Read digital logic state of D34
    int logicState = digitalRead(34);

    Serial.print(F("Step "));
    if (step < 10) Serial.print(F(" "));
    Serial.print(step);
    Serial.print(F(" (Down): Input A2 = "));
    Serial.print(inputV, 3);
    Serial.print(F(" V | Output Q2 = "));
    Serial.print(outputV, 3);
    Serial.print(F(" V | Logic State = "));
    Serial.println(logicState == HIGH ? F("HIGH (1)") : F("LOW (0)"));

    // Detect logic transition HIGH -> LOW
    if (logicState == LOW && !rampDownTransition) {
      rampDownTransition = true;
      transitionStepDown = step;
      transitionVDown = inputV;
      Serial.println(F("----------------------------------------------------------------------"));
      Serial.print(F(">>> LOGIC TRANSITION DETECTED (HIGH -> LOW) at Step "));
      Serial.print(transitionStepDown);
      Serial.print(F("! Input Voltage: "));
      Serial.print(transitionVDown, 3);
      Serial.println(F(" V <<<"));
      Serial.println(F("----------------------------------------------------------------------"));
    }

    if (inputV <= 0.15) {
      Serial.println(F("Capacitor fully discharged. Ending sweep."));
      break;
    }

    // Slow down the sweep for readability
    delay(100);
  }

  // Disable charge/discharge source
  pinMode(40, INPUT);

  Serial.println(F("----------------------------------------------------------------------"));
  Serial.println(F("Threshold Test Summary:"));
  if (thresholdReached) {
    Serial.print(F("  Logic Transition (LOW -> HIGH):  ")); Serial.print(transitionV, 3); Serial.println(F(" V"));
  } else {
    Serial.println(F("  No logic transition detected (LOW -> HIGH)."));
  }
  if (rampDownTransition) {
    Serial.print(F("  Logic Transition (HIGH -> LOW):  ")); Serial.print(transitionVDown, 3); Serial.println(F(" V"));
  } else {
    Serial.println(F("  No logic transition detected (HIGH -> LOW)."));
  }
  if (thresholdReached && rampDownTransition) {
    float hysteresis = transitionV - transitionVDown;
    Serial.print(F("  Calculated Hysteresis:          ")); Serial.print(hysteresis, 3); Serial.println(F(" V"));
  }
  Serial.println(F("========================================="));

  // 4. Safe State Cleanup
  Serial.println(F("Performing safety discharge of input capacitors..."));
  dischargeCapacitor(5);
  dischargeCapacitor(6);
  Serial.println(F("Safety discharge complete. Ready for next command."));
}

// Measures VT+, VT-, V_out_rise, V_out_fall, I_source, and I_sink for a specific gate
void testGateThresholds(int gateNum, float &vtRise, float &vtFall, float &vOutRise, float &vOutFall, float &iSource, float &iSink) {
  int idA = 0, idB = 0, idQ = 0;
  if (gateNum == 1) { idA = 1; idB = 2; idQ = 3; }
  else if (gateNum == 2) { idA = 5; idB = 6; idQ = 4; }
  else if (gateNum == 3) { idA = 9; idB = 10; idQ = 11; }
  else if (gateNum == 4) { idA = 13; idB = 14; idQ = 12; }
  else return;

  const Relay* rA = getRelay(idA);
  const Relay* rB = getRelay(idB);
  const Relay* rQ = getRelay(idQ);

  // 1. Initial safe state: power IC, discharge inputs
  setPowerState(true);
  delay(50);
  dischargeCapacitor(idA);
  dischargeCapacitor(idB);
  delay(50);

  // --- SWEEP UP PHASE (Source Current Measure) ---
  // Configure Output Relay to NC (analog measurement path)
  digitalWrite(rQ->controlPin, HIGH); // NC closed
  
  // Set digital endpoint pin to OUTPUT LOW (0V sink)
  pinMode(rQ->digitalPin, OUTPUT);
  digitalWrite(rQ->digitalPin, LOW);

  // Configure input A digital pin to OUTPUT HIGH (5V source)
  pinMode(rA->digitalPin, OUTPUT);
  digitalWrite(rA->digitalPin, HIGH);

  bool transitionRise = false;
  vtRise = 0.0;
  vOutRise = 0.0;
  iSource = 0.0;

  for (int step = 1; step <= 150; step++) {
    // Pulse Input Relay active (active-LOW) for 10ms (more granular)
    digitalWrite(rA->controlPin, LOW);
    delay(10);
    digitalWrite(rA->controlPin, HIGH); // Open relay, trap charge
    delay(20); // Settle

    float inputV = analogRead(rA->analogSense1) * (5.0 / 1023.0);
    float v1 = analogRead(rQ->analogSense1) * (5.0 / 1023.0);
    float v2 = analogRead(rQ->analogSense2) * (5.0 / 1023.0);

    // Capture first HIGH transition (IC output voltage v1 >= 2.5V)
    if (v1 >= 2.50 && !transitionRise) {
      transitionRise = true;
      vtRise = inputV;
      vOutRise = v1;
      iSource = (v1 - v2) / 5.0; // In mA
    }

    if (inputV >= 4.50) {
      break;
    }
    delay(20); // Speed up sweep
  }

  // --- SWEEP DOWN PHASE (Sink Current Measure) ---
  // Top off Input A back to 4.5V+
  chargeCapacitor(idA, 4.50);
  delay(50);

  // Set digital endpoint pin to OUTPUT HIGH (5V source)
  pinMode(rQ->digitalPin, OUTPUT);
  digitalWrite(rQ->digitalPin, HIGH);

  // Configure input A digital pin to OUTPUT LOW (0V sink)
  pinMode(rA->digitalPin, OUTPUT);
  digitalWrite(rA->digitalPin, LOW);

  bool transitionFall = false;
  vtFall = 0.0;
  vOutFall = 0.0;
  iSink = 0.0;

  for (int step = 1; step <= 150; step++) {
    // Pulse Input Relay active (active-LOW) for 10ms to drain charge (more granular)
    digitalWrite(rA->controlPin, LOW);
    delay(10);
    digitalWrite(rA->controlPin, HIGH); // Open relay, trap charge
    delay(20); // Settle

    float inputV = analogRead(rA->analogSense1) * (5.0 / 1023.0);
    float v1 = analogRead(rQ->analogSense1) * (5.0 / 1023.0);
    float v2 = analogRead(rQ->analogSense2) * (5.0 / 1023.0);

    // Capture first LOW transition (IC output voltage v1 <= 2.5V)
    if (v1 <= 2.50 && !transitionFall) {
      transitionFall = true;
      vtFall = inputV;
      vOutFall = v1;
      iSink = (v2 - v1) / 5.0; // In mA (current flows v2 -> v1)
    }

    if (inputV <= 0.15) {
      break;
    }
    delay(20);
  }

  // Reset pins to high impedance inputs
  pinMode(rA->digitalPin, INPUT);
  pinMode(rQ->digitalPin, INPUT);

  // Safety discharge at end
  dischargeCapacitor(idA);
  dischargeCapacitor(idB);
}

// Runs automated threshold and current sweeps for all 4 gates of an IC
void runICTest(int batch, int icNum) {
  // Store verbose mode state
  bool oldVerbose = verboseMode;
  verboseMode = false; // Mute sweep details

  Serial.println(F("\n========================================="));
  Serial.print(F("CD4071 TEST RESULT: BATCH ")); Serial.print(batch);
  Serial.print(F(", IC SAMPLE ")); Serial.println(icNum);
  Serial.println(F("========================================="));
  Serial.println(F("Batch,IC_Sample,Gate,VT_Rise(V),VT_Fall(V),V_Out_Rise(V),V_Out_Fall(V),I_Source(mA),I_Sink(mA)"));

  for (int g = 1; g <= 4; g++) {
    float vtRise = 0.0;
    float vtFall = 0.0;
    float vOutRise = 0.0;
    float vOutFall = 0.0;
    float iSource = 0.0;
    float iSink = 0.0;

    testGateThresholds(g, vtRise, vtFall, vOutRise, vOutFall, iSource, iSink);

    Serial.print(batch); Serial.print(F(","));
    Serial.print(icNum); Serial.print(F(","));
    Serial.print(g); Serial.print(F(","));
    Serial.print(vtRise, 3); Serial.print(F(","));
    Serial.print(vtFall, 3); Serial.print(F(","));
    Serial.print(vOutRise, 3); Serial.print(F(","));
    Serial.print(vOutFall, 3); Serial.print(F(","));
    Serial.print(iSource, 3); Serial.print(F(","));
    Serial.print(iSink, 3); Serial.println();
    delay(100);
  }
  Serial.println(F("========================================="));

  verboseMode = oldVerbose; // Restore verbose state
}

// Parses and routes CLI inputs
void processCommand(String commandLine) {
  commandLine.trim();
  int spaceIndex = commandLine.indexOf(' ');
  String cmd = (spaceIndex == -1) ? commandLine : commandLine.substring(0, spaceIndex);
  String args = (spaceIndex == -1) ? "" : commandLine.substring(spaceIndex + 1);
  args.trim();

  cmd.toLowerCase();

  if (cmd == "help") {
    printHelp();
  } 
  else if (cmd == "power") {
    args.toLowerCase();
    if (args == "on" || args == "normal") {
      setPowerState(true);
    } else if (args == "off" || args == "reverse") {
      setPowerState(false);
    } else {
      Serial.println(F("Usage: power [on/off/normal/reverse]"));
    }
  } 
  else if (cmd == "discharge") {
    if (args.length() == 0) {
      Serial.println(F("Usage: discharge <relay_id (1-14)>"));
      return;
    }
    int id = args.toInt();
    dischargeCapacitor(id);
  } 
  else if (cmd == "charge") {
    int nextSpace = args.indexOf(' ');
    if (nextSpace == -1) {
      Serial.println(F("Usage: charge <relay_id (1-14)> <target_voltage>"));
      return;
    }
    int id = args.substring(0, nextSpace).toInt();
    float target = args.substring(nextSpace + 1).toFloat();
    chargeCapacitor(id, target);
  } 
  else if (cmd == "read") {
    if (args.length() == 0) {
      Serial.println(F("Usage: read <input_relay_id (1-14)>"));
      return;
    }
    int id = args.toInt();
    readInputVoltage(id);
  } 
  else if (cmd == "readout") {
    int nextSpace = args.indexOf(' ');
    int id = 0;
    String mode = "nc";
    String load = "load";

    if (nextSpace == -1) {
      if (args.length() == 0) {
        Serial.println(F("Usage: readout <output_relay_id (3,4,11,12)> [nc/no] [load/highz]"));
        return;
      }
      id = args.toInt();
    } else {
      id = args.substring(0, nextSpace).toInt();
      String remainder = args.substring(nextSpace + 1);
      remainder.trim();
      int loadSpace = remainder.indexOf(' ');
      if (loadSpace == -1) {
        mode = remainder;
      } else {
        mode = remainder.substring(0, loadSpace);
        load = remainder.substring(loadSpace + 1);
      }
    }
    
    mode.toLowerCase();
    load.toLowerCase();
    
    bool useNC = (mode != "no");
    bool loadToGND = (load != "highz");
    readOutput(id, useNC, loadToGND);
  } 
  else if (cmd == "test") {
    args.toLowerCase();
    if (args == "gate1") {
      runGate1Test();
    } else if (args == "gate2") {
      runGate2Test();
    } else if (args == "threshold2") {
      runGate2ThresholdTest();
    } else if (args.startsWith("ic ")) {
      // Parse batch and icNum from "ic <batch> <icNum>"
      String sub = args.substring(3);
      sub.trim();
      int spaceIdx = sub.indexOf(' ');
      if (spaceIdx == -1) {
        Serial.println(F("Usage: test ic <batch> <ic_num>"));
        return;
      }
      int batch = sub.substring(0, spaceIdx).toInt();
      int icNum = sub.substring(spaceIdx + 1).toInt();
      runICTest(batch, icNum);
    } else if (args.startsWith("live")) {
      int gateNum = 0;
      if (args.length() > 4) {
        String gateStr = args.substring(4);
        gateStr.trim();
        if (gateStr.length() > 0) {
          gateNum = gateStr.toInt();
        }
      }
      if (gateNum > 0 && gateNum <= 4) {
        runLiveTest(gateNum);
      } else if (gateNum == 0) {
        // Run all 4 gates
        for (int g = 1; g <= 4; g++) {
          runLiveTest(g);
        }
      } else {
        Serial.println(F("Usage: test live [gate_num (1-4)]"));
      }
    } else {
      Serial.println(F("Usage: test [gate1|gate2|threshold2|ic <batch> <ic_num>|live [gate_num]]"));
    }
  } 
  else if (cmd == "verbose") {
    args.toLowerCase();
    if (args == "on") {
      verboseMode = true;
      Serial.println(F("Verbose output: ON"));
    } else if (args == "off") {
      verboseMode = false;
      Serial.println(F("Verbose output: OFF"));
    } else {
      Serial.print(F("Verbose mode is currently: "));
      Serial.println(verboseMode ? F("ON") : F("OFF"));
    }
  }
  else {
    Serial.print(F("Unknown command '")); Serial.print(cmd); Serial.println(F("'. Type 'help' for options."));
  }
}

// Prints user help guide
void printHelp() {
  Serial.println(F("\nAvailable Commands:"));
  Serial.println(F("  help                               - Show this menu"));
  Serial.println(F("  power [on/normal/off/reverse]      - Control IC VDD/GND power Relays 7 & 8"));
  Serial.println(F("  discharge <relay_id>               - Safely discharge capacitor (Input Relays: 1,2,5,6,9,10,13,14)"));
  Serial.println(F("  charge <relay_id> <target_volt>    - Step-sweep charge capacitor to target voltage (0-5.0V)"));
  Serial.println(F("  read <relay_id>                    - Read current voltage at input capacitor"));
  Serial.println(F("  readout <relay_id> [nc/no] [load/highz] - Read Output (NC = Analog current-delta, NO = Digital)"));
  Serial.println(F("  test gate1                         - Run automated truth table & load test for Gate 1"));
  Serial.println(F("  test gate2                         - Run automated truth table & load test for Gate 2"));
  Serial.println(F("  test threshold2                    - Run verbose voltage ramp & threshold test for Gate 2"));
  Serial.println(F("  test ic <batch> <ic_num>           - Run full characterization sweep (CSV) for all 4 gates"));
  Serial.println(F("  test live [gate_num]               - Run live datasheet test on specified gate or all gates"));
  Serial.println(F("  verbose [on/off]                   - View/Toggle verbose sweep details"));
  Serial.println();
}

void printField(const char* str, int width) {
  int len = strlen(str);
  Serial.print(str);
  for (int i = len; i < width; i++) {
    Serial.print(' ');
  }
}

void printRow(const char* param, const char* testNo, const char* vdd, const char* vin1, const char* vin2, const char* vo, const char* monitor, const char* mon, float measured, const char* minVal, const char* typVal, const char* maxVal, const char* unit, const char* remarks) {
  char measStr[16];
  if (measured < -90.0) {
    strcpy(measStr, "-");
  } else {
    dtostrf(measured, 5, 3, measStr);
  }

  Serial.print(F("| ")); printField(param, 34);
  Serial.print(F(" | ")); printField(testNo, 8);
  Serial.print(F(" | ")); printField(vdd, 5);
  Serial.print(F(" | ")); printField(vin1, 5);
  Serial.print(F(" | ")); printField(vin2, 5);
  Serial.print(F(" | ")); printField(vo, 6);
  Serial.print(F(" | ")); printField(monitor, 10);
  Serial.print(F(" | ")); printField(mon, 3);
  Serial.print(F(" | ")); printField(measStr, 8);
  Serial.print(F(" | ")); printField(minVal, 6);
  Serial.print(F(" | ")); printField(typVal, 6);
  Serial.print(F(" | ")); printField(maxVal, 6);
  Serial.print(F(" | ")); printField(unit, 4);
  Serial.print(F(" | ")); printField(remarks, 36);
  Serial.println(F(" |"));
}

float sweepLiveThreshold(int gateNum, bool sweepA, bool sweepB, bool sweepUp, bool initialAHigh, bool initialBHigh) {
  int idA = 0, idB = 0, idQ = 0;
  if (gateNum == 1) { idA = 1; idB = 2; idQ = 3; }
  else if (gateNum == 2) { idA = 5; idB = 6; idQ = 4; }
  else if (gateNum == 3) { idA = 9; idB = 10; idQ = 11; }
  else if (gateNum == 4) { idA = 13; idB = 14; idQ = 12; }
  else return 0.0;

  const Relay* rA = getRelay(idA);
  const Relay* rB = getRelay(idB);
  const Relay* rQ = getRelay(idQ);

  setPowerState(true);
  delay(10);

  if (initialAHigh) {
    chargeCapacitor(idA, 4.5);
  } else {
    dischargeCapacitor(idA);
  }

  if (initialBHigh) {
    chargeCapacitor(idB, 4.5);
  } else {
    dischargeCapacitor(idB);
  }
  delay(20);

  digitalWrite(rQ->controlPin, HIGH);
  pinMode(rQ->digitalPin, INPUT);

  if (sweepA) {
    pinMode(rA->digitalPin, OUTPUT);
    digitalWrite(rA->digitalPin, sweepUp ? HIGH : LOW);
  } else {
    pinMode(rA->digitalPin, INPUT);
  }

  if (sweepB) {
    pinMode(rB->digitalPin, OUTPUT);
    digitalWrite(rB->digitalPin, sweepUp ? HIGH : LOW);
  } else {
    pinMode(rB->digitalPin, INPUT);
  }
  delay(20);

  float transitionVolt = 0.0;
  bool transitionDetected = false;

  for (int step = 1; step <= 150; step++) {
    if (sweepA) digitalWrite(rA->controlPin, LOW);
    if (sweepB) digitalWrite(rB->controlPin, LOW);
    delay(10);
    if (sweepA) digitalWrite(rA->controlPin, HIGH);
    if (sweepB) digitalWrite(rB->controlPin, HIGH);
    delay(20);

    float voltA = analogRead(rA->analogSense1) * (5.0 / 1023.0);
    float voltB = analogRead(rB->analogSense1) * (5.0 / 1023.0);
    float vOut = analogRead(rQ->analogSense1) * (5.0 / 1023.0);

    float primaryInputVolt = sweepA ? voltA : voltB;
    if (sweepA && sweepB) primaryInputVolt = (voltA + voltB) / 2.0;

    if (sweepUp) {
      if (vOut >= 2.50 && !transitionDetected) {
        transitionDetected = true;
        transitionVolt = primaryInputVolt;
        break;
      }
      if (primaryInputVolt >= 4.50) break;
    } else {
      if (vOut <= 2.50 && !transitionDetected) {
        transitionDetected = true;
        transitionVolt = primaryInputVolt;
        break;
      }
      if (primaryInputVolt <= 0.15) break;
    }
  }

  pinMode(rA->digitalPin, INPUT);
  pinMode(rB->digitalPin, INPUT);
  dischargeCapacitor(idA);
  dischargeCapacitor(idB);

  return transitionVolt;
}

void runLiveTest(int gateNum) {
  if (gateNum < 1 || gateNum > 4) return;

  int idA = 0, idB = 0, idQ = 0;
  if (gateNum == 1) { idA = 1; idB = 2; idQ = 3; }
  else if (gateNum == 2) { idA = 5; idB = 6; idQ = 4; }
  else if (gateNum == 3) { idA = 9; idB = 10; idQ = 11; }
  else if (gateNum == 4) { idA = 13; idB = 14; idQ = 12; }
  else return;

  const Relay* rA = getRelay(idA);
  const Relay* rB = getRelay(idB);
  const Relay* rQ = getRelay(idQ);

  bool oldVerbose = verboseMode;
  verboseMode = false;

  Serial.println();
  Serial.print(F("========================================================================================================================================\n"));
  Serial.print(F("                                             CD4071 LIVE DATASHEET TEST: GATE ")); Serial.print(gateNum); Serial.println(F("                                             "));
  Serial.print(F("========================================================================================================================================\n"));
  Serial.print(F("| Parameter                          | Test No. | Vdd   | Vin1  | Vin2  | Vo     | Monitor    | MON | Measured | Min    | Typ    | Max    | Unit | Remarks                                  |\n"));
  Serial.print(F("+------------------------------------+----------+-------+-------+-------+--------+------------+-----+----------+--------+--------+--------+------+------------------------------------------+\n"));

  setPowerState(true);
  delay(50);

  // --- 1. VOL ---
  dischargeCapacitor(idA);
  dischargeCapacitor(idB);
  delay(100);
  digitalWrite(rQ->controlPin, HIGH);
  pinMode(rQ->digitalPin, INPUT);
  delay(20);
  float vol = analogRead(rQ->analogSense1) * (5.0 / 1023.0);
  printRow("Output Low Voltage (VOL) b'00", "Test 1", "5.0", "0.0", "0.0", "VOL", "VO", "DC", vol, "-", "0.0", "0.05", "V", "Inputs: A=0V, B=0V (unloaded)");

  // --- 2. VOH_0 (Inputs: A=0, B=5V) ---
  dischargeCapacitor(idA);
  chargeCapacitor(idB, 4.5);
  delay(100);
  float voh0 = analogRead(rQ->analogSense1) * (5.0 / 1023.0);
  printRow("Output High Voltage (VOH_0) b'01", "Test 1", "5.0", "0.0", "5.0", "VOH_0", "VO", "DC", voh0, "4.95", "5.0", "-", "V", "Inputs: A=0V, B=5V (unloaded)");

  // --- 3. VOH_1 (Inputs: A=5V, B=0) ---
  chargeCapacitor(idA, 4.5);
  dischargeCapacitor(idB);
  delay(100);
  float voh1 = analogRead(rQ->analogSense1) * (5.0 / 1023.0);
  printRow("Output High Voltage (VOH_1) b'10", "Test 1", "5.0", "5.0", "0.0", "VOH_1", "VO", "DC", voh1, "4.95", "5.0", "-", "V", "Inputs: A=5V, B=0V (unloaded)");

  // --- 4. VOH_2 (Inputs: A=5V, B=5V) ---
  chargeCapacitor(idA, 4.5);
  chargeCapacitor(idB, 4.5);
  delay(100);
  float voh2 = analogRead(rQ->analogSense1) * (5.0 / 1023.0);
  printRow("Output High Voltage (VOH_2) b'11", "Test 1", "5.0", "5.0", "5.0", "VOH_2", "VO", "DC", voh2, "4.95", "5.0", "-", "V", "Inputs: A=5V, B=5V (unloaded)");

  // --- 5. VIH sweeps ---
  float vih01 = sweepLiveThreshold(gateNum, false, true, true, false, false);
  printRow("Input High Voltage (VIH_00_01)", "Test 2.1", "5.0", "0.0", "sweep", "L->H", "VO", "DC", vih01, "3.5", "2.75", "-", "V", "Vin Slow Sweep up (B sweeps, A=0)");

  float vih10 = sweepLiveThreshold(gateNum, true, false, true, false, false);
  printRow("Input High Voltage (VIH_00_10)", "Test 2.2", "5.0", "sweep", "0.0", "L->H", "VO", "DC", vih10, "3.5", "2.75", "-", "V", "Vin Slow Sweep up (A sweeps, B=0)");

  float vih11 = sweepLiveThreshold(gateNum, true, true, true, false, false);
  printRow("Input High Voltage (VIH_00_11)", "Test 2.3", "5.0", "sweep", "sweep", "L->H", "VO", "DC", vih11, "3.5", "2.75", "-", "V", "Vin Slow Sweep up (A & B sweep)");

  // --- 6. VIL sweeps ---
  float vil01 = sweepLiveThreshold(gateNum, false, true, false, false, true);
  printRow("Input Low Voltage (VIL_01_00)", "Test 3.1", "5.0", "0.0", "sweep", "H->L", "VO", "DC", vil01, "-", "2.25", "1.5", "V", "Vin Slow Sweep dn (B sweeps, A=0)");

  float vil10 = sweepLiveThreshold(gateNum, true, false, false, true, false);
  printRow("Input Low Voltage (VIL_10_00)", "Test 3.2", "5.0", "sweep", "0.0", "H->L", "VO", "DC", vil10, "-", "2.25", "1.5", "V", "Vin Slow Sweep dn (A sweeps, B=0)");

  float vil11 = sweepLiveThreshold(gateNum, true, true, false, true, true);
  printRow("Input Low Voltage (VIL_11_00)", "Test 3.3", "5.0", "sweep", "sweep", "H->L", "VO", "DC", vil11, "-", "2.25", "1.5", "V", "Vin Slow Sweep dn (A & B sweep)");

  // --- 7. IOH ---
  dischargeCapacitor(idA);
  chargeCapacitor(idB, 4.5);
  delay(50);
  digitalWrite(rQ->controlPin, HIGH);
  pinMode(rQ->digitalPin, OUTPUT);
  digitalWrite(rQ->digitalPin, LOW);
  delay(50);
  float vOut_source = analogRead(rQ->analogSense1) * (5.0 / 1023.0);
  float vLoad_source = analogRead(rQ->analogSense2) * (5.0 / 1023.0);
  float ioh = (vOut_source - vLoad_source) / 5.0;
  pinMode(rQ->digitalPin, INPUT);
  printRow("Output High Source Current (IOH)", "Test 4", "5.0", "0.0", "5.0", "4.5", "I_VO_GND", "DC", ioh, "1.0", "2.5", "-", "mA", "Measured across 5k load resistor");

  // --- 8. IOL ---
  dischargeCapacitor(idA);
  dischargeCapacitor(idB);
  delay(50);
  digitalWrite(rQ->controlPin, HIGH);
  pinMode(rQ->digitalPin, OUTPUT);
  digitalWrite(rQ->digitalPin, HIGH);
  delay(50);
  float vOut_sink = analogRead(rQ->analogSense1) * (5.0 / 1023.0);
  float vLoad_sink = analogRead(rQ->analogSense2) * (5.0 / 1023.0);
  float iol = (vLoad_sink - vOut_sink) / 5.0;
  pinMode(rQ->digitalPin, INPUT);
  printRow("Output High Sink Current (IOL)", "Test 5", "5.0", "0.0", "0.0", "0.4", "I_VDD_VO", "DC", iol, "0.64", "1.6", "-", "mA", "Measured across 5k load resistor");

  dischargeCapacitor(idA);
  dischargeCapacitor(idB);

  Serial.print(F("+------------------------------------+----------+-------+-------+-------+--------+------------+-----+----------+--------+--------+--------+------+------------------------------------------+\n"));
  verboseMode = oldVerbose;
}
