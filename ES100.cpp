/**
 * @file      ES100.cpp
 * @brief     ES100 WWVB Receiver Driver Implementation
 * @details   I2C driver for Everset ES100 WWVB atomic clock receiver
 */

#include "ES100.h"

// ============================================================================
// Constructor
// ============================================================================
ES100::ES100(uint8_t enPin, uint8_t irqPin)
    : _enPin(enPin), _irqPin(irqPin), _wire(nullptr),
      _sdaPin(-1), _sclPin(-1),
      _receiving(false), _initialized(false) {
}

// ============================================================================
// Initialization
// ============================================================================
bool ES100::begin(TwoWire *wirePort, int sdaPin, int sclPin) {
    _wire = wirePort;
    _sdaPin = sdaPin;
    _sclPin = sclPin;
    _initialized = false;

    // Configure pins
    pinMode(_enPin, OUTPUT);
    pinMode(_irqPin, INPUT_PULLUP);  // IRQ is active low, open-drain

    // Start with ES100 powered off
    digitalWrite(_enPin, LOW);
    delay(10);

    // Power on ES100
    powerOn();
    delay(ES100_WAKEUP_TIME_MS + 10);  // Wait for wakeup plus margin
    
    // Verify device ID
    uint8_t deviceId = readDeviceID();
    Serial.printf("ES100 Device ID: 0x%02X (expected 0x%02X)\n", deviceId, ES100_DEVICE_ID);
    
    if (deviceId == ES100_DEVICE_ID) {
        _initialized = true;
        // Power off after verification — ES100 should only be on during reception
        // to avoid interfering with other devices on the shared I2C bus
        powerOff();
        Serial.println("ES100 initialization successful (powered off until needed)");
        return true;
    } else if (deviceId == 0xFF || deviceId == 0x00) {
        powerOff();
        Serial.println("ES100 not responding - check I2C connections");
        return false;
    } else {
        powerOff();
        Serial.println("ES100 unexpected device ID");
        return false;
    }
}

// ============================================================================
// Power Control
// ============================================================================
bool ES100::isPoweredOn() {
    return digitalRead(_enPin) == HIGH;
}

void ES100::powerOn() {
    Serial.println("[ES100] Powering on (EN HIGH)...");
    digitalWrite(_enPin, HIGH);
    delay(ES100_WAKEUP_TIME_MS);  // Wait for ES100 to wake up

    // Recover I2C bus - ES100 power-up can glitch SDA/SCL
    recoverBus();
    Serial.println("[ES100] Power on complete, bus recovered");
}

void ES100::powerOff() {
    digitalWrite(_enPin, LOW);
    _receiving = false;
}

// ============================================================================
// Device Identification
// ============================================================================
uint8_t ES100::readDeviceID() {
    if (!isPoweredOn()) {
        powerOn();
    }
    return readRegister(ES100_REG_DEVICE_ID);
}

// ============================================================================
// Reception Control
// ============================================================================
bool ES100::startReception(uint8_t mode) {
    if (!_initialized) {
        Serial.println("ES100 not initialized");
        return false;
    }
    
    // Ensure ES100 is powered on
    if (!isPoweredOn()) {
        powerOn();
        delay(ES100_WAKEUP_TIME_MS);
    }
    
    // Write control register to start reception
    // This clears status/time registers and begins receiving
    if (writeRegister(ES100_REG_CONTROL0, mode)) {
        _receiving = true;
        Serial.printf("ES100 reception started (mode 0x%02X)\n", mode);
        return true;
    }
    
    Serial.println("Failed to start ES100 reception");
    return false;
}

void ES100::stopReception() {
    if (_initialized && isPoweredOn()) {
        // Write 0 to Control0 to stop reception
        writeRegister(ES100_REG_CONTROL0, 0x00);
    }
    _receiving = false;
    // Power off to free the I2C bus for other devices (touch, DS3231)
    powerOff();
}

bool ES100::isReceiving() {
    return _receiving;
}

// ============================================================================
// Status Reading
// ============================================================================
uint8_t ES100::readIRQStatus() {
    if (!isPoweredOn()) {
        return 0;
    }
    // Note: Reading IRQ status clears the IRQ- pin (drives it high)
    return readRegister(ES100_REG_IRQ_STATUS);
}

uint8_t ES100::readStatus0() {
    if (!isPoweredOn()) {
        return 0;
    }
    return readRegister(ES100_REG_STATUS0);
}

