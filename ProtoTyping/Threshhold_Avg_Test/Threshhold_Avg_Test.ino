// --- PIN DEFINITIONS ---
const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 27}; // Photodiode input pins
const int LASER_POWER_PIN = 13;                      // MOSFET pin to control all lasers

// --- GLOBAL CONSTANTS & VARIABLES ---
const int NUM_SENSORS = 5;
const int CALIBRATION_SAMPLES = 50; // Number of readings to average for calibration
const int DEBOUNCE_DELAY = 50;      // How long a state must be stable (in ms) to be confirmed

// Array to store the calculated threshold for each sensor
int thresholds[NUM_SENSORS];

// Arrays for debouncing logic
bool previousStates[NUM_SENSORS];       // Stores the last confirmed state (broken or not)
bool pendingStates[NUM_SENSORS];        // Stores the potential new state during the debounce period
unsigned long lastChangeTime[NUM_SENSORS]; // Stores the time of the last detected change

// --- SETUP: INITIALIZATION AND CALIBRATION ---
void setup() {
  // 1. Initialize serial communication
  Serial.begin(115200);
  Serial.println("System starting up...");

  // 2. Initialize all state-tracking arrays
  for (int i = 0; i < NUM_SENSORS; i++) {
    previousStates[i] = false; // All lasers start as "reconnected"
    pendingStates[i] = false;
    lastChangeTime[i] = 0;
  }

  // 3. Configure the laser power pin as an output
  pinMode(LASER_POWER_PIN, OUTPUT);
  digitalWrite(LASER_POWER_PIN, LOW); // Lasers start off

  // 4. Run the calibration routine to find unique thresholds
  calibrateSensors();

  // 5. Turn lasers ON for continuous monitoring
  digitalWrite(LASER_POWER_PIN, HIGH);
  Serial.println("Lasers are now ON. Continuously monitoring for broken beams...");
}


// --- MAIN LOOP: CONTINUOUSLY CHECK SENSORS (WITH DEBOUNCING) ---
void loop() {
  for (int i = 0; i < NUM_SENSORS; i++) {
    int photodiodeValue = analogRead(PHOTODIODE_PINS[i]);
    
    // Determine the current state based on the robust threshold
    // A low value means the beam is blocked.
    bool laserBroken = photodiodeValue < thresholds[i];

    // Check if the reading is different from the last PENDING state.
    // This detects the very beginning of a potential change.
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
        // *** MODIFIED LOGIC: Swapped the output messages as requested ***
        if (pendingStates[i]) {
          Serial.println(": Laser Reconnected"); // This now prints when the beam is broken
        } else {
          Serial.println(": ---> LASER BROKEN <---"); // This now prints when the beam is reconnected
        }
        previousStates[i] = pendingStates[i]; // Lock in the new state
      }
    }
  }

  // Print a full status report periodically
  printFullStatus();
  
  // Small delay to prevent the loop from running too fast
  delay(10);
}


// --- FUNCTION: AUTOMATIC SENSOR CALIBRATION ---
void calibrateSensors() {
  long laserOnReadings[NUM_SENSORS] = {0};
  long ambientReadings[NUM_SENSORS] = {0};

  Serial.println("\n--- Starting Sensor Calibration ---");

  // Step A: Get readings with lasers ON
  Serial.println("Turning lasers ON for calibration...");
  digitalWrite(LASER_POWER_PIN, HIGH);
  delay(500);

  for (int i = 0; i < NUM_SENSORS; i++) {
    long sum = 0;
    for (int j = 0; j < CALIBRATION_SAMPLES; j++) {
      sum += analogRead(PHOTODIODE_PINS[i]);
      delay(2);
    }
    laserOnReadings[i] = sum / CALIBRATION_SAMPLES;
  }
  
  // Step B: Get readings with lasers OFF (ambient light)
  Serial.println("Turning lasers OFF for calibration...");
  digitalWrite(LASER_POWER_PIN, LOW);
  delay(500);

  for (int i = 0; i < NUM_SENSORS; i++) {
    long sum = 0;
    for (int j = 0; j < CALIBRATION_SAMPLES; j++) {
      sum += analogRead(PHOTODIODE_PINS[i]);
      delay(2);
    }
    ambientReadings[i] = sum / CALIBRATION_SAMPLES;
  }

  // Step C: Calculate the robust threshold for each sensor
  Serial.println("\n--- Calibration Results ---");
  for (int i = 0; i < NUM_SENSORS; i++) {
    // Calculate the total range between on and off
    long range = laserOnReadings[i] - ambientReadings[i];
    // Set the threshold 75% of the way from ambient to on for better noise rejection
    thresholds[i] = ambientReadings[i] + (range * 0.75);
    
    Serial.print("Sensor ");
    Serial.print(i + 1);
    Serial.print(": Laser ON Avg = ");
    Serial.print(laserOnReadings[i]);
    Serial.print(", Ambient Avg = ");
    Serial.print(ambientReadings[i]);
    Serial.print(" -> Calculated Threshold = ");
    Serial.println(thresholds[i]);
  }
  Serial.println("--- Calibration Complete ---\n");
}


// --- FUNCTION: PRINT PERIODIC FULL STATUS REPORT ---
void printFullStatus() {
  static unsigned long lastPrintTime = 0;
  
  if (millis() - lastPrintTime > 2000) {
    Serial.println("\n=== Full System Status Report ===");
    
    String reconnectedList = ""; // Renamed for clarity based on swapped logic
    bool anyReconnected = false;

    for (int i = 0; i < NUM_SENSORS; i++) {
      // Report based on the last confirmed (debounced) state
      // With the swapped logic, previousStates[i] being true now means "Reconnected"
      if (previousStates[i]) {
        if (anyReconnected) reconnectedList += ", ";
        reconnectedList += String(i + 1);
        anyReconnected = true;
      }
    }
    
    // *** MODIFIED LOGIC: Changed reporting message to align with swapped output ***
    Serial.print("Strings Currently 'Reconnected' (Action State): ");
    Serial.println(anyReconnected ? reconnectedList : "None");
    Serial.println("=============================\n");
    
    lastPrintTime = millis();
  }
}