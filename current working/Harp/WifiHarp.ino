// --- INCLUDES ---
#include "driver/i2s.h"
#include "driver/adc.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>

// --- HARDWARE & PIN DEFINITIONS ---
#define SD_CS_PIN 5
#define SPI_MOSI_PIN 23
#define SPI_MISO_PIN 19
#define SPI_SCK_PIN 18

// Dial control pins
#define DIAL_LEFT_PIN 4 // Safe input pin
#define DIAL_RIGHT_PIN 15 // Strapping pin but works as input with pull-up
#define LASER_CONTROL_PIN 13
#define PUMP_PIN 2 // Strapping pin - HIGH=pump ON (good for boot)

const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 36};
const adc1_channel_t ADC_CHANNELS[5] = {
    ADC1_CHANNEL_6, // GPIO 34
    ADC1_CHANNEL_7, // GPIO 35
    ADC1_CHANNEL_4, // GPIO 32
    ADC1_CHANNEL_5, // GPIO 33
    ADC1_CHANNEL_0 // GPIO 36
};

// --- WIFI CONFIGURATION ---
const char* ap_ssid = "AquaStrings";

// --- GLOBAL CONSTANTS ---
const int NUM_SENSORS = 5;
const int WAV_HEADER_SIZE = 44;
const size_t MIXER_BUFFER_SAMPLES = 128;
const int MAX_SCALES = 20;

// --- THRESHOLD & DEBOUNCING ---
int stringThresholds[5] = {3200, 3200, 3200, 3200, 3200}; // Individual thresholds
const int DEBOUNCE_DELAY = 20;
bool previousStates[NUM_SENSORS];
bool pendingStates[NUM_SENSORS];
unsigned long lastChangeTime[NUM_SENSORS];

// --- SYSTEM STATE CONTROL ---
enum SystemState {
    SYSTEM_OFF, // State 1: All off (LEFT: HIGH, RIGHT: LOW)
    FOUNTAIN_MODE, // State 2: Fountain mode (LEFT: LOW, RIGHT: LOW) - WiFi control available
    HARP_MODE // State 3: Full harp mode (LEFT: LOW, RIGHT: HIGH)
};

SystemState currentSystemState = SYSTEM_OFF;
SystemState previousSystemState = SYSTEM_OFF;

// --- WEB CONTROL VARIABLES ---
bool webPumpState = false;
bool webLaserState = false;

// --- SCALE MANAGEMENT ---
struct ScaleInfo {
    String name;
    String folderPath;
};

ScaleInfo availableScales[MAX_SCALES];
int numAvailableScales = 0;
String currentScaleName = "CMajor";
bool scaleLoadInProgress = false;
String scaleLoadStatus = "";
int scaleLoadProgress = 0;

// --- AUDIO FILE CONFIGURATION ---
String noteFiles[NUM_SENSORS];
const char* noteNames[5] = {"1", "2", "3", "4", "5"};

// --- AUDIO DATA STRUCTURES ---
struct WavHeader {
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t num_channels;
};

struct AudioNote {
    uint8_t* buffer = NULL;
    size_t size = 0;
    WavHeader header;
    volatile bool isPlaying = false;
    volatile size_t current_position = 0;
};

volatile AudioNote notes[NUM_SENSORS];

// --- RTOS & TASK MANAGEMENT ---
TaskHandle_t audioMixerTaskHandle = NULL;
TaskHandle_t webServerTaskHandle = NULL;
bool audioSystemInitialized = false;
bool sdCardInitialized = false;
bool wifiInitialized = false;

// --- FUNCTION PROTOTYPES ---
bool loadNoteToPsram(const char* path, AudioNote& note);
void audioMixerTask(void *pvParameters);
void webServerTask(void *pvParameters);
bool initializeI2S(const WavHeader& header);
SystemState readDialState();
void handleSystemStateChange();
bool initializeAudioSystem();
void shutdownAudioSystem();
bool initializeSDCard();
void initializeWiFi();
void shutdownWiFi();
void scanForScales();
bool loadScale(const String& scaleName);
void unloadCurrentScale();
String findNoteFileForString(const String& scaleName, int stringNumber);
void loadScaleTask(void *pvParameters);
String urlDecode(String str);

