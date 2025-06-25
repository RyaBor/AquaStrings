#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "driver/i2s.h"

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

// Audio enhancement settings
#define VOLUME_LEVEL      0.8    // 0.0 to 1.0 (80% volume to prevent clipping)
#define DC_BIAS_ADJUSTMENT 0     // Fine-tune if needed
#define USE_DITHERING     true   // Adds small noise to reduce quantization effects

// Filter settings - adjust these for your audio
#define IIR_FILTER_ORDER  3      // Second-order filter
float iirCoeffsB[IIR_FILTER_ORDER] = {0.2, 0.3, 0.5}; // Input coefficients (b values)
float iirCoeffsA[IIR_FILTER_ORDER] = {1.0, 0.0, 0.0}; // Output coefficients (a values, a[0] must be 1.0)
float iirInputHistory[IIR_FILTER_ORDER] = {0};        // x history
float iirOutputHistory[IIR_FILTER_ORDER] = {0};       // y history

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
int16_t applyIIRFilter(int16_t newSample);
void resetFilters();

void setup() {
  Serial.begin(115200);
  while(!Serial) { delay(10); }
  delay(1000);
  Serial.println("ESP32 SD Card and DAC Audio Player with Enhanced Audio");
  
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
  
  // Reset filters to initial state
  resetFilters();
  
  // List files and check WAV files
  root = SD.open("/");
  Serial.println("Files on SD card:");
  listFiles(root, 0);
  checkWavFiles();
  
  Serial.println("\nReady to play WAV files with enhanced audio quality.");
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
      // Convert to the format needed by internal DAC with audio enhancement
      size_t i2s_bytes_written = 0;
      uint16_t i2s_buffer[I2S_BUFFER_SIZE];
      
      for (int i = 0; i < bytesRead; i++) {
        // Convert 8-bit PCM to 16-bit for DAC (center around zero)
        int16_t sample = (buffer[i] - 128) * 256;
        
        // Apply advanced audio filtering
        sample = applyIIRFilter(sample);
        
        // Apply volume control to prevent clipping
        sample = (int16_t)(sample * VOLUME_LEVEL);
        
        // Apply DC bias adjustment if needed
        sample += DC_BIAS_ADJUSTMENT;
        
        // Apply dithering to reduce quantization noise
        if (USE_DITHERING) {
          sample += random(-8, 8);  // Slight dithering
        }
        
        // Ensure sample is within valid range
        sample = constrain(sample, -32768, 32767);
        
        // Store in I2S buffer
        i2s_buffer[i] = sample;
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

// Improved IIR filter implementation
int16_t applyIIRFilter(int16_t newSample) {
  // Shift input history
  for (int i = IIR_FILTER_ORDER - 1; i > 0; i--) {
    iirInputHistory[i] = iirInputHistory[i-1];
  }
  iirInputHistory[0] = newSample;
  
  // Calculate new output based on IIR filter formula
  float outputSample = 0;
  
  // Apply input side (FIR part)
  for (int i = 0; i < IIR_FILTER_ORDER; i++) {
    outputSample += iirCoeffsB[i] * iirInputHistory[i];
  }
  
  // Apply output side (recursive part) - skipping a[0] which must be 1.0
  for (int i = 1; i < IIR_FILTER_ORDER; i++) {
    outputSample -= iirCoeffsA[i] * iirOutputHistory[i-1];
  }
  
  // Shift output history
  for (int i = IIR_FILTER_ORDER - 1; i > 0; i--) {
    iirOutputHistory[i] = iirOutputHistory[i-1];
  }
  iirOutputHistory[0] = outputSample;
  
  return (int16_t)outputSample;
}

void resetFilters() {
  // Reset all filter states
  for (int i = 0; i < IIR_FILTER_ORDER; i++) {
    iirInputHistory[i] = 0;
    iirOutputHistory[i] = 0;
  }
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
      Serial.println("Playback started with enhanced audio quality");
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
    .dma_buf_count = 16,  // Increased buffer count for smoother playback
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
  
  Serial.println("I2S initialized with internal DAC and enhanced audio processing");
}

bool playWavFile(const char* filename) {
  // Close any open file
  if (wavFile) {
    wavFile.close();
  }
  
  // Reset audio processing state
  resetFilters();
  
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
      
      // List root directory to see what's there
      root = SD.open("/");
      Serial.println("Files in root directory:");
      listFiles(root, 0);
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
  
  Serial.print("Playing WAV with enhanced audio: ");
  Serial.print(sampleRate);
  Serial.print(" Hz, ");
  Serial.print(numChannels);
  Serial.print(" channels, ");
  Serial.print(bitsPerSample);
  Serial.println(" bits per sample");
  
  // Adjust filter coefficients based on sample rate if needed
  if (sampleRate > 32000) {
    // Higher sample rate needs different filter characteristics
    iirCoeffsB[0] = 0.15;
    iirCoeffsB[1] = 0.25;
    iirCoeffsB[2] = 0.6;
  } else if (sampleRate < 16000) {
    // Lower sample rate needs different filter characteristics
    iirCoeffsB[0] = 0.25;
    iirCoeffsB[1] = 0.35;
    iirCoeffsB[2] = 0.4;
  }
  
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
    
    // Reset filter states
    resetFilters();
    
    // Flush I2S buffer
    i2s_zero_dma_buffer(I2S_NUM);
  }
}

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