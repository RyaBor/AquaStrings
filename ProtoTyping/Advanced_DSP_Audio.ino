#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "driver/i2s.h"
#include <math.h>

// SD Card pins
#define SD_CS         5
#define SPI_MOSI      23
#define SPI_MISO      19
#define SPI_SCK       18

// I2S configuration for internal DAC
#define I2S_NUM           I2S_NUM_0
#define I2S_SAMPLE_RATE   22050  // Match your WAV file sample rate
#define I2S_BUFFER_SIZE   512
#define I2S_DAC_CHANNEL   I2S_DAC_CHANNEL_RIGHT_EN  // GPIO25 (DAC channel 1)

// Master audio processing settings
#define MASTER_VOLUME     0.90   // 0.0 to 1.0
#define DC_BIAS_ADJUSTMENT -100     // Fine-tune if needed (-100 to 100)
#define USE_DITHERING      true   // Noise shaping dithering

// Advanced DSP modules enable/disable
#define USE_PARAMETRIC_EQ  true   // Professional 3-band parametric EQ
#define USE_COMPRESSOR     true   // Dynamic range compression
#define USE_EXCITER        true   // Add controlled harmonics

// Parametric EQ settings (3 bands for better performance)
#define EQ_NUM_BANDS      3
typedef struct {
    float freq;    // Center frequency
    float gain;    // Gain in dB (-12 to +12)
    float q;       // Q factor (bandwidth)
} EQBand;

// EQ bands configuration (freq, gain, q)
EQBand eqBands[EQ_NUM_BANDS] = {
    {100.0f,  2.0f, 1.0f},  // Low shelf: boost bass
    {1000.0f, 0.0f, 1.0f},  // Mids
    {3000.0f, 3.0f, 1.0f}   // Upper mids: boost presence
};

// EQ filter states
float eqState[EQ_NUM_BANDS][4] = {{0}};
float eqCoeffs[EQ_NUM_BANDS][5] = {{0}};

// Compressor settings
#define COMP_THRESHOLD    0.4f    // Threshold above which compression starts (0.0 to 1.0)
#define COMP_RATIO        3.0f    // Compression ratio (higher = more compression)
#define COMP_ATTACK       0.01f   // Attack time in seconds
#define COMP_RELEASE      0.1f    // Release time in seconds
#define COMP_MAKEUP_GAIN  1.2f    // Makeup gain after compression
float compLevel = 0.0f;
float compGain = 1.0f;

// Exciter settings
#define EX_DRIVE          0.3f    // Drive amount (0.0 to 1.0)
#define EX_BLEND          0.25f   // Blend with original (0.0 to 1.0)
#define EX_FREQ           2000.0f // Frequency above which harmonics are added
float exState[2] = {0};

File root;
File wavFile;
bool isPlaying = false;
String serialBuffer = "";

// Function prototypes
void listFiles(File dir, int numTabs);
void checkWavFiles();
bool playWavFile(const char* filename);
void stopPlayback();
void setupI2S();
void initializeAudioProcessing(uint32_t sampleRate);
void resetAudioProcessing();
float applyAudioProcessing(float input);
void calculateBiquadCoeffs(float *coeffs, float frequency, float q, float gain, float sampleRate, int type);
float applyBiquad(float input, float *coeffs, float *state);
float applyCompressor(float input);
float applyExciter(float input);
float applyParametricEQ(float input);
void processCommand(String command);

void setup() {
  Serial.begin(115200);
  while(!Serial) { delay(10); }
  delay(1000);
  Serial.println("ESP32 SD Card and DAC Audio Player with Enhanced Processing");
  
  // Initialize SPI bus for SD card
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  
  // Initialize SD card
  Serial.println("Initializing SD card...");
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card initialization failed!");
    while (1);
  }
  Serial.println("SD card initialized successfully.");
  
  // Set up I2S with internal DAC
  setupI2S();
  
  // Initialize audio processing
  initializeAudioProcessing(I2S_SAMPLE_RATE);
  
  // List files and check WAV files
  root = SD.open("/");
  Serial.println("Files on SD card:");
  listFiles(root, 0);
  checkWavFiles();
  
  Serial.println("\nReady to play WAV files with enhanced audio processing.");
  Serial.println("Type the name of a WAV file to play or 'stop' to stop playback.");
}

