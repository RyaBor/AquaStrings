#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS         5
#define SPI_MOSI      23
#define SPI_MISO      19
#define SPI_SCK       18

File root;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 SD Card Test");
  
  // Initialize SPI bus for SD card
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  
  // Initialize SD card
  Serial.println("Initializing SD card...");
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card initialization failed!");
    while (1);
  }
  Serial.println("SD card initialized successfully.");
  
  // List all files in SD card
  root = SD.open("/");
  Serial.println("Files on SD card:");
  listFiles(root, 0);
  
  // Try to open each WAV file and read its header
  checkWavFiles();
}

void loop() {
  // Nothing to do here
  delay(1000);
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