// --- INCLUDES ---
#include "driver/i2s.h"
#include "driver/adc.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"

// --- HARDWARE & PIN DEFINITIONS ---
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 26
#define SD_CS_PIN     5
#define SPI_MOSI_PIN 23
#define SPI_MISO_PIN 19
#define SPI_SCK_PIN  18
#define CONSTANT_HIGH_PIN 17

// Dial control pins - using available pins (4, 15, 2)
#define DIAL_LEFT_PIN 4   // Safe input pin
#define DIAL_RIGHT_PIN 15 // Strapping pin but works as input with pull-up
#define LASER_CONTROL_PIN 13
#define CONSTANT_HIGH_PIN 2  // Strapping pin - HIGH=pump ON (good for boot)

const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 36};
const adc1_channel_t ADC_CHANNELS[5] = {
    ADC1_CHANNEL_6, // GPIO 34
    ADC1_CHANNEL_7, // GPIO 35
    ADC1_CHANNEL_4, // GPIO 32
    ADC1_CHANNEL_5, // GPIO 33
    ADC1_CHANNEL_0  // GPIO 36
};

// --- GLOBAL CONSTANTS ---
const int NUM_SENSORS = 5;
const int WAV_HEADER_SIZE = 44;
const size_t MIXER_BUFFER_SAMPLES = 128; 

// --- MANUAL THRESHOLD & DEBOUNCING ---
const int TRIGGER_THRESHOLD = 3200;
const int DEBOUNCE_DELAY = 20;
bool previousStates[NUM_SENSORS];
bool pendingStates[NUM_SENSORS];
unsigned long lastChangeTime[NUM_SENSORS];

// --- SYSTEM STATE CONTROL ---
enum SystemState {
    SYSTEM_OFF,        // State 1: All off (LEFT: HIGH, RIGHT: LOW)
    PUMP_ONLY,         // State 2: Only pump on (LEFT: LOW, RIGHT: LOW)
    FULL_SYSTEM        // State 3: Full system running (LEFT: LOW, RIGHT: HIGH)
};

SystemState currentSystemState = SYSTEM_OFF;
SystemState previousSystemState = SYSTEM_OFF;

// --- AUDIO FILE CONFIGURATION ---
const char* noteFiles[NUM_SENSORS] = {
    "/CMajor/1C.wav", "/CMajor/2D.wav", "/CMajor/3E.wav",
    "/CMajor/4G.wav", "/CMajor/5A.wav"
};

// Silent note file path
const char* silentNoteFile = "/CMajor/silent.wav";

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
volatile AudioNote silentNote; // Background silent note

// --- RTOS & TASK MANAGEMENT ---
TaskHandle_t audioMixerTaskHandle = NULL;
bool audioSystemInitialized = false;
bool sdCardInitialized = false;

// --- HARDWARE OBJECTS ---
Adafruit_TPA2016 audioamp = Adafruit_TPA2016();

// --- FUNCTION PROTOTYPES ---
bool loadNoteToPsram(const char* path, AudioNote& note);
void audioMixerTask(void *pvParameters);
bool initializeI2S(const WavHeader& header);
SystemState readDialState();
void handleSystemStateChange();
bool initializeAudioSystem();
void shutdownAudioSystem();
bool initializeSDCard();
bool anyNoteCurrentlyPlaying();

