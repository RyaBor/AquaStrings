const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 27}; // Photodiode input pins
const int LED_PINS[5] = {16, 26, 4, 25, 13};           // LED output pins
const int NUM_SENSORS = 5;

// Threshold value to determine if laser is broken
const int THRESHOLD = 512; // Adjust based on your readings

// Variables for timing detection
unsigned long detectionTimes[NUM_SENSORS];
bool previousStates[NUM_SENSORS];
bool timingTest = false; // Set to true to enable timing measurements

// Performance monitoring
unsigned long loopStartTime;
unsigned long maxLoopTime = 0;
unsigned long totalLoops = 0;
unsigned long loopTimeSum = 0;

void setup() {
  Serial.begin(115200);
  
  // Configure LED pins as outputs
  for (int i = 0; i < NUM_SENSORS; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
    previousStates[i] = false;
    detectionTimes[i] = 0;
  }
  
  Serial.println("Laser Break Detection System with Timing Test");
  Serial.println("Commands:");
  Serial.println("  't' - Toggle timing test mode");
  Serial.println("  'r' - Reset timing statistics");
  Serial.println("  's' - Show performance stats");
  Serial.println("=====================================");
}

void loop() {
  loopStartTime = micros(); // Start timing the loop
  
  // Check for serial commands
  if (Serial.available()) {
    char command = Serial.read();
    handleSerialCommand(command);
  }
  
  // Main detection loop
  for (int i = 0; i < NUM_SENSORS; i++) {
    unsigned long readingStartTime = micros();
    int photodiodeValue = analogRead(PHOTODIODE_PINS[i]);
    unsigned long readingTime = micros() - readingStartTime;
    
    bool laserBroken = photodiodeValue < THRESHOLD;
    
    // Detect state change and measure timing
    if (laserBroken != previousStates[i]) {
      unsigned long detectionTime = micros();
      detectionTimes[i] = detectionTime;
      previousStates[i] = laserBroken;
      
      // Control LED
      digitalWrite(LED_PINS[i], laserBroken ? HIGH : LOW);
      
      // Print timing information
      if (timingTest) {
        Serial.print(">>> TIMING: String ");
        Serial.print(i + 1);
        if (laserBroken) {
          Serial.println(" BROKEN detected!");
        } else {
          Serial.println(" RESTORED detected!");
        }
        Serial.print("    Detection timestamp: ");
        Serial.print(detectionTime);
        Serial.println(" microseconds");
        Serial.print("    ADC read time: ");
        Serial.print(readingTime);
        Serial.println(" microseconds");
        Serial.println();
      } else {
        Serial.print("String ");
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.println(laserBroken ? "BROKEN" : "Connected");
      }
    } else {
      // Update LED state even if no change (ensures consistency)
      digitalWrite(LED_PINS[i], laserBroken ? HIGH : LOW);
    }
  }
  
  // Calculate loop performance
  unsigned long loopTime = micros() - loopStartTime;
  totalLoops++;
  loopTimeSum += loopTime;
  if (loopTime > maxLoopTime) {
    maxLoopTime = loopTime;
  }
  
  // Print status periodically (only when not in timing test mode)
  if (!timingTest) {
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 500) {
      printStatus();
      lastPrint = millis();
    }
  }
  
  // Small delay - comment out for maximum speed testing
  delayMicroseconds(100); // 100 microseconds = 0.1ms
}

void handleSerialCommand(char command) {
  switch (command) {
    case 't':
    case 'T':
      timingTest = !timingTest;
      Serial.print("Timing test mode: ");
      Serial.println(timingTest ? "ENABLED" : "DISABLED");
      if (timingTest) {
        Serial.println("Break a laser beam to see detection timing!");
      }
      break;
      
    case 'r':
    case 'R':
      maxLoopTime = 0;
      totalLoops = 0;
      loopTimeSum = 0;
      Serial.println("Timing statistics reset.");
      break;
      
    case 's':
    case 'S':
      printPerformanceStats();
      break;
  }
}

void printStatus() {
  Serial.println("=== LASER STRING STATUS ===");
  
  for (int i = 0; i < NUM_SENSORS; i++) {
    int reading = analogRead(PHOTODIODE_PINS[i]);
    bool isBroken = reading < THRESHOLD;
    
    Serial.print("String ");
    Serial.print(i + 1);
    Serial.print(": Reading = ");
    Serial.print(reading);
    Serial.print(" | Status: ");
    
    if (isBroken) {
      Serial.print("BROKEN");
    } else {
      Serial.print("Connected");
    }
    Serial.println();
  }
  
  // Summary of broken strings
  Serial.print("Broken strings: ");
  bool anyBroken = false;
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (analogRead(PHOTODIODE_PINS[i]) < THRESHOLD) {
      if (anyBroken) Serial.print(", ");
      Serial.print(i + 1);
      anyBroken = true;
    }
  }
  if (!anyBroken) {
    Serial.print("None");
  }
  Serial.println();
  Serial.println();
}

void printPerformanceStats() {
  Serial.println("=== PERFORMANCE STATISTICS ===");
  Serial.print("Total loops executed: ");
  Serial.println(totalLoops);
  
  if (totalLoops > 0) {
    Serial.print("Average loop time: ");
    Serial.print(loopTimeSum / totalLoops);
    Serial.println(" microseconds");
    
    Serial.print("Maximum loop time: ");
    Serial.print(maxLoopTime);
    Serial.println(" microseconds");
    
    Serial.print("Estimated detection frequency: ");
    Serial.print(1000000.0 / (loopTimeSum / totalLoops));
    Serial.println(" Hz");
    
    Serial.print("Theoretical minimum detection time: ");
    Serial.print(loopTimeSum / totalLoops);
    Serial.println(" microseconds");
  }
  Serial.println("===============================");
}