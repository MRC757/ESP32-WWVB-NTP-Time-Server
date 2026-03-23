/**
 * @file      ReceptionHistory.h
 * @brief     WWVB Reception History Tracker
 * @details   Tracks sync success/failure over 48 hours for charting
 */

#ifndef RECEPTIONHISTORY_H
#define RECEPTIONHISTORY_H

#include <Arduino.h>

// ============================================================================
// Configuration
// ============================================================================
#define HISTORY_HOURS       48      // Hours of history to track
#define HISTORY_BUCKETS     48      // One bucket per hour

// ============================================================================
// ReceptionHistory Class Definition
// ============================================================================

/**
 * @brief Reception History Tracker Class
 * @details Maintains a 48-hour rolling history of WWVB sync attempts
 *          Used for displaying reception quality chart
 */
class ReceptionHistory {
public:
    /**
     * @brief Constructor
     */
    ReceptionHistory();
    
    /**
     * @brief Initialize the history tracker
     */
    void begin();
    
    /**
     * @brief Record a sync attempt
     * @param success True if sync was successful
     */
    void recordAttempt(bool success);
    
    /**
     * @brief Called once per second to track time
     * @details Shifts buckets when a new hour begins
     */
    void hourlyTick();
    
    /**
     * @brief Get the history data array for charting
     * @param data Array to fill with history (48 elements)
     *             Index 0 = oldest (48h ago), Index 47 = newest (current hour)
     */
    void getHistoryData(uint8_t *data);
    
    /**
     * @brief Get pointer to hourly data array (for direct access)
     * @return Pointer to internal bucket array (48 elements)
     */
    const uint8_t* getHourlyData();
    
    /**
     * @brief Get the maximum value in history (for chart scaling)
     * @return Maximum sync count in any hour
     */
    uint8_t getMaxValue();
    
    /**
     * @brief Get the maximum hourly count (alias for getMaxValue)
     * @return Maximum sync count in any hour
     */
    uint8_t getMaxHourlyCount();
    
    /**
     * @brief Get total successful syncs in last 48 hours
     * @return Number of successful syncs
     */
    int getRecentSuccessCount();
    
    /**
     * @brief Get total successful syncs since power on
     * @return Total success count
     */
    int getTotalSuccessCount();
    
    /**
     * @brief Get total sync attempts since power on
     * @return Total attempt count
     */
    int getTotalAttemptCount();
    
    /**
     * @brief Get millis() timestamp of last successful sync
     * @return Timestamp (0 if never synced)
     */
    unsigned long getLastSuccessTime();
    
    /**
     * @brief Get millis() timestamp of last sync attempt
     * @return Timestamp (0 if never attempted)
     */
    unsigned long getLastAttemptTime();
    
    /**
     * @brief Get success rate as percentage (0-100)
     * @return Success rate percentage
     */
    uint8_t getSuccessRate();
    
    /**
     * @brief Reset all history data
     */
    void reset();

private:
    // History buckets - each represents successful syncs in that hour
    // Index 0 = 47-48 hours ago, Index 47 = current hour
    uint8_t _buckets[HISTORY_BUCKETS];
    
    // Current bucket index (always HISTORY_BUCKETS - 1 for newest)
    uint8_t _currentBucket;
    
    // Counters
    int _totalSuccess;          // Total successful syncs (all time)
    int _totalAttempts;         // Total sync attempts (all time)
    
    // Timestamps
    unsigned long _lastSuccessTime;     // millis() of last success
    unsigned long _lastAttemptTime;     // millis() of last attempt
    
    // Hour tracking
    unsigned long _lastHourMillis;      // millis() at last hour boundary
    uint32_t _secondsInCurrentHour;     // Seconds elapsed in current hour
    
    /**
     * @brief Shift all buckets left (age by one hour)
     */
    void shiftBuckets();
};

#endif // RECEPTIONHISTORY_H