void loop() {
  // Improved serial input handling
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        processCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
    delay(1); // Small delay to allow serial buffer to fill
  }
  
  // Handle audio playback if active
  if (isPlaying) {
    uint8_t buffer[I2S_BUFFER_SIZE];
    size_t bytesRead = wavFile.read(buffer, I2S_BUFFER_SIZE);
    
    if (bytesRead > 0) {
      // Process and output audio
      size_t i2s_bytes_written = 0;
      uint16_t i2s_buffer[I2S_BUFFER_SIZE];
      
      for (int i = 0; i < bytesRead; i++) {
        // Convert 8-bit PCM to normalized float (-1.0 to 1.0)
        float sample = ((float)buffer[i] - 128.0f) / 128.0f;
        
        // Apply full audio processing chain
        sample = applyAudioProcessing(sample);
        
        // Convert back to 16-bit integer for DAC
        int16_t sampleInt = (int16_t)(sample * 32767.0f);
        
        // Apply dithering
        if (USE_DITHERING) {
          sampleInt += (random(0, 7) - 3);  // Noise-shaped dithering
        }
        
        // Ensure sample is within valid range
        sampleInt = constrain(sampleInt, -32768, 32767);
        
        // Store in I2S buffer
        i2s_buffer[i] = sampleInt;
      }
      
      // Write to I2S (non-blocking)
      i2s_write(I2S_NUM, i2s_buffer, bytesRead * 2, &i2s_bytes_written, 0);
    } else {
      // End of file
      Serial.println("End of file reached. Stopping playback.");
      stopPlayback();
    }
  }
}

// Main audio processing function
float applyAudioProcessing(float input) {
  float output = input;
  
  // Apply parametric EQ
  if (USE_PARAMETRIC_EQ) {
    output = applyParametricEQ(output);
  }
  
  // Apply compression
  if (USE_COMPRESSOR) {
    output = applyCompressor(output);
  }
  
  // Apply exciter for harmonic enhancement
  if (USE_EXCITER) {
    output = applyExciter(output);
  }
  
  // Apply master volume
  output *= MASTER_VOLUME;
  
  // Add DC bias adjustment if needed
  output += (DC_BIAS_ADJUSTMENT / 32768.0f);
  
  return output;
}

// Initialize audio processing based on sample rate
void initializeAudioProcessing(uint32_t sampleRate) {
  float fs = (float)sampleRate;
  
  // Calculate EQ coefficients
  for (int i = 0; i < EQ_NUM_BANDS; i++) {
    calculateBiquadCoeffs(eqCoeffs[i], eqBands[i].freq, eqBands[i].q, eqBands[i].gain, fs, 0); // Type 0 = Peak/Bell EQ
  }
  
  resetAudioProcessing();
}

// Reset all audio processing states
void resetAudioProcessing() {
  // Reset EQ states
  for (int i = 0; i < EQ_NUM_BANDS; i++) {
    memset(eqState[i], 0, sizeof(eqState[i]));
  }
  
  // Reset compressor states
  compLevel = 0.0f;
  compGain = 1.0f;
  
  // Reset exciter state
  memset(exState, 0, sizeof(exState));
}

// Calculate biquad filter coefficients
// types: 0=peaking, 1=lowpass, 2=highpass
void calculateBiquadCoeffs(float *coeffs, float frequency, float q, float gainDB, float sampleRate, int type) {
  float A = pow(10.0f, gainDB / 40.0f);
  float omega = 2.0f * M_PI * frequency / sampleRate;
  float sn = sin(omega);
  float cs = cos(omega);
  float alpha = sn / (2.0f * q);
  
  float b0, b1, b2, a0, a1, a2;
  
  if (type == 0) { // Peaking EQ
    b0 = 1.0f + alpha * A;
    b1 = -2.0f * cs;
    b2 = 1.0f - alpha * A;
    a0 = 1.0f + alpha / A;
    a1 = -2.0f * cs;
    a2 = 1.0f - alpha / A;
  } else if (type == 1) { // Low-pass
    b0 = (1.0f - cs) / 2.0f;
    b1 = 1.0f - cs;
    b2 = (1.0f - cs) / 2.0f;
    a0 = 1.0f + alpha;
    a1 = -2.0f * cs;
    a2 = 1.0f - alpha;
  } else if (type == 2) { // High-pass
    b0 = (1.0f + cs) / 2.0f;
    b1 = -(1.0f + cs);
    b2 = (1.0f + cs) / 2.0f;
    a0 = 1.0f + alpha;
    a1 = -2.0f * cs;
    a2 = 1.0f - alpha;
  }
  
  // Normalize by a0
  coeffs[0] = b0 / a0;
  coeffs[1] = b1 / a0;
  coeffs[2] = b2 / a0;
  coeffs[3] = a1 / a0;
  coeffs[4] = a2 / a0;
}

