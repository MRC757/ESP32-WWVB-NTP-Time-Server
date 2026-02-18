/**
 * @file      ES100.h
 * @brief     ES100 WWVB Receiver Driver for ESP32
 * @details   I2C driver for Everset ES100 WWVB atomic clock receiver
 * 
 * Based on ES100 Data Sheet Ver 0.97
 * I2C Address: 0x32
 * Device ID: 0x10
 */

#ifndef ES100_H
#define ES100_H

#include <Arduino.h>
#include <Wire.h>

// ============================================================================
// ES100 I2C Configuration
// ============================================================================
#define ES100_I2C_ADDR          0x32    // 7-bit I2C address
#define ES100_DEVICE_ID         0x10    // Expected device ID

// ============================================================================
// ES100 Register Addresses
// ============================================================================
#define ES100_REG_CONTROL0      0x00    // Control register 0 (R/W)
#define ES100_REG_CONTROL1      0x01    // Control register 1 (R/W) - unused
#define ES100_REG_IRQ_STATUS    0x02    // IRQ status register (R)
#define ES100_REG_STATUS0       0x03    // Status register 0 (R)
#define ES100_REG_YEAR          0x04    // Year (BCD)
#define ES100_REG_MONTH         0x05    // Month (BCD)
#define ES100_REG_DAY           0x06    // Day (BCD)
#define ES100_REG_HOUR          0x07    // Hour (BCD)
#define ES100_REG_MINUTE        0x08    // Minute (BCD)
#define ES100_REG_SECOND        0x09    // Second (BCD)
#define ES100_REG_NEXT_DST_MO   0x0A    // Next DST month
#define ES100_REG_NEXT_DST_DAY  0x0B    // Next DST day
#define ES100_REG_NEXT_DST_HR   0x0C    // Next DST hour
#define ES100_REG_DEVICE_ID     0x0D    // Device ID register

// ============================================================================
// Control 0 Register Bits
// ============================================================================
#define ES100_CTRL0_START       0x01    // Start reception
#define ES100_CTRL0_ANT1_OFF    0x02    // Disable Antenna 1
#define ES100_CTRL0_ANT2_OFF    0x04    // Disable Antenna 2
#define ES100_CTRL0_START_ANT   0x08    // Start with Antenna 2 (0=Ant1)
#define ES100_CTRL0_TRACKING    0x10    // Enable tracking mode

// Valid Control 0 values for starting reception
#define ES100_CTRL0_NORMAL      0x01    // Normal, start Ant1, toggle
#define ES100_CTRL0_ANT2_ONLY   0x03    // Antenna 2 only
#define ES100_CTRL0_ANT1_ONLY   0x05    // Antenna 1 only
#define ES100_CTRL0_NORMAL_ANT2 0x09    // Normal, start Ant2, toggle

// ============================================================================
// IRQ Status Register Bits
// ============================================================================
#define ES100_IRQ_RX_COMPLETE   0x01    // Reception complete (success)
#define ES100_IRQ_CYCLE_COMPLETE 0x04   // Cycle complete (failed, will retry)

// ============================================================================
// Status 0 Register Bits
// ============================================================================
#define ES100_STATUS_RX_OK      0x01    // Successful reception
#define ES100_STATUS_ANT        0x02    // Antenna used (1=Ant2, 0=Ant1)
#define ES100_STATUS_LSW_MASK   0x18    // Leap second warning bits
#define ES100_STATUS_DST_MASK   0x60    // DST status bits
#define ES100_STATUS_TRACKING   0x80    // Tracking mode was used

// DST Status values (bits 5-6)
#define ES100_DST_NOT_IN_EFFECT 0x00    // DST not in effect
#define ES100_DST_ENDS_TODAY    0x20    // DST ends today
#define ES100_DST_BEGINS_TODAY  0x40    // DST begins today
#define ES100_DST_IN_EFFECT     0x60    // DST is in effect

// ============================================================================
// Timing Constants
// ============================================================================
#define ES100_WAKEUP_TIME_MS    20      // Time for ES100 to wake up after EN high
#define ES100_MAX_RECEPTION_MS  134000  // Max time for single reception attempt

// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief Structure to hold received time data
 */
