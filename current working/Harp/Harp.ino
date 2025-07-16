// --- INCLUDES ---
#include <Audio.h> 
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"

// --- HARDWARE & PIN DEFINITIONS ---
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 26
#define SD_CS_PIN    5
#define SPI_MOSI_PIN 23
#define SPI_MISO_PIN 19
#define SPI_SCK_PIN  18
const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 27};
const int LASER_POWER_PIN = 13; 

// --- GLOBAL CONSTANTS ---
const int NUM_SENSORS = 5;

// --- SENSOR CALIBRATION & DEBOUNCING ---
const int CALIBRATION_SAMPLES = 50;
const int DEBOUNCE_DELAY = 50;
int thresholds[NUM_SENSORS];
bool previousStates[NUM_SENSORS];
bool pendingStates[NUM_SENSORS];
unsigned long lastChangeTime[NUM_SENSORS];

// --- AUDIO FILE CONFIGURATION ---
const char* noteFiles[NUM_SENSORS] = {
    "/HNotes/C.wav", "/HNotes/D.wav", "/HNotes/E.wav",
    "/HNotes/G.wav", "/HNotes/A.wav"
};

// --- HARDWARE & LIBRARY OBJECTS ---
Adafruit_TPA2016 audioamp = Adafruit_TPA2016();
Audio audio; 

// --- FUNCTION PROTOTYPES ---
void calibrateSensors();

// =================================================================
// ---                           SETUP                           ---
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Laser Harp System (Single-Task Blocking Architecture) ---");

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!audioamp.begin()) {
        Serial.println("FATAL: TPA2016 amplifier not found."); while (1);
    }
    audioamp.setGain(15);
    Serial.printf("TPA2016 amplifier found! Gain: %d dB.\n", audioamp.getGain());
    
    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("FATAL: SD Card mount failed!"); while (true);
    }
    Serial.println("SD Card OK.");

    audio.setVolume(21); 

    pinMode(LASER_POWER_PIN, OUTPUT);
    digitalWrite(LASER_POWER_PIN, LOW);

    for (int i = 0; i < NUM_SENSORS; i++) {
        previousStates[i] = false; 
        pendingStates[i] = false; 
        lastChangeTime[i] = 0;
    }
    
    calibrateSensors();

    digitalWrite(LASER_POWER_PIN, HIGH);
    Serial.println("\n--- System Ready! ---");
}

// =================================================================
// ---                         MAIN LOOP                         ---
// =================================================================
void loop() {
    // If audio is currently playing, dedicate the loop to processing it.
    if (audio.isRunning()) {
        audio.loop();
        return; // Skip sensor checks until audio is done.
    }

    // If no audio is playing, check the sensors.
    for (int i = 0; i < NUM_SENSORS; i++) {
        int photodiodeValue = analogRead(PHOTODIODE_PINS[i]);
        bool laserBroken = photodiodeValue < thresholds[i];

        if (laserBroken != pendingStates[i]) {
            lastChangeTime[i] = millis();
            pendingStates[i] = laserBroken;
        }

        if ((millis() - lastChangeTime[i]) > DEBOUNCE_DELAY) {
            if (pendingStates[i] != previousStates[i]) {
                previousStates[i] = pendingStates[i];
                if (pendingStates[i]) { 
                    Serial.printf("Sensor %d BROKEN. Playing sound...\n", i + 1);
                    
                    // Directly play the sound from the main loop.
                    audio.connecttoFS(SD, noteFiles[i]);
                    
                    // After starting the sound, immediately enter the audio processing loop.
                    while (audio.isRunning()) {
                        audio.loop();
                    }
                    Serial.println("Playback finished.");
                }
            }
        }
    }
}

// =================================================================
// ---                  SENSOR CALIBRATION                     ---
// =================================================================
void calibrateSensors() {
    long laserOnReadings[NUM_SENSORS] = {0};
    long ambientReadings[NUM_SENSORS] = {0};
    
    Serial.println("\n--- Starting Sensor Calibration ---");

    digitalWrite(LASER_POWER_PIN, LOW); 
    delay(200);
    Serial.println("Step 1: Reading ambient light...");
    for (int i = 0; i < NUM_SENSORS; i++) {
        long sum = 0;
        for (int j = 0; j < CALIBRATION_SAMPLES; j++) { 
            sum += analogRead(PHOTODIODE_PINS[i]); 
            delay(2); 
        }
        ambientReadings[i] = sum / CALIBRATION_SAMPLES;
    }
    
    digitalWrite(LASER_POWER_PIN, HIGH); 
    delay(200);
    Serial.println("Step 2: Reading with lasers ON...");
    for (int i = 0; i < NUM_SENSORS; i++) {
        long sum = 0;
        for (int j = 0; j < CALIBRATION_SAMPLES; j++) { 
            sum += analogRead(PHOTODIODE_PINS[i]); 
            delay(2); 
        }
        laserOnReadings[i] = sum / CALIBRATION_SAMPLES;
    }

    digitalWrite(LASER_POWER_PIN, LOW); 

    Serial.println("\n--- Calibration Results ---");
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (laserOnReadings[i] < ambientReadings[i]) {
            Serial.printf("!!! WARNING: Sensor %d logic is INVERTED. Swapping ON/AMBIENT values.\n", i + 1);
            long temp = laserOnReadings[i];
            laserOnReadings[i] = ambientReadings[i];
            ambientReadings[i] = temp;
        }

        long range = laserOnReadings[i] - ambientReadings[i];
        if (range < 100) {
            Serial.printf("WARNING: Sensor %d has low signal range (%ld). Check alignment.\n", i+1, range);
        }
        
        thresholds[i] = ambientReadings[i] + (range * 0.75);
        
        Serial.printf("Sensor %d: ON=%ld, AMBIENT=%ld -> Threshold=%d\n",
            i + 1, laserOnReadings[i], ambientReadings[i], thresholds[i]);
    }
    Serial.println("--- Calibration Complete ---");
}