// Apply a single biquad filter
float applyBiquad(float input, float *coeffs, float *state) {
  // Direct Form II implementation
  float w = input - coeffs[3] * state[0] - coeffs[4] * state[1];
  float output = coeffs[0] * w + coeffs[1] * state[0] + coeffs[2] * state[1];
  
  // Update state
  state[1] = state[0];
  state[0] = w;
  
  return output;
}

// Apply parametric EQ (all bands)
float applyParametricEQ(float input) {
  float output = input;
  
  // Apply each EQ band in series
  for (int i = 0; i < EQ_NUM_BANDS; i++) {
    output = applyBiquad(output, eqCoeffs[i], eqState[i]);
  }
  
  return output;
}

// Apply compression
float applyCompressor(float input) {
  float inputAbs = fabs(input);
  
  // Update level detector with smoothing
  if (inputAbs > compLevel) {
    compLevel += (inputAbs - compLevel) * COMP_ATTACK;
  } else {
    compLevel += (inputAbs - compLevel) * COMP_RELEASE;
  }
  
  // Calculate gain reduction
  float targetGain = 1.0f;
  if (compLevel > COMP_THRESHOLD) {
    float excess = compLevel - COMP_THRESHOLD;
    float reduction = excess * (1.0f - 1.0f/COMP_RATIO);
    targetGain = (compLevel - reduction) / compLevel;
  }
  
  // Smooth gain changes
  compGain += (targetGain - compGain) * COMP_ATTACK;
  
  // Apply gain with makeup
  return input * compGain * COMP_MAKEUP_GAIN;
}

// Apply exciter effect for harmonic enhancement
float applyExciter(float input) {
  // High-pass filter to isolate high frequencies
  float highPass = input - exState[0];
  exState[0] = exState[0] + highPass * (EX_FREQ / (float)I2S_SAMPLE_RATE);
  
  // Generate harmonics through soft clipping
  float drive = highPass * (1.0f + EX_DRIVE * 10.0f);
  float harmonics = (drive > 0) ? 
                   1.0f - expf(-drive) : 
                   -1.0f + expf(drive);
  
  // Blend with original
  return input * (1.0f - EX_BLEND) + harmonics * EX_BLEND;
}

void processCommand(String command) {
  command.trim();
  Serial.print("Processing command: ");
  Serial.println(command);
  
  if (command.equals("stop")) {
    stopPlayback();
    Serial.println("Playback stopped");
  } else if (command.endsWith(".wav") || command.endsWith(".WAV")) {
    // Stop current playback if any
    stopPlayback();
    
    // Play the new file - ensure proper path formatting
    String filepath = command;
    if (!filepath.startsWith("/")) {
      filepath = "/" + filepath;  // Add leading slash for SD card paths
    }
    
    Serial.print("Attempting to play: ");
    Serial.println(filepath);
    
    if (playWavFile(filepath.c_str())) {
      Serial.println("Playback started with enhanced audio processing");
    } else {
      Serial.println("Failed to start playback");
    }
  } else {
    Serial.println("Invalid command. Use a WAV filename or 'stop'");
  }
}

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 16,  // Increased buffer count
    .dma_buf_len = 128,   // Increased buffer length
    .use_apll = true,     // Use APLL for better clock precision
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  
  // Install and start I2S driver
  i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
  
  // Enable the DAC output
  i2s_set_dac_mode(I2S_DAC_CHANNEL);
  
  // Clear any old data from the buffer
  i2s_zero_dma_buffer(I2S_NUM);
  
  Serial.println("I2S initialized with internal DAC and enhanced processing");
}