struct ES100Time {
    uint16_t year;      // Full year (e.g., 2025)
    uint8_t month;      // Month (1-12)
    uint8_t day;        // Day of month (1-31)
    uint8_t hour;       // Hour (0-23) UTC
    uint8_t minute;     // Minute (0-59)
    uint8_t second;     // Second (0-59)
    uint8_t dstStatus;  // DST status from Status0
    bool antenna2Used;  // True if Antenna 2 was used
};

// ============================================================================
// ES100 Class Definition
// ============================================================================

/**
 * @brief ES100 WWVB Receiver Driver Class
 */
class ES100 {
public:
    /**
     * @brief Constructor
     * @param enPin GPIO pin connected to ES100 EN (enable)
     * @param irqPin GPIO pin connected to ES100 IRQ- (interrupt)
     */
    ES100(uint8_t enPin, uint8_t irqPin);
    
    /**
     * @brief Initialize the ES100 receiver
     * @param wirePort Pointer to TwoWire instance (default Wire)
     * @param sdaPin I2C SDA pin (needed for bus recovery)
     * @param sclPin I2C SCL pin (needed for bus recovery)
     * @return true if initialization successful
     */
    bool begin(TwoWire *wirePort = &Wire, int sdaPin = -1, int sclPin = -1);
    
    /**
     * @brief Check if ES100 is powered on
     * @return true if EN pin is HIGH
     */
    bool isPoweredOn();
    
    /**
     * @brief Power on the ES100 (set EN high)
     */
    void powerOn();
    
    /**
     * @brief Power off the ES100 (set EN low)
     */
    void powerOff();
    
    /**
     * @brief Read the device ID register
     * @return Device ID (should be 0x10 for ES100)
     */
    uint8_t readDeviceID();
    
    /**
     * @brief Start WWVB reception
     * @param mode Control0 value (default: normal with antenna toggle)
     * @return true if reception started successfully
     */
    bool startReception(uint8_t mode = ES100_CTRL0_NORMAL);
    
    /**
     * @brief Stop WWVB reception
     */
    void stopReception();
    
    /**
     * @brief Check if reception is in progress
     * @return true if currently receiving
     */
    bool isReceiving();
    
    /**
     * @brief Read the IRQ status register
     * @note Reading this register clears the IRQ- pin
     * @return IRQ status byte
     */
    uint8_t readIRQStatus();
    
    /**
     * @brief Read the Status 0 register
     * @return Status 0 byte
     */
    uint8_t readStatus0();
    
    /**
     * @brief Read the received date and time
     * @param time Pointer to ES100Time structure to fill
     * @return true if time was read successfully
     */
    bool readDateTime(ES100Time *time);
    
    /**
     * @brief Read a single register
     * @param reg Register address
     * @return Register value
     */
    uint8_t readRegister(uint8_t reg);
    
    /**
     * @brief Write a single register
     * @param reg Register address
     * @param value Value to write
     * @return true if write successful
     */
    bool writeRegister(uint8_t reg, uint8_t value);
    
    /**
     * @brief Read multiple consecutive registers
     * @param startReg Starting register address
     * @param buffer Buffer to store read data
     * @param count Number of bytes to read
     * @return Number of bytes actually read
     */
    uint8_t readRegisters(uint8_t startReg, uint8_t *buffer, uint8_t count);

private:
    TwoWire *_wire;         // I2C interface
    uint8_t _enPin;         // Enable pin
    uint8_t _irqPin;        // IRQ pin
    int _sdaPin;            // I2C SDA pin (for bus recovery)
    int _sclPin;            // I2C SCL pin (for bus recovery)
    bool _receiving;        // Reception in progress flag
    bool _initialized;      // Initialization status

    /**
     * @brief Recover I2C bus from stuck state
     * @details Toggles SCL 9 times to release any slave holding SDA low,
     *          then re-initializes Wire. Call after ES100 power transitions.
     */
    void recoverBus();
    
    /**
     * @brief Convert BCD byte to decimal
     * @param bcd BCD encoded value
     * @return Decimal value
     */
    uint8_t bcdToDec(uint8_t bcd);
    
    /**
     * @brief Convert decimal to BCD
     * @param dec Decimal value
     * @return BCD encoded value
     */
    uint8_t decToBcd(uint8_t dec);
};

#endif // ES100_H