// =================================================================
// ---                           SETUP                           ---
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Laser Harp System with Dial Control Initializing ---");

    // --- DIAL CONTROL INITIALIZATION ---
    pinMode(DIAL_LEFT_PIN, INPUT_PULLUP);
    pinMode(DIAL_RIGHT_PIN, INPUT_PULLUP);
    pinMode(LASER_CONTROL_PIN, OUTPUT);
    digitalWrite(LASER_CONTROL_PIN, LOW); // Start with lasers off

    // --- BASIC HARDWARE INITIALIZATION ---
    pinMode(CONSTANT_HIGH_PIN, OUTPUT);
    digitalWrite(CONSTANT_HIGH_PIN, LOW); // Start with pump off

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

    // --- SD CARD AND AUDIO INITIALIZATION ---
    Serial.println("Initializing SD Card and loading audio files...");
    if (!initializeSDCard()) {
        Serial.println("FATAL: SD Card initialization failed!"); 
        while (true);
    }
    
    // Load silent note first
    Serial.println("Loading silent background note...");
    if (!loadNoteToPsram(silentNoteFile, (AudioNote&)silentNote)) {
        Serial.printf("FATAL: Failed to load %s. Halting.\n", silentNoteFile);
        while(true);
    }
    Serial.println("Silent note loaded successfully.");
    
    // Load all WAV files into PSRAM at startup
    Serial.println("Loading WAV files into PSRAM...");
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (!loadNoteToPsram(noteFiles[i], (AudioNote&)notes[i])) {
            Serial.printf("FATAL: Failed to load %s. Halting.\n", noteFiles[i]);
            while(true);
        }
    }
    Serial.println("All WAV files loaded successfully.");

    // Read initial dial state
    currentSystemState = readDialState();
    previousSystemState = currentSystemState;
    
    Serial.println("\n--- System Ready! Monitoring dial state... ---");
    Serial.printf("Initial state: %d\n", currentSystemState);
    Serial.println("Note: SD card will initialize when switching to FULL_SYSTEM mode");
}