// =================================================================
// --- SETUP ---
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Integrated AquaStrings System Initializing ---");

    // --- DIAL CONTROL INITIALIZATION ---
    pinMode(DIAL_LEFT_PIN, INPUT_PULLUP);
    pinMode(DIAL_RIGHT_PIN, INPUT_PULLUP);
    pinMode(LASER_CONTROL_PIN, OUTPUT);
    pinMode(PUMP_PIN, OUTPUT);

    // Start with everything off
    digitalWrite(LASER_CONTROL_PIN, LOW);
    digitalWrite(PUMP_PIN, LOW);
    Serial.println("Hardware pins initialized - everything OFF");

    // --- ADC INITIALIZATION ---
    Serial.println("Configuring ADC channels...");
    adc1_config_width(ADC_WIDTH_BIT_12);
    for (int i = 0; i < NUM_SENSORS; i++) {
        adc1_config_channel_atten(ADC_CHANNELS[i], ADC_ATTEN_DB_11);
    }
    Serial.println("ADC OK.");

    // --- PSRAM INITIALIZATION ---
    if (!psramInit()) {
        Serial.println("FATAL: PSRAM failed!");
        while (true);
    }
    Serial.println("PSRAM OK.");

    // Initialize sensor states
    for (int i = 0; i < NUM_SENSORS; i++) {
        previousStates[i] = false;
        pendingStates[i] = false;
        lastChangeTime[i] = 0;
    }

    // --- SD CARD AND AUDIO LOADING ---
    Serial.println("Loading CMajor scale from SD Card into PSRAM...");
    if (!initializeSDCard()) {
        Serial.println("WARNING: SD Card initialization failed!");
        Serial.println("System will continue - audio disabled");
    } else {
        // Scan for available scales first
        scanForScales();

        // Load default CMajor scale
        Serial.println("Loading default CMajor scale into PSRAM...");
        if (!loadScale("CMajor")) {
            Serial.println("WARNING: Failed to load default CMajor scale");
            // Try loading the first available scale
            if (numAvailableScales > 0) {
                Serial.printf("Attempting to load first available scale: %s\n", availableScales[0].name.c_str());
                loadScale(availableScales[0].name);
            }
        }
    }

    // --- INITIALIZE WIFI ACCESS POINT ---
    Serial.println("Starting WiFi Access Point...");
    initializeWiFi();

    // --- READ INITIAL DIAL STATE AND CONFIGURE SYSTEM ---
    currentSystemState = readDialState();
    previousSystemState = currentSystemState;
    handleSystemStateChange(); // Configure initial state

    Serial.println("\n--- System Ready! ---");
    Serial.printf("Current mode: ");
    switch(currentSystemState) {
        case SYSTEM_OFF: Serial.println("OFF - All controls disabled"); break;
        case FOUNTAIN_MODE: Serial.println("FOUNTAIN - Web controls available"); break;
        case HARP_MODE: Serial.println("HARP - Full system active, web controls disabled"); break;
    }

    Serial.println("Mode descriptions:");
    Serial.println("- OFF: Everything disabled, web interface shows status only");
    Serial.println("- FOUNTAIN: Pump ON, DAC OFF, Lasers controllable via web");
    Serial.println("- HARP: Pump ON, DAC ON, Lasers ON, web controls disabled");
    Serial.printf("Current scale: %s (%d scales available)\n", currentScaleName.c_str(), numAvailableScales);
}

// =================================================================
// --- MAIN LOOP ---
// =================================================================
void loop() {
    // Read current dial state
    currentSystemState = readDialState();

    // Handle state changes
    if (currentSystemState != previousSystemState) {
        handleSystemStateChange();
        previousSystemState = currentSystemState;
    }

    // Only process laser sensors if in HARP_MODE
    if (currentSystemState == HARP_MODE && audioSystemInitialized) {
        for (int i = 0; i < NUM_SENSORS; i++) {
            int photodiodeValue = adc1_get_raw(ADC_CHANNELS[i]);
            bool laserBroken = photodiodeValue > stringThresholds[i]; // REVERSED LOGIC

            if (laserBroken != pendingStates[i]) {
                lastChangeTime[i] = millis();
                pendingStates[i] = laserBroken;
            }

            if ((millis() - lastChangeTime[i]) > DEBOUNCE_DELAY) {
                if (pendingStates[i] != previousStates[i]) {
                    previousStates[i] = pendingStates[i];
                    if (pendingStates[i]) {
                        // Re-trigger the note by resetting its state
                        notes[i].current_position = WAV_HEADER_SIZE;
                        notes[i].isPlaying = true;
                    }
                }
            }
        }
    }

    delay(10); // Small delay to prevent excessive polling
}

// =================================================================
// --- SCALE MANAGEMENT FUNCTIONS ---
// =================================================================
void scanForScales() {
    Serial.println("Scanning SD card for scale folders...");
    numAvailableScales = 0;

    File root = SD.open("/");
    if (!root) {
        Serial.println("ERROR: Failed to open root directory");
        return;
    }

    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;

        if (entry.isDirectory() && numAvailableScales < MAX_SCALES) {
            String folderName = String(entry.name());
            // Skip system folders
            if (!folderName.startsWith(".") && !folderName.startsWith("System")) {
                availableScales[numAvailableScales].name = folderName;
                availableScales[numAvailableScales].folderPath = "/" + folderName;

                Serial.printf("Found scale folder: %s\n", folderName.c_str());
                numAvailableScales++;
            }
        }
        entry.close();
    }
    root.close();

    Serial.printf("Found %d scale folders\n", numAvailableScales);
}

String findNoteFileForString(const String& scaleName, int stringNumber) {
    String folderPath = "/" + scaleName;
    String stringPrefix = String(stringNumber);

    File dir = SD.open(folderPath);
    if (!dir) {
        return "";
    }

    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;

        String fileName = String(entry.name());
        entry.close();

        // Check if file starts with our string number and ends with .wav
        if (fileName.startsWith(stringPrefix) && fileName.endsWith(".wav")) {
            dir.close();
            return folderPath + "/" + fileName;
        }
    }
    dir.close();
    return ""; // Not found
}

