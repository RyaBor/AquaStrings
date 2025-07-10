// --- Included Libraries ---
#include "driver/i2s.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"

// --- Pin Definitions ---

// SD Card Pins
#define SD_CS    5
#define SPI_MOSI 23
#define SPI_MISO 19
#define SPI_SCK  18

// I2C Pins for Amplifier
#define I2C_SDA 22
#define I2C_SCL 26

// String Sensor Pins (5 Strings)
const int NUM_SENSORS = 5;
const int PHOTODIODE_PINS[NUM_SENSORS] = {34, 35, 32, 33, 27}; // Photodiode input pins

// --- Audio Files ---
// Ensure these files exist in the /HNOTES/ folder on your SD card.
const char* SOUND_FILES[NUM_SENSORS] = {
  "/HNotes/C.wav",
  "/HNotes/A.wav",
  "/HNotes/G.wav",
  "/HNotes/E.wav",
  "/HNotes/D.wav"
};

// --- Global Variables & Data Structures ---

// Amplifier Object
Adafruit_TPA2016 audioamp = Adafruit_TPA2016();

const int WAV_HEADER_SIZE = 44;

// Struct to hold WAV file properties
struct WavHeader {
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t num_channels;
};

// Struct to hold preloaded audio data
struct PreloadedNote {
    uint8_t* buffer = NULL;
    size_t size = 0;
    WavHeader header;
};

PreloadedNote preloadedNotes[NUM_SENSORS];

// Sensor threshold to determine if a laser is broken
const int THRESHOLD = 2000;

// Variables to store previous states for change detection
bool previousStates[NUM_SENSORS] = {false};
// Flag to prevent multiple sounds from playing at once
volatile bool isPlaying = false;


// --- Function Prototypes ---
void preloadAllNotes();
bool configureI2sFromWav(const WavHeader& header);
void playWavFile(const PreloadedNote& note);
void playNote(int noteIndex);
void checkStrings();

//================================================================================
// SETUP FUNCTION
//================================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("AquaStrings Initializing...");

    // --- Initialize I2C and Amplifier ---
    Wire.begin(I2C_SDA, I2C_SCL);
    if (!audioamp.begin()) {
        Serial.println("Could not find TPA2016 amplifier, check wiring!");
        while (1);
    }
    Serial.println("TPA2016 amplifier found!");
    audioamp.setGain(-10); // Set gain from -28dB to +30dB.
    Serial.printf("Amplifier gain set to: %d dB\n", audioamp.getGain());

    // --- Initialize PSRAM ---
    if (!psramInit()) {
        Serial.println("PSRAM not found or failed to initialize!");
        while (true);
    }
    Serial.println("PSRAM initialized.");

    // --- Initialize SD Card ---
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card mount failed!");
        while (true);
    }
    Serial.println("SD Card initialized.");

    // --- Preload all sound files into PSRAM ---
    preloadAllNotes();

    // Photodiode pins are analog inputs by default, no setup needed.
    Serial.println("Sensor pins are ready.");
    Serial.println("\nSystem Ready. Break a water stream to play a note!");
}

//================================================================================
// MAIN LOOP
//================================================================================
void loop() {
    // Continuously check the state of the laser strings
    checkStrings();

    // A small delay to keep the system responsive without overwhelming the CPU
    delay(20);
}

//================================================================================
// SENSOR AND SYSTEM LOGIC
//================================================================================

/**
 * @brief Checks the state of all laser strings and triggers notes.
 */
void checkStrings() {
    for (int i = 0; i < NUM_SENSORS; i++) {
        int photodiodeValue = analogRead(PHOTODIODE_PINS[i]);
        bool laserBroken = photodiodeValue < THRESHOLD;

        if (laserBroken && !previousStates[i]) {
            Serial.printf("String %d BROKEN. Value: %d\n", i + 1, photodiodeValue);
            if (!isPlaying) {
                playNote(i);
            } else {
                Serial.println("Audio system is busy. Note skipped.");
            }
        }
        previousStates[i] = laserBroken;
    }
}

/**
 * @brief Main function to handle playing a note for a given string index.
 * @param noteIndex The index (0-4) of the string/note to play.
 */
void playNote(int noteIndex) {
    if (noteIndex < 0 || noteIndex >= NUM_SENSORS || preloadedNotes[noteIndex].buffer == NULL) {
        Serial.println("Invalid note index or note not loaded.");
        return;
    }

    isPlaying = true;
    Serial.printf("Playing preloaded note %d...\n", noteIndex + 1);

    if (configureI2sFromWav(preloadedNotes[noteIndex].header)) {
        playWavFile(preloadedNotes[noteIndex]);
    }

    isPlaying = false;
    Serial.println("Ready for next note.");
}


//================================================================================
// PRELOADING AND AUDIO PLAYBACK FUNCTIONS
//================================================================================

/**
 * @brief Iterates through all sound files and loads them into PSRAM.
 */
