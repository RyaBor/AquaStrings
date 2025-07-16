// --- PIN DEFINITIONS ---
const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 27}; // Photodiode input pins
const int LASER_POWER_PIN = 13;                      // MOSFET pin to control all lasers

// --- GLOBAL CONSTANTS & VARIABLES ---
const int NUM_SENSORS = 5;
const int DEBOUNCE_DELAY = 50;      // How long a state must be stable (in ms) to be confirmed

// --- SET YOUR MANUAL THRESHOLDS HERE ---
// Set a specific threshold value for each sensor.
// The order corresponds to the sensors (Sensor 1, Sensor 2, etc.).
const int thresholds[NUM_SENSORS] = {2000, 2000, 2000, 2000, 2000};

// Array to store the most recent reading from each sensor
int currentReadings[NUM_SENSORS];

// Arrays for debouncing logic
bool previousStates[NUM_SENSORS];       // Stores the last confirmed state (broken or not)
bool pendingStates[NUM_SENSORS];        // Stores the potential new state during the debounce period
unsigned long lastChangeTime[NUM_SENSORS]; // Stores the time of the last detected change

// --- SETUP: INITIALIZATION ---
void setup() {
  // 1. Initialize serial communication
  Serial.begin(115200);
  Serial.println("System starting up with manual thresholds...");

  // 2. Initialize all state-tracking arrays
  for (int i = 0; i < NUM_SENSORS; i++) {
    previousStates[i] = false;
    pendingStates[i] = false;
    lastChangeTime[i] = 0;
    currentReadings[i] = 0;
  }

  // 3. Configure the laser power pin as an output and turn lasers ON
  pinMode(LASER_POWER_PIN, OUTPUT);
  digitalWrite(LASER_POWER_PIN, HIGH);
  
  Serial.println("Lasers are ON. Continuously monitoring for broken beams...");
  
  // Print the manually set thresholds for confirmation
  Serial.println("\n--- Manual Thresholds Loaded ---");
  for(int i = 0; i < NUM_SENSORS; i++) {
    Serial.print("Sensor ");
    Serial.print(i + 1);
    Serial.print(" Threshold: ");
    Serial.println(thresholds[i]);
  }
  Serial.println("--------------------------------\n");
}


// --- MAIN LOOP: CONTINUOUSLY CHECK SENSORS (WITH DEBOUNCING) ---
void loop() {
  for (int i = 0; i < NUM_SENSORS; i++) {
    int photodiodeValue = analogRead(PHOTODIODE_PINS[i]);
    currentReadings[i] = photodiodeValue; // Store the latest reading

    // Determine the current state based on the robust threshold
    // A low value means the beam is blocked.
    bool laserBroken = photodiodeValue < thresholds[i];

    // Check if the reading is different from the last PENDING state.
    if (laserBroken != pendingStates[i]) {
      lastChangeTime[i] = millis(); // Reset the debounce timer
      pendingStates[i] = laserBroken;
    }

    // Check if the state has been stable for longer than the debounce delay
    if ((millis() - lastChangeTime[i]) > DEBOUNCE_DELAY) {
      // If the stable state is different from the last CONFIRMED state, we have a real event.
      if (pendingStates[i] != previousStates[i]) {
        Serial.print("Sensor ");
        Serial.print(i + 1);
        if (pendingStates[i]) {
          Serial.println(": ---> LASER BROKEN <---");
        } else {
          Serial.println(": Laser Reconnected");
        }
        previousStates[i] = pendingStates[i]; // Lock in the new state
      }
    }
  }

  // Print a full status report periodically
  printFullStatus();

  // Small delay to prevent the loop from running too fast
  delay(0);
}


// --- FUNCTION: PRINT PERIODIC FULL STATUS REPORT ---
void printFullStatus() {
  static unsigned long lastPrintTime = 0;
  const long PRINT_INTERVAL = 2000; // Time in ms between reports

  if (millis() - lastPrintTime > PRINT_INTERVAL) {
    Serial.println("\n=== Full System Status Report ===");

    // --- Section to Print Live Readings ---
    Serial.println("--- Live Sensor Readings (Value / Threshold) ---");
    for (int i = 0; i < NUM_SENSORS; i++) {
      Serial.print("Sensor ");
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(currentReadings[i]);
      Serial.print(" / ");
      Serial.print(thresholds[i]);

      if (currentReadings[i] < thresholds[i]) {
        Serial.println("  (State: BROKEN)");
      } else {
        Serial.println("  (State: OK)");
      }
    }
    Serial.println("----------------------------------------------");
    // --- End of Section ---

    String brokenList = "";
    bool anyBroken = false;

    for (int i = 0; i < NUM_SENSORS; i++) {
      if (previousStates[i]) { 
        if (anyBroken) brokenList += ", ";
        brokenList += String(i + 1);
        anyBroken = true;
      }
    }

    Serial.print("Strings Currently Broken (Action State): ");
    Serial.println(anyBroken ? brokenList : "None");
    Serial.println("============================================\n");

    lastPrintTime = millis();
  }
}