bool loadScale(const String& scaleName) {
    if (scaleLoadInProgress) {
        Serial.println("Scale load already in progress");
        return false;
    }

    scaleLoadInProgress = true;
    scaleLoadProgress = 0;
    scaleLoadStatus = "Preparing to load " + scaleName + "...";
    Serial.printf("Loading scale: %s\n", scaleName.c_str());

    // First, find and verify all required files exist
    scaleLoadStatus = "Scanning scale files...";
    String foundFiles[NUM_SENSORS];

    for (int i = 0; i < NUM_SENSORS; i++) {
        String filePath = findNoteFileForString(scaleName, i + 1); // Strings are numbered 1-5
        if (filePath.length() == 0) {
            Serial.printf("ERROR: No file found for string %d in scale %s\n", i + 1, scaleName.c_str());
            scaleLoadStatus = "ERROR: Missing file for string " + String(i + 1) + " in " + scaleName;
            scaleLoadInProgress = false;
            return false;
        }
        foundFiles[i] = filePath;
        Serial.printf("Found for string %d: %s\n", i + 1, filePath.c_str());
    }

    // Unload current scale from PSRAM
    scaleLoadStatus = "Unloading current scale...";
    unloadCurrentScale();
    delay(200);

    // Load new scale files into PSRAM
    bool allLoaded = true;

    for (int i = 0; i < NUM_SENSORS; i++) {
        scaleLoadProgress = (i + 1) * (100 / NUM_SENSORS);
        scaleLoadStatus = "Loading string " + String(i + 1) + "/" + String(NUM_SENSORS) + "...";
        noteFiles[i] = foundFiles[i];

        if (!loadNoteToPsram(foundFiles[i].c_str(), (AudioNote&)notes[i])) {
            Serial.printf("ERROR: Failed to load %s\n", foundFiles[i].c_str());
            allLoaded = false;
            break;
        }
        delay(100);
    }

    if (allLoaded) {
        currentScaleName = scaleName;
        scaleLoadStatus = "New scale loaded!";
        Serial.printf("Scale %s loaded successfully!\n", scaleName.c_str());
    } else {
        scaleLoadStatus = "ERROR: Failed to load " + scaleName;
        Serial.printf("Failed to load scale: %s\n", scaleName.c_str());
    }

    scaleLoadInProgress = false;
    scaleLoadProgress = 0;
    return allLoaded;
}

void unloadCurrentScale() {
    Serial.println("Unloading current scale from PSRAM...");

    // Stop all playing notes
    for (int i = 0; i < NUM_SENSORS; i++) {
        notes[i].isPlaying = false;
        notes[i].current_position = 0;

        // Free PSRAM memory
        if (notes[i].buffer != NULL) {
            free(notes[i].buffer);
            notes[i].buffer = NULL;
        }
        notes[i].size = 0;
    }

    Serial.println("Current scale unloaded from PSRAM");
}

// =================================================================
// --- DIAL CONTROL FUNCTIONS ---
// =================================================================
SystemState readDialState() {
    int stateLeft = digitalRead(DIAL_LEFT_PIN);
    int stateRight = digitalRead(DIAL_RIGHT_PIN);

    if (stateLeft == HIGH && stateRight == LOW) {
        return SYSTEM_OFF;
    } else if (stateLeft == LOW && stateRight == LOW) {
        return FOUNTAIN_MODE;
    } else if (stateLeft == LOW && stateRight == HIGH) {
        return HARP_MODE;
    }

    // Default to off for any undefined state
    return SYSTEM_OFF;
}

void handleSystemStateChange() {
    Serial.printf("State change: %d -> %d\n", previousSystemState, currentSystemState);

    switch (currentSystemState) {
        case SYSTEM_OFF:
            Serial.println("Switching to: SYSTEM OFF");
            // Turn everything OFF
            digitalWrite(PUMP_PIN, LOW);
            digitalWrite(LASER_CONTROL_PIN, LOW);

            // Shutdown audio system (DAC OFF)
            if (audioSystemInitialized) {
                shutdownAudioSystem();
            }

            // Reset web control states
            webPumpState = false;
            webLaserState = false;

            Serial.println("- Pump: OFF");
            Serial.println("- Lasers: OFF");
            Serial.println("- DAC: OFF");
            Serial.println("- Web controls: DISABLED (display only)");
            break;

        case FOUNTAIN_MODE:
            Serial.println("Switching to: FOUNTAIN MODE");
            // Turn pump ON, keep lasers controllable via web
            digitalWrite(PUMP_PIN, HIGH);
            webPumpState = true; // Update web state to reflect reality

            // Lasers controlled by web interface
            digitalWrite(LASER_CONTROL_PIN, webLaserState ? HIGH : LOW);

            // Keep audio system OFF (DAC OFF)
            if (audioSystemInitialized) {
                shutdownAudioSystem();
            }

            Serial.println("- Pump: ON (controllable via web)");
            Serial.println("- Lasers: Controllable via web");
            Serial.println("- DAC: OFF");
            Serial.println("- Web controls: ENABLED");
            break;

        case HARP_MODE:
            Serial.println("Switching to: HARP MODE");
            // Force everything ON
            digitalWrite(PUMP_PIN, HIGH);
            digitalWrite(LASER_CONTROL_PIN, HIGH);

            // Update web states to reflect hardware reality
            webPumpState = true;
            webLaserState = true;

            // Initialize audio system (DAC ON)
            if (!audioSystemInitialized && sdCardInitialized) {
                delay(500); // Give system time to stabilize
                if (initializeAudioSystem()) {
                    Serial.println("Audio system (DAC) initialized successfully");
                } else {
                    Serial.println("Failed to initialize audio system");
                }
            } else if (!sdCardInitialized) {
                Serial.println("Audio disabled - no SD card available");
            }

            Serial.println("- Pump: ON (forced)");
            Serial.println("- Lasers: ON (forced)");
            Serial.println("- DAC: ON");
            Serial.println("- Web controls: DISABLED (display only)");
            break;
    }
}