// =================================================================
// ---                         MAIN LOOP                         ---
// =================================================================
void loop() {
    // Read current dial state
    currentSystemState = readDialState();
    
    // Handle state changes
    if (currentSystemState != previousSystemState) {
        handleSystemStateChange();
        previousSystemState = currentSystemState;
    }

    // Only process laser sensors if in FULL_SYSTEM mode
    if (currentSystemState == FULL_SYSTEM && audioSystemInitialized) {
        for (int i = 0; i < NUM_SENSORS; i++) {
            int photodiodeValue = adc1_get_raw(ADC_CHANNELS[i]);
            bool laserBroken = photodiodeValue < TRIGGER_THRESHOLD;

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
// ---                    DIAL CONTROL FUNCTIONS                 ---
// =================================================================

SystemState readDialState() {
    int stateLeft = digitalRead(DIAL_LEFT_PIN);
    int stateRight = digitalRead(DIAL_RIGHT_PIN);

    if (stateLeft == HIGH && stateRight == LOW) {
        return SYSTEM_OFF;
    } else if (stateLeft == LOW && stateRight == LOW) {
        return PUMP_ONLY;
    } else if (stateLeft == LOW && stateRight == HIGH) {
        return FULL_SYSTEM;
    }
    
    // Default to off for any undefined state
    return SYSTEM_OFF;
}

void handleSystemStateChange() {
    Serial.printf("State change: %d -> %d\n", previousSystemState, currentSystemState);
    
    switch (currentSystemState) {
        case SYSTEM_OFF:
            Serial.println("Switching to: SYSTEM OFF");
            digitalWrite(CONSTANT_HIGH_PIN, LOW);  // Turn off pump
            digitalWrite(LASER_CONTROL_PIN, LOW);  // Turn off lasers
            if (audioSystemInitialized) {
                shutdownAudioSystem();
            }
            break;
            
        case PUMP_ONLY:
            Serial.println("Switching to: PUMP ONLY");
            digitalWrite(CONSTANT_HIGH_PIN, HIGH); // Turn on pump
            digitalWrite(LASER_CONTROL_PIN, LOW);  // Turn off lasers
            if (audioSystemInitialized) {
                shutdownAudioSystem();
            }
            break;
            
        case FULL_SYSTEM:
            Serial.println("Switching to: FULL SYSTEM");
            digitalWrite(CONSTANT_HIGH_PIN, HIGH); // Turn on pump
            digitalWrite(LASER_CONTROL_PIN, HIGH); // Turn on lasers
            if (!audioSystemInitialized) {
                delay(500); // Give system time to stabilize
                if (initializeAudioSystem()) {
                    Serial.println("Audio system initialized successfully");
                } else {
                    Serial.println("Failed to initialize audio system");
                    // Fall back to pump only
                    currentSystemState = PUMP_ONLY;
                    digitalWrite(LASER_CONTROL_PIN, LOW); // Turn off lasers if fallback
                }
            }
            break;
    }
}

bool initializeAudioSystem() {
    Serial.println("Initializing audio hardware...");
    
    // Initialize I2C and amplifier
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!audioamp.begin()) {
        Serial.println("ERROR: TPA2016 amplifier not found.");
        return false;
    }
    audioamp.setGain(0);
    Serial.printf("TPA2016 amplifier found! Gain: %d dB.\n", audioamp.getGain());

    // Initialize I2S (audio files are already loaded)
    Serial.println("Initializing I2S audio driver...");
    if (!initializeI2S(((AudioNote&)notes[0]).header)) { 
        Serial.println("ERROR: Failed to initialize I2S driver.");
        return false;
    }
    Serial.println("I2S driver OK.");

    // Start silent note playing immediately
    silentNote.current_position = WAV_HEADER_SIZE;
    silentNote.isPlaying = true;
    Serial.println("Silent background note started.");

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
    
    // Stop all notes first (including silent note)
    silentNote.isPlaying = false;
    silentNote.current_position = 0;
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
    Serial.println("Note: Audio files remain loaded in PSRAM for quick restart");
}

// =================================================================
// ---                      AUDIO FUNCTIONS                      ---
// =================================================================

bool anyNoteCurrentlyPlaying() {
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (notes[i].isPlaying) {
            return true;
        }
    }
    return false;
}

void audioMixerTask(void *pvParameters) {
    uint16_t i2s_output_buffer[MIXER_BUFFER_SAMPLES * 2];

    while (true) {
        // Only process audio if system is in full mode
        if (currentSystemState != FULL_SYSTEM) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        for (int i = 0; i < MIXER_BUFFER_SAMPLES; i++) {
            int32_t mixed_sample = 0;
            
            // Check if any musical notes are playing
            bool anyMusicPlaying = anyNoteCurrentlyPlaying();
            
            // If no musical notes are playing, ensure silent note is playing
            if (!anyMusicPlaying && !silentNote.isPlaying) {
                silentNote.current_position = WAV_HEADER_SIZE;
                silentNote.isPlaying = true;
            }
            // If musical notes are playing, stop the silent note
            else if (anyMusicPlaying && silentNote.isPlaying) {
                silentNote.isPlaying = false;
            }
            
            // Mix silent note (background)
            if (silentNote.isPlaying) {
                if (silentNote.current_position < silentNote.size) {
                    uint8_t sample8bit = silentNote.buffer[silentNote.current_position];
                    mixed_sample += (int16_t)(sample8bit - 128);
                    silentNote.current_position++;
                } else {
                    // Loop the silent note
                    silentNote.current_position = WAV_HEADER_SIZE;
                }
            }
            
            // Mix musical notes
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

            // Clamp the mixed sample
            if (mixed_sample > 127) mixed_sample = 127;
            if (mixed_sample < -128) mixed_sample = -128;

            uint8_t final_8bit_sample = (uint8_t)(mixed_sample + 128);
            uint16_t i2s_sample = ((uint16_t)final_8bit_sample) << 8;

            i2s_output_buffer[i * 2]     = i2s_sample;
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
        Serial.printf("    -> ERROR: Failed to open %s\n", path);
        return false;
    }
    
    note.size = file.size();
    if (note.size <= WAV_HEADER_SIZE) {
        Serial.println("    -> ERROR: File is too small.");
        file.close(); return false;
    }

    note.buffer = (uint8_t *)ps_malloc(note.size);
    if (note.buffer == NULL) {
        Serial.println("    -> FATAL: Failed to allocate PSRAM!");
        file.close(); return false;
    }
    
    if(file.read(note.buffer, note.size) != note.size) {
        Serial.println("    -> ERROR: File read failed.");
        free(note.buffer); note.buffer = NULL; file.close(); return false;
    }
    file.close();

    note.header.num_channels = *(uint16_t*)(note.buffer + 22);
    note.header.sample_rate = *(uint32_t*)(note.buffer + 24);
    note.header.bits_per_sample = *(uint16_t*)(note.buffer + 34);

    Serial.printf("    -> Loaded %s (%d Hz, %d-bit, %d chan)\n", 
        path, note.header.sample_rate, note.header.bits_per_sample, note.header.num_channels);

    if (note.header.num_channels != 1 || note.header.bits_per_sample != 8 || note.header.sample_rate != 96000) {
        Serial.println("    -> ERROR: WAV must be 8-bit, Mono, and 96kHz for mixing.");
        free(note.buffer); note.buffer = NULL; return false;
    }
    return true;
}
