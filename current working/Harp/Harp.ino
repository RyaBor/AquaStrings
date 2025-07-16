// --- INCLUDES ---
#include "driver/i2s.h"
#include "driver/adc.h" // ADDED: For modern ADC functions
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"
#include "freertos/queue.h"

// --- HARDWARE & PIN DEFINITIONS ---
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 26
#define SD_CS_PIN    5
#define SPI_MOSI_PIN 23
#define SPI_MISO_PIN 19
#define SPI_SCK_PIN  18
#define CONSTANT_HIGH_PIN 17 // Pin to be set and kept high

// CORRECTED PINS: Pin 16 is not an ADC pin. It has been replaced with 36.
// You must use valid ADC pins (e.g., GPIOs 32-39 on most ESP32s).
const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 27}; 

// ADDED: Map GPIO pins to their corresponding ADC1 channel enums
const adc1_channel_t ADC_CHANNELS[5] = {
    ADC1_CHANNEL_6, // Corresponds to GPIO 34
    ADC1_CHANNEL_7, // Corresponds to GPIO 35
    ADC1_CHANNEL_4, // Corresponds to GPIO 32
    ADC1_CHANNEL_5, // Corresponds to GPIO 33
    ADC1_CHANNEL_0  // Corresponds to GPIO 36
};

// --- GLOBAL CONSTANTS ---
const int NUM_SENSORS = 5;
const int WAV_HEADER_SIZE = 44;
// This buffer holds the 16-bit stereo data after conversion, before sending to I2S
const size_t I2S_PROCESSING_BUFFER_SIZE = 4096; 

// --- MANUAL THRESHOLD & DEBOUNCING ---
const int TRIGGER_THRESHOLD = 2000;
const int DEBOUNCE_DELAY = 50; // Time in ms for a sensor state to be stable before triggering
bool previousStates[NUM_SENSORS];
bool pendingStates[NUM_SENSORS];
unsigned long lastChangeTime[NUM_SENSORS];

// --- AUDIO FILE CONFIGURATION ---
// IMPORTANT: These files MUST be 8-bit, 96kHz, Mono WAV files.
const char* noteFiles[NUM_SENSORS] = {
    "/HNotes/C.wav", "/HNotes/D.wav", "/HNotes/E.wav",
    "/HNotes/G.wav", "/HNotes/A.wav"
};

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
};

AudioNote notes[NUM_SENSORS];

// --- RTOS & TASK MANAGEMENT ---
QueueHandle_t noteQueue;
TaskHandle_t audioTaskHandle = NULL;

// --- HARDWARE OBJECTS ---
Adafruit_TPA2016 audioamp = Adafruit_TPA2016();

// --- FUNCTION PROTOTYPES ---
bool loadNoteToPsram(const char* path, AudioNote& note);
void audioPlayerTask(void *pvParameters);
void playNote(int noteIndex);
bool initializeI2S(const WavHeader& header);

// =================================================================
// ---                           SETUP                           ---
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Laser Harp System Initializing (8-bit/96kHz Audio) ---");

    // --- ADC INITIALIZATION (REPLACED) ---
    Serial.println("Configuring ADC channels...");
    // Set the resolution to 12-bit (0-4095), same as analogRead
    adc1_config_width(ADC_WIDTH_BIT_12);
    // Configure the attenuation for each channel for a full 0-3.3V input range
    for (int i = 0; i < NUM_SENSORS; i++) {
        adc1_config_channel_atten(ADC_CHANNELS[i], ADC_ATTEN_DB_11);
    }
    Serial.println("ADC OK.");

    // --- Set Pin 17 HIGH ---
    pinMode(CONSTANT_HIGH_PIN, OUTPUT);
    digitalWrite(CONSTANT_HIGH_PIN, HIGH);
    Serial.printf("Pin %d has been set HIGH.\n", CONSTANT_HIGH_PIN);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!audioamp.begin()) {
        Serial.println("FATAL: TPA2016 amplifier not found. Halting.");
        while (1);
    }
    audioamp.setGain(24);
    Serial.printf("TPA2016 amplifier found! Gain: %d dB.\n", audioamp.getGain());

    if (!psramInit()) {
        Serial.println("FATAL: PSRAM failed!"); while (true);
    }
    Serial.println("PSRAM OK.");
    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("FATAL: SD Card mount failed!"); while (true);
    }
    Serial.println("SD Card OK.");

    Serial.println("Loading WAV files...");
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (!loadNoteToPsram(noteFiles[i], notes[i])) {
            Serial.printf("FATAL: Failed to load %s. Halting.\n", noteFiles[i]);
            while(true);
        }
    }
    Serial.println("All WAV files loaded successfully.");

    Serial.println("Initializing I2S audio driver...");
    if (!initializeI2S(notes[0].header)) {
        Serial.println("FATAL: Failed to initialize I2S driver. Halting.");
        while(true);
    }
    Serial.println("I2S driver OK.");

    for (int i = 0; i < NUM_SENSORS; i++) {
        previousStates[i] = false; pendingStates[i] = false; lastChangeTime[i] = 0;
    }

    noteQueue = xQueueCreate(5, sizeof(int));
    if(noteQueue == NULL){
        Serial.println("FATAL: Could not create note queue. Halting.");
        while(true);
    }

    Serial.println("Creating background audio playback task...");
    xTaskCreatePinnedToCore(
        audioPlayerTask, "AudioPlayer", 16384, NULL, 5, &audioTaskHandle, 1);

    Serial.println("\n--- System Ready! Monitoring for broken beams... ---");
    Serial.printf("Trigger threshold set to: %d\n", TRIGGER_THRESHOLD);
}

