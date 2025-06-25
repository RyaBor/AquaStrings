// Simple FreeRTOS 5-Laser System with Individual Laser Control
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ===== PIN CONFIGURATION =====
const int LASER_PINS[5] = {2, 4, 5, 16, 17};      // Pins to control laser power
const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 36}; // Photodiode input pins
const int LED_PINS[5] = {23, 22, 21, 19, 18};        // Status LED pins

// ===== SETTINGS =====
const int THRESHOLD = 2000;        // Detection threshold for all sensors
const int POLL_RATE_MS = 10;       // How fast to check each sensor
const int REPORT_RATE_MS = 100;    // How often to print status

// ===== SHARED DATA =====
int sensorValues[5] = {0};          // Current photodiode readings
bool beamBroken[5] = {false};       // True if beam is broken

// ===== SENSOR TASKS (One per laser/photodiode pair) =====
void sensorTask0(void *parameter) { sensorLoop(0); }
void sensorTask1(void *parameter) { sensorLoop(1); }
void sensorTask2(void *parameter) { sensorLoop(2); }
void sensorTask3(void *parameter) { sensorLoop(3); }
void sensorTask4(void *parameter) { sensorLoop(4); }

// Generic sensor monitoring function
void sensorLoop(int sensorNum) {
  while (true) {
    // Read photodiode
    sensorValues[sensorNum] = analogRead(PHOTODIODE_PINS[sensorNum]);
    
    // Check if beam is broken
    if (sensorValues[sensorNum] < THRESHOLD) {
      beamBroken[sensorNum] = true;
      digitalWrite(LED_PINS[sensorNum], HIGH);  // Turn on status LED
    } else {
      beamBroken[sensorNum] = false;
      digitalWrite(LED_PINS[sensorNum], LOW);   // Turn off status LED
    }
    
    // Wait before next reading
    vTaskDelay(pdMS_TO_TICKS(POLL_RATE_MS));
  }
}

// ===== REPORTING TASK =====
void reportTask(void *parameter) {
  while (true) {
    Serial.println("=== Laser Status ===");
    
    for (int i = 0; i < 5; i++) {
      Serial.print("Laser ");
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(sensorValues[i]);
      Serial.print(" - ");
      Serial.println(beamBroken[i] ? "BROKEN" : "OK");
    }
    
    // Count broken beams
    int brokenCount = 0;
    for (int i = 0; i < 5; i++) {
      if (beamBroken[i]) brokenCount++;
    }
    
    if (brokenCount > 0) {
      Serial.print("*** ");
      Serial.print(brokenCount);
      Serial.println(" BEAMS BROKEN! ***");
    }
    
    Serial.println("==================");
    
    // Wait before next report
    vTaskDelay(pdMS_TO_TICKS(REPORT_RATE_MS));
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  Serial.println("Simple FreeRTOS Laser System Starting...");
  
  // Setup laser control pins (turn lasers ON)
  for (int i = 0; i < 5; i++) {
    pinMode(LASER_PINS[i], OUTPUT);
    digitalWrite(LASER_PINS[i], HIGH);  // Turn laser ON
  }
  
  // Setup status LED pins
  for (int i = 0; i < 5; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);     // Start with LEDs OFF
  }
  
  Serial.println("All lasers powered ON");
  Serial.println("Creating sensor tasks...");
  
  // Create one task per sensor
  xTaskCreate(sensorTask0, "Sensor0", 2048, NULL, 1, NULL);
  xTaskCreate(sensorTask1, "Sensor1", 2048, NULL, 1, NULL);
  xTaskCreate(sensorTask2, "Sensor2", 2048, NULL, 1, NULL);
  xTaskCreate(sensorTask3, "Sensor3", 2048, NULL, 1, NULL);
  xTaskCreate(sensorTask4, "Sensor4", 2048, NULL, 1, NULL);
  
  // Create reporting task
  xTaskCreate(reportTask, "Reporter", 4096, NULL, 1, NULL);
  
  Serial.println("System ready!");
}

// ===== MAIN LOOP =====
void loop() {
  // FreeRTOS handles everything - this just keeps the system alive
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ===== UTILITY FUNCTIONS =====

// Turn specific laser ON or OFF
void controlLaser(int laserNum, bool turnOn) {
  if (laserNum >= 0 && laserNum < 5) {
    digitalWrite(LASER_PINS[laserNum], turnOn ? HIGH : LOW);
    Serial.print("Laser ");
    Serial.print(laserNum + 1);
    Serial.println(turnOn ? " turned ON" : " turned OFF");
  }
}

// Turn all lasers ON or OFF
void controlAllLasers(bool turnOn) {
  for (int i = 0; i < 5; i++) {
    digitalWrite(LASER_PINS[i], turnOn ? HIGH : LOW);
  }
  Serial.println(turnOn ? "All lasers ON" : "All lasers OFF");
}

// Check if any beam is broken
bool anyBeamBroken() {
  for (int i = 0; i < 5; i++) {
    if (beamBroken[i]) return true;
  }
  return false;
}

// Get number of broken beams
int getBrokenBeamCount() {
  int count = 0;
  for (int i = 0; i < 5; i++) {
    if (beamBroken[i]) count++;
  }
  return count;
}