void preloadAllNotes() {
    Serial.println("Starting to preload notes into PSRAM...");
    for (int i = 0; i < NUM_SENSORS; i++) {
        File file = SD.open(SOUND_FILES[i]);
        if (!file) {
            Serial.printf("Failed to open %s for preloading.\n", SOUND_FILES[i]);
            continue; // Skip to the next file
        }

        size_t fileSize = file.size();
        if (fileSize <= WAV_HEADER_SIZE) {
            Serial.printf("File %s is too small to be a valid WAV.\n", SOUND_FILES[i]);
            file.close();
            continue;
        }

        // Allocate memory from PSRAM
        preloadedNotes[i].buffer = (uint8_t*)ps_malloc(fileSize);
        if (preloadedNotes[i].buffer == NULL) {
            Serial.printf("Failed to allocate %d bytes in PSRAM for note %d!\n", fileSize, i);
            file.close();
            continue; // Not enough memory, skip.
        }

        // Read file into the PSRAM buffer
        size_t bytesRead = file.read(preloadedNotes[i].buffer, fileSize);
        file.close();

        if (bytesRead == fileSize) {
            preloadedNotes[i].size = fileSize;
            // Parse the header from the buffer
            preloadedNotes[i].header.num_channels = *(uint16_t*)(preloadedNotes[i].buffer + 22);
            preloadedNotes[i].header.sample_rate = *(uint32_t*)(preloadedNotes[i].buffer + 24);
            preloadedNotes[i].header.bits_per_sample = *(uint16_t*)(preloadedNotes[i].buffer + 34);
            Serial.printf("Successfully preloaded %s (%d bytes).\n", SOUND_FILES[i], fileSize);
        } else {
            Serial.printf("Failed to read full file for %s. Read %d bytes.\n", SOUND_FILES[i], bytesRead);
            free(preloadedNotes[i].buffer); // Free the failed allocation
            preloadedNotes[i].buffer = NULL;
        }
    }
    Serial.println("Note preloading complete.");
}


/**
 * @brief Configures the I2S driver based on pre-parsed WAV header info.
 */
bool configureI2sFromWav(const WavHeader& header) {
    Serial.printf("Configuring I2S for: %d Ch, %d Hz, %d-bit\n", header.num_channels, header.sample_rate, header.bits_per_sample);

    if (header.num_channels != 1 || (header.bits_per_sample != 8 && header.bits_per_sample != 16)) {
        Serial.println("Error: Only 8 or 16-bit mono WAV files are supported.");
        return false;
    }

    i2s_driver_uninstall(I2S_NUM_0); 

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate = header.sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, NULL);

    return true;
}

/**
 * @brief Plays the .wav file from its PSRAM buffer with a pop-prevention sequence.
 */
void playWavFile(const PreloadedNote& note) {
    if (note.buffer == NULL) return;

    i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
    
    uint16_t silence_buffer[256] = {0};
    for(int i=0; i<256; i++) silence_buffer[i] = 0x8000;
    size_t bytes_written = 0;
    i2s_write(I2S_NUM_0, silence_buffer, sizeof(silence_buffer), &bytes_written, portMAX_DELAY);

    uint8_t processing_buffer[2048];
    size_t bytes_read_from_psram = 0;
    uint8_t *audio_data_start = note.buffer + WAV_HEADER_SIZE;
    size_t audio_data_size = note.size - WAV_HEADER_SIZE;

    while (bytes_read_from_psram < audio_data_size) {
        int bytes_to_process = sizeof(processing_buffer);
        int samples_in_buffer = 0;

        if (note.header.bits_per_sample == 8) {
            samples_in_buffer = sizeof(processing_buffer) / 4;
            if (bytes_read_from_psram + samples_in_buffer > audio_data_size) {
                samples_in_buffer = audio_data_size - bytes_read_from_psram;
            }
            bytes_to_process = samples_in_buffer * 4;
            uint16_t* stereo_buffer_ptr = (uint16_t*)processing_buffer;
            for (int i = 0; i < samples_in_buffer; i++) {
                uint16_t sample = ((uint16_t)(audio_data_start[bytes_read_from_psram + i])) << 8;
                *stereo_buffer_ptr++ = sample;
                *stereo_buffer_ptr++ = sample;
            }
            bytes_read_from_psram += samples_in_buffer;
        } else if (note.header.bits_per_sample == 16) {
            samples_in_buffer = sizeof(processing_buffer) / 4;
            size_t source_bytes_to_read = samples_in_buffer * 2;
            if (bytes_read_from_psram + source_bytes_to_read > audio_data_size) {
                source_bytes_to_read = audio_data_size - bytes_read_from_psram;
                samples_in_buffer = source_bytes_to_read / 2;
            }
            bytes_to_process = samples_in_buffer * 4;
            uint16_t* stereo_buffer_ptr = (uint16_t*)processing_buffer;
            int16_t* source_ptr = (int16_t*)(audio_data_start + bytes_read_from_psram);
            for (int i = 0; i < samples_in_buffer; i++) {
                uint16_t sample = (*source_ptr++) + 32768;
                *stereo_buffer_ptr++ = sample;
                *stereo_buffer_ptr++ = sample;
            }
            bytes_read_from_psram += source_bytes_to_read;
        }
        i2s_write(I2S_NUM_0, processing_buffer, bytes_to_process, &bytes_written, portMAX_DELAY);
    }

    i2s_write(I2S_NUM_0, silence_buffer, sizeof(silence_buffer), &bytes_written, portMAX_DELAY);
    i2s_stop(I2S_NUM_0);
    i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
    
    Serial.println("Playback complete.");
}