// =================================================================
// ---                         MAIN LOOP                         ---
// =================================================================
void loop() {
    for (int i = 0; i < NUM_SENSORS; i++) {
        // MODIFIED: Use the modern ESP-IDF function instead of analogRead()
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
                    Serial.printf("Sensor %d: ---> LASER BROKEN <---\n", i + 1);
                    xQueueSend(noteQueue, &i, (TickType_t)0);
                } else {
                    Serial.printf("Sensor %d: Laser Reconnected\n", i + 1);
                }
            }
        }
    }
    delay(5);
}

// =================================================================
// ---                     AUDIO FUNCTIONS                     ---
// =================================================================

void audioPlayerTask(void *pvParameters) {
    int noteIndexToPlay;
    while (true) {
        if (xQueueReceive(noteQueue, &noteIndexToPlay, portMAX_DELAY) == pdPASS) {
            if (!notes[noteIndexToPlay].isPlaying) {
                playNote(noteIndexToPlay);
            }
        }
    }
}

/**
 * @brief Plays an 8-bit audio note by converting it to 16-bit and streaming it.
 */
void playNote(int noteIndex) {
    if (noteIndex < 0 || noteIndex >= NUM_SENSORS) return;

    notes[noteIndex].isPlaying = true;
    Serial.printf("Audio Task: Playing note %d\n", noteIndex + 1);

    i2s_set_sample_rates(I2S_NUM_0, notes[noteIndex].header.sample_rate);

    uint8_t *audio_data_start = notes[noteIndex].buffer + WAV_HEADER_SIZE;
    size_t audio_data_size = notes[noteIndex].size - WAV_HEADER_SIZE;
    size_t bytes_sent = 0;
    
    // Buffer to hold the 16-bit stereo data after conversion
    uint8_t processing_buffer[I2S_PROCESSING_BUFFER_SIZE];

    while (bytes_sent < audio_data_size) {
        // Determine how many 8-bit source samples to process in this chunk
        // Each 8-bit sample becomes 4 bytes (16-bit Left + 16-bit Right)
        int samples_to_process = I2S_PROCESSING_BUFFER_SIZE / 4;

        // Don't process past the end of the file
        if (bytes_sent + samples_to_process > audio_data_size) {
            samples_to_process = audio_data_size - bytes_sent;
        }

        // Get a pointer to the 16-bit stereo processing buffer
        uint16_t* stereo_buffer_ptr = (uint16_t*)processing_buffer;

        // Process the chunk: Read 8-bit, convert to 16-bit stereo
        for (int i = 0; i < samples_to_process; i++) {
            uint8_t sample8bit = audio_data_start[bytes_sent + i];
            // Convert 8-bit unsigned (0-255) to 16-bit unsigned
            uint16_t sample16bit = ((uint16_t)sample8bit) << 8;
            // Write to stereo buffer (L/R channels get the same mono sample)
            *stereo_buffer_ptr++ = sample16bit;
            *stereo_buffer_ptr++ = sample16bit;
        }

        size_t bytes_to_write = samples_to_process * 4;
        size_t bytes_written = 0;
        
        i2s_write(I2S_NUM_0, processing_buffer, bytes_to_write, &bytes_written, portMAX_DELAY);
        
        if (bytes_written > 0) {
            // Advance the source buffer position by the number of 8-bit samples we processed
            bytes_sent += (bytes_written / 4);
        } else {
            Serial.println("Error writing to I2S, stopping playback.");
            break;
        }
    }
    
    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.printf("Audio Task: Finished note %d\n", noteIndex + 1);
    notes[noteIndex].isPlaying = false;
}


bool initializeI2S(const WavHeader& header) {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate = header.sample_rate,
        // We ALWAYS output 16-bit data to the DAC, even if the source is 8-bit.
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_NUM_0, NULL) != ESP_OK) return false;
    if (i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN) != ESP_OK) return false;
    
    return true;
}

bool loadNoteToPsram(const char* path, AudioNote& note) {
    File file = SD.open(path, "r");
    if (!file) {
        Serial.printf("   -> ERROR: Failed to open %s\n", path);
        return false;
    }
    
    note.size = file.size();
    if (note.size <= WAV_HEADER_SIZE) {
        Serial.println("   -> ERROR: File is too small.");
        file.close(); return false;
    }

    note.buffer = (uint8_t *)ps_malloc(note.size);
    if (note.buffer == NULL) {
        Serial.println("   -> FATAL: Failed to allocate PSRAM!");
        file.close(); return false;
    }
    
    if(file.read(note.buffer, note.size) != note.size) {
        Serial.println("   -> ERROR: File read failed.");
        free(note.buffer); note.buffer = NULL; file.close(); return false;
    }
    file.close();

    note.header.num_channels = *(uint16_t*)(note.buffer + 22);
    note.header.sample_rate = *(uint32_t*)(note.buffer + 24);
    note.header.bits_per_sample = *(uint16_t*)(note.buffer + 34);

    Serial.printf("   -> Loaded %s (%d Hz, %d-bit, %d chan)\n", 
        path, note.header.sample_rate, note.header.bits_per_sample, note.header.num_channels);

    // --- VALIDATION FOR 8-BIT MONO ---
    if (note.header.num_channels != 1 || note.header.bits_per_sample != 8) {
        Serial.println("   -> ERROR: WAV must be 8-bit, Mono.");
        free(note.buffer); note.buffer = NULL; return false;
    }
    return true;
}