bool playWavFile(const char* filename) {
  // Close any open file
  if (wavFile) {
    wavFile.close();
  }
  
  // Reset audio processing state
  resetAudioProcessing();
  
  // Open the WAV file
  Serial.print("Opening file: ");
  Serial.println(filename);
  
  wavFile = SD.open(filename);
  if (!wavFile) {
    Serial.print("Failed to open file: ");
    Serial.println(filename);
    
    // Debug SD access
    if (SD.exists(filename)) {
      Serial.println("File exists but couldn't be opened");
    } else {
      Serial.println("File does not exist");
    }
    
    return false;
  }
  
  Serial.println("File opened successfully");
  Serial.print("File size: ");
  Serial.println(wavFile.size());
  
  // Read WAV header
  char header[44];
  size_t bytesRead = wavFile.read((uint8_t*)header, 44);
  
  if (bytesRead != 44) {
    Serial.println("Failed to read WAV header");
    wavFile.close();
    return false;
  }
  
  // Verify WAV header
  if (!(header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
        header[8] == 'W' && header[9] == 'A' && header[10] == 'V' && header[11] == 'E')) {
    Serial.println("Not a valid WAV file");
    wavFile.close();
    return false;
  }
  
  // Extract audio parameters
  uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
  uint16_t numChannels = header[22] | (header[23] << 8);
  uint16_t bitsPerSample = header[34] | (header[35] << 8);
  
  Serial.print("Playing WAV with enhanced processing: ");
  Serial.print(sampleRate);
  Serial.print(" Hz, ");
  Serial.print(numChannels);
  Serial.print(" channels, ");
  Serial.print(bitsPerSample);
  Serial.println(" bits per sample");
  
  // Initialize audio processing for this sample rate
  initializeAudioProcessing(sampleRate);
  
  // Set playback state
  isPlaying = true;
  
  return true;
}

void stopPlayback() {
  if (isPlaying) {
    isPlaying = false;
    if (wavFile) {
      wavFile.close();
    }
    
    // Reset audio processing state
    resetAudioProcessing();
    
    // Flush I2S buffer
    i2s_zero_dma_buffer(I2S_NUM);
  }
}

// List all files in directory and subdirectories
void listFiles(File dir, int numTabs) {
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      // No more files
      break;
    }
    
    // Print file details
    for (uint8_t i = 0; i < numTabs; i++) {
      Serial.print('\t');
    }
    
    Serial.print(entry.name());
    if (entry.isDirectory()) {
      Serial.println("/");
      listFiles(entry, numTabs + 1);
    } else {
      // Print file size
      Serial.print("\t\t");
      Serial.print(entry.size(), DEC);
      Serial.println(" bytes");
    }
    entry.close();
  }
}

// Check WAV files and analyze their headers
void checkWavFiles() {
  root = SD.open("/");
  
  Serial.println("\nChecking WAV files:");
  
  while (true) {
    File entry = root.openNextFile();
    if (!entry) {
      break;
    }
    
    String filename = entry.name();
    if (!entry.isDirectory() && filename.endsWith(".wav")) {
      Serial.print("Found WAV file: ");
      Serial.println(filename);
      
      // Try to read and verify WAV header
      char header[44]; // Standard WAV header is 44 bytes
      size_t bytesRead = entry.read((uint8_t*)header, 44);
      
      if (bytesRead == 44) {
        // Check if it's a valid WAV file
        if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
            header[8] == 'W' && header[9] == 'A' && header[10] == 'V' && header[11] == 'E') {
          
          // Extract sample rate
          uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
          
          // Extract number of channels
          uint16_t numChannels = header[22] | (header[23] << 8);
          
          // Extract bits per sample
          uint16_t bitsPerSample = header[34] | (header[35] << 8);
          
          Serial.print("  Sample Rate: ");
          Serial.print(sampleRate);
          Serial.print(" Hz, Channels: ");
          Serial.print(numChannels);
          Serial.print(", Bits Per Sample: ");
          Serial.println(bitsPerSample);
          
          // Check if supported by ESP32 DAC
          if (sampleRate > 44100) {
            Serial.println("  WARNING: Sample rate may be too high for ESP32 DAC");
          }
          
          if (bitsPerSample != 8 && bitsPerSample != 16) {
            Serial.println("  WARNING: ESP32 works best with 8-bit or 16-bit samples");
          }
          
        } else {
          Serial.println("  Not a valid WAV file or header is corrupt");
        }
      } else {
        Serial.println("  Could not read header - file may be damaged");
      }
    }
    entry.close();
  }
  
  Serial.println("WAV file check complete");
}