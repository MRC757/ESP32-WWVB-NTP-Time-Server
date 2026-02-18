/**
 * @file      TimeManager.h
 * @brief     Internal Time Management for WWVB Clock
 * @details   Maintains time between WWVB syncs using ESP32 millis()
 */

#ifndef TIMEMANAGER_H
#define TIMEMANAGER_H

#include <Arduino.h>

// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief Structure to hold date and time
 */
struct ClockTime {
    uint16_t year;      // Full year (e.g., 2025)
    uint8_t month;      // Month (1-12)
    uint8_t day;        // Day of month (1-31)
    uint8_t hour;       // Hour (0-23)
    uint8_t minute;     // Minute (0-59)
    uint8_t second;     // Second (0-59)
};

// ============================================================================
// TimeManager Class Definition
// ============================================================================

/**
 * @brief Time Manager Class
 * @details Maintains internal time using ESP32 millis() timer
 *          Time is stored as UTC and converted to local time on request
 */
class TimeManager {
public:
    /**
     * @brief Constructor
     */
    TimeManager();
    
    /**
     * @brief Set the current UTC time
     * @param year Full year (e.g., 2025)
     * @param month Month (1-12)
     * @param day Day of month (1-31)
     * @param hour Hour (0-23)
     * @param minute Minute (0-59)
     * @param second Second (0-59)
     */
    void setTime(uint16_t year, uint8_t month, uint8_t day,
                 uint8_t hour, uint8_t minute, uint8_t second);
    
    /**
     * @brief Update internal time (call once per second)
     * @details Increments internal second counter based on millis()
     */
    void tick();
    
    /**
     * @brief Get current UTC time
     * @return ClockTime structure with UTC time
     */
    ClockTime getUTCTime();
    
    /**
     * @brief Get current local time with UTC offset and DST
     * @param utcOffset Hours offset from UTC (e.g., -5 for EST)
     * @param dst True if Daylight Saving Time is active
     * @return ClockTime structure with local time
     */
    ClockTime getLocalTime(int8_t utcOffset, bool dst = false);
    
    /**
     * @brief Get Unix timestamp (seconds since Jan 1, 1970)
     * @return Unix timestamp
     */
    uint32_t getUnixTime();

    /**
     * @brief Get current milliseconds within the second (0-999)
     * @details Computed on-the-fly from accumulated millis, no side effects
     * @return Milliseconds (0-999)
     */
    uint16_t getMilliseconds();
    
    /**
     * @brief Set time from Unix timestamp
     * @param unixTime Seconds since Jan 1, 1970
     */
    void setUnixTime(uint32_t unixTime);
    
    /**
     * @brief Check if time has been set (synced at least once)
     * @return true if time has been set
     */
    bool isTimeSet();
    
    /**
     * @brief Get seconds since last time set
     * @return Seconds elapsed since setTime() was called
     */
    uint32_t getSecondsSinceSync();
    
    /**
     * @brief Calculate day of week (0=Sunday, 6=Saturday)
     * @param year Full year
     * @param month Month (1-12)
     * @param day Day of month (1-31)
     * @return Day of week (0-6)
     */
    static int calculateDayOfWeek(uint16_t year, uint8_t month, uint8_t day);
    
    /**
     * @brief Check if year is a leap year
     * @param year Full year
     * @return true if leap year
     */
    static bool isLeapYear(uint16_t year);
    
    /**
     * @brief Get number of days in a month
     * @param year Full year (needed for February leap year check)
     * @param month Month (1-12)
     * @return Number of days in the month
     */
    static uint8_t daysInMonth(uint16_t year, uint8_t month);

private:
    // Internal time storage (UTC)
    uint16_t _year;
    uint8_t _month;
    uint8_t _day;
    uint8_t _hour;
    uint8_t _minute;
    uint8_t _second;
    
    // Timing tracking
    unsigned long _lastTickMillis;      // millis() at last tick
    unsigned long _syncMillis;          // millis() when time was set
    bool _timeSet;                      // Has time been set?
    
    // Accumulated milliseconds for sub-second accuracy
    uint16_t _accumMillis;
    
    /**
     * @brief Increment internal time by one second
     */
    void incrementSecond();
    
    /**
     * @brief Normalize time values after arithmetic operations
     */
    void normalizeTime();
    
    /**
     * @brief Add hours to current time (can be negative)
     * @param hours Hours to add (positive or negative)
     */
    void addHours(int8_t hours);
};

#endif // TIMEMANAGER_H