// ============================================================================
// Time Reading
// ============================================================================
bool ES100::readDateTime(ES100Time *time) {
    if (!isPoweredOn()) {
        Serial.println("ES100 not powered - cannot read time");
        return false;
    }
    
    if (time == nullptr) {
        return false;
    }
    
    // Read Status0 first to check RX_OK
    uint8_t status0 = readStatus0();
    if (!(status0 & ES100_STATUS_RX_OK)) {
        Serial.println("RX_OK not set - time data not valid");
        return false;
    }
    
    // Read all time registers in one burst (registers 0x04-0x09)
    uint8_t timeData[6];
    if (readRegisters(ES100_REG_YEAR, timeData, 6) != 6) {
        Serial.println("Failed to read time registers");
        return false;
    }
    
    // Convert BCD to decimal and populate structure
    // Year is 2-digit BCD, we need to add century
    uint8_t yearBcd = timeData[0];
    time->year = 2000 + bcdToDec(yearBcd);
    
    // Month (1-12)
    time->month = bcdToDec(timeData[1] & 0x1F);  // Mask upper bits
    
    // Day (1-31)
    time->day = bcdToDec(timeData[2] & 0x3F);  // Mask upper bits
    
    // Hour (0-23) - UTC
    time->hour = bcdToDec(timeData[3] & 0x3F);  // Mask upper bits
    
    // Minute (0-59)
    time->minute = bcdToDec(timeData[4] & 0x7F);  // Mask upper bit
    
    // Second (0-59)
    time->second = bcdToDec(timeData[5] & 0x7F);  // Mask upper bit
    
    // Extract DST status from Status0
    time->dstStatus = (status0 & ES100_STATUS_DST_MASK) >> 5;
    
    // Check which antenna was used
    time->antenna2Used = (status0 & ES100_STATUS_ANT) != 0;
    
    Serial.printf("ES100 read: %04d-%02d-%02d %02d:%02d:%02d UTC (Ant%d)\n",
                  time->year, time->month, time->day,
                  time->hour, time->minute, time->second,
                  time->antenna2Used ? 2 : 1);
    
    return true;
}

// ============================================================================
// Low-Level I2C Functions
// ============================================================================
uint8_t ES100::readRegister(uint8_t reg) {
    if (_wire == nullptr || !isPoweredOn()) {
        return 0xFF;
    }
    
    _wire->beginTransmission(ES100_I2C_ADDR);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) {  // Send repeated start
        Serial.printf("I2C error writing register address 0x%02X\n", reg);
        return 0xFF;
    }
    
    if (_wire->requestFrom((uint8_t)ES100_I2C_ADDR, (uint8_t)1) != 1) {
        Serial.printf("I2C error reading register 0x%02X\n", reg);
        return 0xFF;
    }
    
    return _wire->read();
}

bool ES100::writeRegister(uint8_t reg, uint8_t value) {
    if (_wire == nullptr || !isPoweredOn()) {
        return false;
    }
    
    _wire->beginTransmission(ES100_I2C_ADDR);
    _wire->write(reg);
    _wire->write(value);
    uint8_t result = _wire->endTransmission();
    
    if (result != 0) {
        Serial.printf("I2C error writing 0x%02X to register 0x%02X (error %d)\n", 
                      value, reg, result);
        return false;
    }
    
    return true;
}

uint8_t ES100::readRegisters(uint8_t startReg, uint8_t *buffer, uint8_t count) {
    if (_wire == nullptr || buffer == nullptr || !isPoweredOn()) {
        return 0;
    }
    
    _wire->beginTransmission(ES100_I2C_ADDR);
    _wire->write(startReg);
    if (_wire->endTransmission(false) != 0) {  // Send repeated start
        Serial.printf("I2C error writing start register 0x%02X\n", startReg);
        return 0;
    }
    
    uint8_t bytesRead = _wire->requestFrom((uint8_t)ES100_I2C_ADDR, count);
    
    for (uint8_t i = 0; i < bytesRead; i++) {
        buffer[i] = _wire->read();
    }
    
    return bytesRead;
}

// ============================================================================
// I2C Bus Recovery
// ============================================================================
void ES100::recoverBus() {
    if (_sdaPin < 0 || _sclPin < 0 || _wire == nullptr) {
        return;  // No pin info, skip recovery
    }

    // Check if SDA is stuck low
    // NOTE: Do NOT call pinMode() here — it disconnects the pin from the I2C
    // peripheral and breaks Wire. digitalRead() works on ESP32 even when the
    // pin is assigned to I2C (reads GPIO input register directly).
    if (digitalRead(_sdaPin) == HIGH) {
        return;  // Bus is fine, no recovery needed
    }

    Serial.println("[ES100] I2C bus stuck - recovering...");

    // Toggle SCL up to 9 times to clock out any stuck slave
    pinMode(_sclPin, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(_sclPin, LOW);
        delayMicroseconds(5);
        digitalWrite(_sclPin, HIGH);
        delayMicroseconds(5);

        // Check if SDA released
        if (digitalRead(_sdaPin) == HIGH) {
            Serial.printf("[ES100] Bus recovered after %d clock pulses\n", i + 1);
            break;
        }
    }

    // Generate STOP condition: SDA low→high while SCL high
    pinMode(_sdaPin, OUTPUT);
    digitalWrite(_sdaPin, LOW);
    delayMicroseconds(5);
    digitalWrite(_sclPin, HIGH);
    delayMicroseconds(5);
    digitalWrite(_sdaPin, HIGH);
    delayMicroseconds(5);

    // Re-initialize Wire to restore normal I2C operation
    _wire->begin(_sdaPin, _sclPin);

    Serial.println("[ES100] I2C bus recovery complete");
}

// ============================================================================
// BCD Conversion Utilities
// ============================================================================
uint8_t ES100::bcdToDec(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

uint8_t ES100::decToBcd(uint8_t dec) {
    return ((dec / 10) << 4) | (dec % 10);
}