// =================================================================
// --- WIFI FUNCTIONS ---
// =================================================================
void initializeWiFi() {
    Serial.println("Initializing WiFi Access Point...");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid);

    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    Serial.printf("Connect to: %s (No password)\n", ap_ssid);
    Serial.printf("Then open: http://%s\n", IP.toString().c_str());

    // Create Web Server Task on Core 1
    xTaskCreatePinnedToCore(
        webServerTask, "WebServer", 16384, NULL, 1, &webServerTaskHandle, 1
    );

    wifiInitialized = true;
    Serial.println("WiFi Access Point ready!");
}

void shutdownWiFi() {
    if (wifiInitialized) {
        Serial.println("Shutting down WiFi...");

        // Stop web server task
        if (webServerTaskHandle != NULL) {
            vTaskDelete(webServerTaskHandle);
            webServerTaskHandle = NULL;
        }

        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiInitialized = false;
        Serial.println("WiFi shutdown complete.");
    }
}

// =================================================================
// --- BACKGROUND AND WEB SERVER TASKS ---
// =================================================================

String urlDecode(String str) {
  String decodedString = "";
  char temp[] = "0x00";
  int len = str.length();
  for (int i = 0; i < len; i++) {
    if (str[i] == '%') {
      if (i + 2 < len) {
        temp[2] = str[i+1];
        temp[3] = str[i+2];
        decodedString += (char) strtol(temp, NULL, 16);
        i += 2;
      }
    } else if (str[i] == '+') {
      decodedString += ' ';
    } else {
      decodedString += str[i];
    }
  }
  return decodedString;
}

/**
 * @brief Task to load a new scale in the background.
 * This prevents the web server from blocking during file I/O.
 * @param pvParameters A pointer to a String object containing the scale name.
 */
void loadScaleTask(void *pvParameters) {
    // The parameter is a pointer to the String object with the scale name
    String* scaleName = (String*)pvParameters;

    // Call the existing loadScale function
    loadScale(*scaleName);

    // Clean up the dynamically allocated String
    delete scaleName;

    // Delete the task
    vTaskDelete(NULL);
}

void webServerTask(void *pvParameters) {
    WiFiServer server(80);
    String header;

    server.begin();

    for(;;) {
        // Check if we should still be running
        if (!wifiInitialized) {
            Serial.println("WiFi task stopping...");
            vTaskDelete(NULL);
        }

        WiFiClient client = server.accept();

        if (client) {
            String currentLine = "";
            // Read current system mode
            SystemState currentMode = readDialState();
            bool controlsEnabled = (currentMode == FOUNTAIN_MODE);
            
            while (client.connected()) {
                if (client.available()) {
                    char c = client.read();
                    header += c;
                    if (c == '\n') {
                        if (currentLine.length() == 0) {
                            
                            // Handle the favicon request gracefully
                            if (header.indexOf("GET /favicon.ico") >= 0) {
                                client.println("HTTP/1.1 204 No Content");
                                client.println();
                                break;
                            }

                            // Handle API endpoint for live data
                            if (header.indexOf("GET /api/status") >= 0) {
                                client.println("HTTP/1.1 200 OK");
                                client.println("Content-type:application/json");
                                client.println("Connection: close");
                                client.println();
                                
                                // Send JSON response with current status
                                SystemState apiMode = readDialState();
                                client.print("{");
                                client.print("\"pump\":" + String(webPumpState ? "true" : "false") + ",");
                                client.print("\"laser\":" + String(webLaserState ? "true" : "false") + ",");
                                client.print("\"controlsEnabled\":" + String(controlsEnabled ? "true" : "false") + ",");
                                client.print("\"mode\":\"");
                                if (apiMode == SYSTEM_OFF) client.print("SYSTEM OFF");
                                else if (apiMode == FOUNTAIN_MODE) client.print("FOUNTAIN MODE");
                                else if (apiMode == HARP_MODE) client.print("HARP MODE");
                                client.print("\",");
                                client.print("\"currentScale\":\"" + currentScaleName + "\",");
                                client.print("\"scaleLoadInProgress\":" + String(scaleLoadInProgress ? "true" : "false") + ",");
                                client.print("\"scaleLoadStatus\":\"" + scaleLoadStatus + "\",");
                                client.print("\"scaleLoadProgress\":" + String(scaleLoadProgress) + ",");
                                client.print("\"availableScales\":[");
                                for (int i = 0; i < numAvailableScales; i++) {
                                    client.print("\"" + availableScales[i].name + "\"");
                                    if (i < numAvailableScales - 1) client.print(",");
                                }
                                client.print("],");
                                client.print("\"sensors\":[");
                                for (int i = 0; i < 5; i++) {
                                    client.print(adc1_get_raw(ADC_CHANNELS[i]));
                                    if (i < 4) client.print(",");
                                }
                                client.print("],");
                                client.print("\"thresholds\":[");
                                for (int i = 0; i < 5; i++) {
                                    client.print(stringThresholds[i]);
                                    if (i < 4) client.print(",");
                                }
                                client.print("]}");
                                client.println();
                                break;
                            }
                            
                            // --- NEW SENSOR-ONLY API ENDPOINT ---
                            else if (header.indexOf("GET /api/sensors") >= 0) {
                                client.println("HTTP/1.1 200 OK");
                                client.println("Content-type:application/json");
                                client.println("Connection: close");
                                client.println();

                                client.print("{");
                                client.print("\"sensors\":[");
                                for (int i = 0; i < 5; i++) {
                                    client.print(adc1_get_raw(ADC_CHANNELS[i]));
                                    if (i < 4) client.print(",");
                                }
                                client.print("],");
                                client.print("\"thresholds\":[");
                                for (int i = 0; i < 5; i++) {
                                    client.print(stringThresholds[i]);
                                    if (i < 4) client.print(",");
                                }
                                client.print("]}");
                                client.println();
                                break;
                            }


                            // Flag to check if we handled an action and should stop
                            bool actionHandled = false;

                            // Handle scale loading
                            if (controlsEnabled && !scaleLoadInProgress && header.indexOf("GET /loadscale?name=") >= 0) {
                                int nameStart = header.indexOf("name=") + 5;
                                int nameEnd = header.indexOf(" ", nameStart);
                                if (nameEnd == -1) nameEnd = header.indexOf("&", nameStart);
                                if (nameEnd == -1) nameEnd = header.length();
                                
                                String scaleNameEncoded = header.substring(nameStart, nameEnd);
                                String scaleName = urlDecode(scaleNameEncoded);
                                
                                Serial.printf("Web request to load scale: %s\n", scaleName.c_str());
                                
                                bool scaleExists = false;
                                for (int i = 0; i < numAvailableScales; i++) {
                                    if (availableScales[i].name == scaleName) {
                                        scaleExists = true;
                                        break;
                                    }
                                }
                                
                                if (scaleExists) {
                                    // Create a new String object on the heap to pass to the task
                                    String* scaleNameToLoad = new String(scaleName);

                                    // Create the task to load the scale in the background
                                    xTaskCreate(
                                        loadScaleTask, // Task function
                                        "ScaleLoader", // Name of the task
                                        8192, // Stack size in words
                                        (void*)scaleNameToLoad, // Task input parameter
                                        2, // Priority of the task
                                        NULL // Task handle
                                    );
                                } else {
                                    scaleLoadStatus = "ERROR: Scale not found: " + scaleName;
                                }
                                actionHandled = true; // Mark as handled
                            }

                            // Handle control commands (ONLY process in fountain mode)
                            if (controlsEnabled) {
                                if (header.indexOf("GET /pump_on") >= 0) {
                                    webPumpState = true;
                                    digitalWrite(PUMP_PIN, HIGH);
                                    Serial.println("Web: Pump ON");
                                    actionHandled = true;
                                } else if (header.indexOf("GET /pump_off") >= 0) {
                                    webPumpState = false;
                                    digitalWrite(PUMP_PIN, LOW);
                                    Serial.println("Web: Pump OFF");
                                    actionHandled = true;
                                } else if (header.indexOf("GET /laser_on") >= 0) {
                                    webLaserState = true;
                                    digitalWrite(LASER_CONTROL_PIN, HIGH);
                                    Serial.println("Web: Laser ON");
                                    actionHandled = true;
                                } else if (header.indexOf("GET /laser_off") >= 0) {
                                    webLaserState = false;
                                    digitalWrite(LASER_CONTROL_PIN, LOW);
                                    Serial.println("Web: Laser OFF");
                                    actionHandled = true;
                                }

                                // Handle threshold updates
                                for (int i = 0; i < 5; i++) {
                                    String thresholdCmd = "/setthresh" + String(i) + "?value=";
                                    if (header.indexOf("GET " + thresholdCmd) >= 0) {
                                        int valueStart = header.indexOf("value=") + 6;
                                        int valueEnd = header.indexOf(" ", valueStart);
                                        if (valueEnd == -1) valueEnd = header.indexOf("&", valueStart);
                                        if (valueEnd == -1) valueEnd = header.length();
                                        
                                        int newThreshold = header.substring(valueStart, valueEnd).toInt();
                                        if (newThreshold >= 0 && newThreshold <= 4095) {
                                            stringThresholds[i] = newThreshold;
                                            Serial.printf("Web: String %s threshold set to %d\n", noteNames[i], newThreshold);
                                        }
                                        actionHandled = true;
                                    }
                                }
                            }

                            // If we handled an action, send a "No Content" response and stop
                            if (actionHandled) {
                                client.println("HTTP/1.1 204 No Content");
                                client.println("Connection: close");
                                client.println();
                                break; // IMPORTANT: Exit after handling the action
                            }


                            // If no action was handled, send the main HTML page
                            client.println("HTTP/1.1 200 OK");
                            client.println("Content-type:text/html");
                            client.println("Connection: close");
                            client.println();

                            // HTML page
                            client.println("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no\">");
                            client.println("<style>body{font-family:Arial;text-align:center;margin:0;background-color:#f5f5f5;}");
                            client.println(".container{max-width:800px;margin:0 auto;background:white;padding:10px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}");
                            client.println(".button{background-color:#008CBA;border:none;color:white;padding:12px 24px;margin:5px;cursor:pointer;border-radius:4px;font-size:16px;}");
                            client.println(".button2{background-color:#555555;}.button3{background-color:#4CAF50;}");
                            client.println(".disabled{background-color:#ccc !important;color:#999 !important;cursor:not-allowed !important;opacity:0.6;}");
                            client.println(".slider{width:200px;} .slider:disabled{opacity:0.5;}");
                            client.println("select{padding:5px;margin:5px;font-size:16px;} select:disabled{opacity:0.5;}");
                            client.println("input[type='number']{width:60px;padding:5px;margin:2px;} input:disabled{opacity:0.5;}");
                            client.println(".status{padding:10px;border-radius:4px;margin:10px 0;}");
                            client.println(".status-off{background-color:#ffebee;color:#c62828;}");
                            client.println(".status-fountain{background-color:#e8f5e8;color:#2e7d32;}");
                            client.println(".status-harp{background-color:#e3f2fd;color:#1565c0;}");
                            client.println(".progress-bar-container{width:100%;background-color:#ddd;border-radius:4px;margin:10px 0;}");
                            client.println(".progress-bar{width:0%;height:20px;background-color:#4CAF50;text-align:center;line-height:20px;color:white;border-radius:4px;transition:width 0.2s;}");
                            client.println("table{width:100%;border-collapse:collapse;margin:10px auto;}th,td{padding:4px;border:1px solid #ddd;font-size:14px;}");
                            client.println("</style></head>");
                            client.println("<body><div class='container'>");
                            client.println("<h1>AquaStrings Controls</h1>");
                            
                            // Mode display
                            client.println("<h2><span id='currentMode'></span></h2>");

                            // Scale Selection Section
                            client.println("<h3>Scale Selection</h3>");
                            client.println("<p><strong>Current Scale:</strong> <span id='currentScale'></span></p>");
                            client.println("<div><select id='scaleDropdown'></select>");
                            client.println("<button class='button' onclick='loadSelectedScale()' id='loadButton'>Load</button></div>");
                            client.println("<div id='loadingStatus'></div>");
                            client.println("<div id='progressBarContainer' class='progress-bar-container' style='display:none;'><div id='progressBar' class='progress-bar'></div></div>");

                            // System Controls
                            client.println("<h3>System Controls</h3>");
                            client.println("<div id='pumpContainer'><button id='pumpButton'></button></div>");
                            client.println("<div id='laserContainer'><button id='laserButton'></button></div>");

                            // String Thresholds
                            client.println("<h3>String Sensor Readings & Thresholds</h3>");
                            client.println("<div id='thresholdsDescription'></div>");
                            client.println("<table>");
                            client.println("<tr><th>String</th><th>Live Reading</th><th>Threshold</th><th>Status</th></tr>");
                            for (int i = 0; i < 5; i++) {
                                client.println("<tr><td>" + String(noteNames[i]) + "</td>");
                                client.println("<td><span id='sensor" + String(i) + "'>0</span></td>");
                                client.println("<td id='threshInputContainer" + String(i) + "'></td>");
                                client.println("<td><span id='status" + String(i) + "'>-</span></td></tr>");
                            }
                            client.println("</table>");

                            // JavaScript
                            client.println("<script>");
                            client.println("let isClientLoading = false;");
                            client.println("let scaleBeingLoaded = '';");
                            client.println("let userSelection = null;");

                            // --- Control Functions (send data to ESP) ---
                            client.println("function setThreshold(index, value) { fetch('/setthresh' + index + '?value=' + value); }");
                            
                            client.println("function togglePump(state) { fetch(state ? '/pump_on' : '/pump_off'); }");
                            client.println("function toggleLaser(state) { fetch(state ? '/laser_on' : '/laser_off'); }");

                            client.println("function loadSelectedScale() {");
                            client.println("  const dropdown = document.getElementById('scaleDropdown');");
                            client.println("  const selectedScale = dropdown.value;");
                            client.println("  isClientLoading = true;");
                            client.println("  scaleBeingLoaded = selectedScale;");
                            client.println("  userSelection = null;"); // Clear user selection once loading starts
                            client.println("  fetch('/loadscale?name=' + encodeURIComponent(selectedScale));");
                            client.println("}");

                            // --- UI Update Functions (get data from ESP) ---

                            // Updates only the live sensor readings, called every second
                            client.println("function updateLiveReadings() {");
                            client.println("  fetch('/api/sensors').then(r => r.json()).then(data => {");
                            client.println("    for(let i = 0; i < 5; i++) {");
                            client.println("      document.getElementById('sensor' + i).textContent = data.sensors[i];");
                            client.println("      const isTriggered = data.sensors[i] > data.thresholds[i];"); // REVERSED LOGIC
                            client.println("      const statusEl = document.getElementById('status' + i);");
                            client.println("      statusEl.textContent = isTriggered ? 'TRIGGERED' : 'CLEAR';");
                            client.println("      statusEl.style.color = isTriggered ? '#c62828' : '#2e7d32';");
                            client.println("    }");
                            client.println("  }).catch(e => console.log('Sensor update failed:', e));");
                            client.println("}");

                            // Updates the main page content, called on load and after major actions
                            client.println("function updatePageUI() {");
                            client.println("  fetch('/api/status').then(r => r.json()).then(data => {");
                            
                            // If the client was waiting for a load and the server's current scale now matches the target, the load is finished.
                            client.println("    if (isClientLoading && !data.scaleLoadInProgress && data.currentScale === scaleBeingLoaded) {");
                            client.println("        isClientLoading = false;");
                            client.println("        scaleBeingLoaded = '';");
                            client.println("    }");

                            client.println("    document.getElementById('currentMode').textContent = data.mode;");
                            client.println("    document.getElementById('currentScale').textContent = data.currentScale;");
                            client.println("    const statusDiv = document.getElementById('loadingStatus');");
                            client.println("    const progressBarContainer = document.getElementById('progressBarContainer');");
                            client.println("    const progressBar = document.getElementById('progressBar');");
                            client.println("    if(data.scaleLoadInProgress) {");
                            client.println("      statusDiv.innerHTML = '<p>' + data.scaleLoadStatus + '</p>';");
                            client.println("      progressBarContainer.style.display = 'block';");
                            client.println("      progressBar.style.width = data.scaleLoadProgress + '%';");
                            client.println("      progressBar.textContent = data.scaleLoadProgress + '%';");
                            client.println("    } else {");
                            client.println("      statusDiv.innerHTML = '<p style=\"font-weight:bold;\">' + data.scaleLoadStatus + '</p>';");
                            client.println("      progressBarContainer.style.display = 'none';");
                            client.println("    }");
                            
                            // Handle the scale dropdown logic
                            client.println("    const dropdown = document.getElementById('scaleDropdown');");
                            client.println("    const loadBtn = document.getElementById('loadButton');");
                            
                            // Intelligently set the selected value
                            client.println("    if (userSelection) {");
                            client.println("        dropdown.value = userSelection;");
                            client.println("    } else if (isClientLoading) {");
                            client.println("        dropdown.value = scaleBeingLoaded;");
                            client.println("    } else {");
                            client.println("        dropdown.value = data.currentScale;");
                            client.println("    }");
                            
                            // Disable/enable controls
                            client.println("    dropdown.disabled = !data.controlsEnabled || data.scaleLoadInProgress;");
                            client.println("    loadBtn.disabled = !data.controlsEnabled || data.scaleLoadInProgress;");
                            client.println("    if (data.scaleLoadInProgress) loadBtn.classList.add('disabled'); else loadBtn.classList.remove('disabled');");

                            client.println("    const pumpButton = document.getElementById('pumpButton');");
                            client.println("    const laserButton = document.getElementById('laserButton');");
                            client.println("    if (data.mode === 'SYSTEM OFF') {");
                            client.println("        pumpButton.textContent = 'PUMP ON';");
                            client.println("        pumpButton.className = 'button button3 disabled';");
                            client.println("        pumpButton.onclick = () => false;");
                            client.println("        laserButton.textContent = 'LASER ON';");
                            client.println("        laserButton.className = 'button button3 disabled';");
                            client.println("        laserButton.onclick = () => false;");
                            client.println("    } else {");
                            client.println("        // Update Pump Button");
                            client.println("        if (data.pump) {");
                            client.println("            pumpButton.textContent = 'PUMP ON';");
                            client.println("            pumpButton.className = `button button3 ${data.controlsEnabled ? '' : 'disabled'}`;");
                            client.println("            pumpButton.onclick = () => togglePump(false);");
                            client.println("        } else {");
                            client.println("            pumpButton.textContent = 'PUMP OFF';");
                            client.println("            pumpButton.className = `button button2 ${data.controlsEnabled ? '' : 'disabled'}`;");
                            client.println("            pumpButton.onclick = () => togglePump(true);");
                            client.println("        }");
                            client.println("        // Update Laser Button");
                            client.println("        if (data.laser) {");
                            client.println("            laserButton.textContent = 'LASER ON';");
                            client.println("            laserButton.className = `button button3 ${data.controlsEnabled ? '' : 'disabled'}`;");
                            client.println("            laserButton.onclick = () => toggleLaser(false);");
                            client.println("        } else {");
                            client.println("            laserButton.textContent = 'LASER OFF';");
                            client.println("            laserButton.className = `button button2 ${data.controlsEnabled ? '' : 'disabled'}`;");
                            client.println("            laserButton.onclick = () => toggleLaser(true);");
                            client.println("        }");
                            client.println("    }");

                            client.println("    for(let i = 0; i < 5; i++) {");
                            client.println("      document.getElementById('threshInputContainer' + i).innerHTML = `<input type='number' min='0' max='4095' value='${data.thresholds[i]}' id='customThresh${i}' onchange='setThreshold(${i}, this.value)' ${data.controlsEnabled ? '' : 'disabled'}>`;");
                            client.println("    }");
                            client.println("  }).catch(e => { console.log('Update failed:', e); isClientLoading = false; });");
                            client.println("}");

                            // --- Event Listeners ---
                            client.println("document.addEventListener('DOMContentLoaded', () => {");
                            client.println("    const dropdown = document.getElementById('scaleDropdown');");
                            client.println("    dropdown.addEventListener('change', () => { userSelection = dropdown.value; });");
                            // Populate dropdown once on load
                            client.println("    fetch('/api/status').then(r => r.json()).then(data => {");
                            client.println("        data.availableScales.forEach(scale => {");
                            client.println("            const option = document.createElement('option');");
                            client.println("            option.value = scale; option.textContent = scale;");
                            client.println("            dropdown.appendChild(option);");
                            client.println("        });");
                            client.println("        updatePageUI();"); // Set initial state after populating
                            client.println("    });");
                            client.println("    updateLiveReadings();");
                            client.println("});");
                            client.println("setInterval(updateLiveReadings, 1000);");
                            client.println("setInterval(updatePageUI, 500);");
                            
                            client.println("</script></div></body></html>");


                            client.println();
                            break;
                        } else {
                            currentLine = "";
                        }
                    }
                }
            }
            header = "";
            client.stop();
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =================================================================
// --- AUDIO FUNCTIONS ---
// =================================================================
bool initializeAudioSystem() {
    Serial.println("Initializing audio hardware...");

    // Initialize I2S (audio files are already loaded)
    Serial.println("Initializing I2S audio driver...");
    if (!initializeI2S(((AudioNote&)notes[0]).header)) {
        Serial.println("ERROR: Failed to initialize I2S driver.");
        return false;
    }
    Serial.println("I2S driver OK.");

    // Create audio mixer task
    if (audioMixerTaskHandle == NULL) {
        Serial.println("Creating background audio mixer task...");
        xTaskCreatePinnedToCore(
            audioMixerTask, "AudioMixer", 8192, NULL, 5, &audioMixerTaskHandle, 1);
    }

    audioSystemInitialized = true;
    Serial.println("Audio system ready!");
    return true;
}

bool initializeSDCard() {
    Serial.println("Initializing SD Card...");

    // Initialize SPI
    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, SD_CS_PIN);
    delay(200); // Give SPI time to stabilize

    // Try multiple times with different settings
    for (int attempt = 0; attempt < 3; attempt++) {
        Serial.printf("SD Card init attempt %d...\n", attempt + 1);

        // Try different frequencies
        uint32_t freq = (attempt == 0) ? 1000000 : (attempt == 1) ? 4000000 : 8000000;

        if (SD.begin(SD_CS_PIN, SPI, freq)) {
            Serial.printf("SD Card OK (freq: %d Hz)\n", freq);
            sdCardInitialized = true;
            return true;
        }

        delay(500);
    }

    Serial.println("ERROR: All SD card initialization attempts failed");
    return false;
}

void shutdownAudioSystem() {
    Serial.println("Shutting down audio system...");

    // Stop all notes first
    for (int i = 0; i < NUM_SENSORS; i++) {
        notes[i].isPlaying = false;
        notes[i].current_position = 0;
    }

    // Stop audio mixer task
    if (audioMixerTaskHandle != NULL) {
        vTaskDelete(audioMixerTaskHandle);
        audioMixerTaskHandle = NULL;
        delay(100); // Give task time to stop
    }

    // Stop I2S driver
    if (audioSystemInitialized) {
        i2s_stop(I2S_NUM_0);
        i2s_driver_uninstall(I2S_NUM_0);
    }

    audioSystemInitialized = false;
    Serial.println("Audio system shutdown complete.");
}

void audioMixerTask(void *pvParameters) {
    uint16_t i2s_output_buffer[MIXER_BUFFER_SAMPLES * 2];

    while (true) {
        // Only process audio if system is in harp mode
        if (currentSystemState != HARP_MODE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        for (int i = 0; i < MIXER_BUFFER_SAMPLES; i++) {
            int32_t mixed_sample = 0;
            for (int j = 0; j < NUM_SENSORS; j++) {
                if (notes[j].isPlaying) {
                    if (notes[j].current_position < notes[j].size) {
                        uint8_t sample8bit = notes[j].buffer[notes[j].current_position];
                        mixed_sample += (int16_t)(sample8bit - 128);
                        notes[j].current_position++;
                    } else {
                        notes[j].isPlaying = false;
                    }
                }
            }

            if (mixed_sample > 127) mixed_sample = 127;
            if (mixed_sample < -128) mixed_sample = -128;

            uint8_t final_8bit_sample = (uint8_t)(mixed_sample + 128);
            uint16_t i2s_sample = ((uint16_t)final_8bit_sample) << 8;

            i2s_output_buffer[i * 2] = i2s_sample;
            i2s_output_buffer[i * 2 + 1] = i2s_sample;
        }

        size_t bytes_written = 0;
        i2s_write(I2S_NUM_0, i2s_output_buffer, sizeof(i2s_output_buffer), &bytes_written, portMAX_DELAY);
    }
}

bool initializeI2S(const WavHeader& header) {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate = 96000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_MSB),
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_NUM_0, NULL) != ESP_OK) return false;
    if (i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN) != ESP_OK) return false;
    i2s_zero_dma_buffer(I2S_NUM_0);

    return true;
}

bool loadNoteToPsram(const char* path, AudioNote& note) {
    File file = SD.open(path, "r");
    if (!file) {
        Serial.printf(" -> ERROR: Failed to open %s\n", path);
        return false;
    }
    
    note.size = file.size();
    if (note.size <= WAV_HEADER_SIZE) {
        Serial.println(" -> ERROR: File is too small.");
        file.close();
        return false;
    }

    note.buffer = (uint8_t *)ps_malloc(note.size);
    if (note.buffer == NULL) {
        Serial.println(" -> FATAL: Failed to allocate PSRAM!");
        file.close();
        return false;
    }
    
    if(file.read(note.buffer, note.size) != note.size) {
        Serial.println(" -> ERROR: File read failed.");
        free(note.buffer);
        note.buffer = NULL;
        file.close();
        return false;
    }
    file.close();

    note.header.num_channels = *(uint16_t*)(note.buffer + 22);
    note.header.sample_rate = *(uint32_t*)(note.buffer + 24);
    note.header.bits_per_sample = *(uint16_t*)(note.buffer + 34);

    Serial.printf(" -> Loaded %s (%d Hz, %d-bit, %d chan)\n", 
        path, note.header.sample_rate, note.header.bits_per_sample, note.header.num_channels);

    if (note.header.num_channels != 1 || note.header.bits_per_sample != 8 || note.header.sample_rate != 96000) {
        Serial.println(" -> WARNING: WAV should be 8-bit, Mono, and 96kHz for optimal mixing.");
    }
    return true;
}
