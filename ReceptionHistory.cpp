/**
 * @file      ReceptionHistory.cpp
 * @brief     WWVB Reception History Tracker Implementation
 */

#include "ReceptionHistory.h"

// ============================================================================
// Constructor
// ============================================================================
ReceptionHistory::ReceptionHistory() 
    : _currentBucket(HISTORY_BUCKETS - 1),
      _totalSuccess(0), _totalAttempts(0),
      _lastSuccessTime(0), _lastAttemptTime(0),
      _lastHourMillis(0), _secondsInCurrentHour(0) {
    
    // Initialize all buckets to 0
    memset(_buckets, 0, sizeof(_buckets));
}

// ============================================================================
// Initialization
// ============================================================================
void ReceptionHistory::begin() {
    reset();
    _lastHourMillis = millis();
    Serial.println("ReceptionHistory initialized");
}

void ReceptionHistory::reset() {
    memset(_buckets, 0, sizeof(_buckets));
    _currentBucket = HISTORY_BUCKETS - 1;
    _totalSuccess = 0;
    _totalAttempts = 0;
    _lastSuccessTime = 0;
    _lastAttemptTime = 0;
    _secondsInCurrentHour = 0;
    _lastHourMillis = millis();
}

// ============================================================================
// Recording
// ============================================================================
void ReceptionHistory::recordAttempt(bool success) {
    _totalAttempts++;
    _lastAttemptTime = millis();
    
    if (success) {
        _totalSuccess++;
        _lastSuccessTime = millis();
        
        // Increment current hour's bucket (cap at 255)
        if (_buckets[_currentBucket] < 255) {
            _buckets[_currentBucket]++;
        }
        
        Serial.printf("ReceptionHistory: Success recorded (bucket %d now = %d)\n", 
                      _currentBucket, _buckets[_currentBucket]);
    } else {
        Serial.println("ReceptionHistory: Failed attempt recorded");
    }
}

// ============================================================================
// Time Tracking
// ============================================================================
void ReceptionHistory::hourlyTick() {
    // This should be called approximately once per second
    _secondsInCurrentHour++;
    
    // Check if an hour has passed (3600 seconds)
    if (_secondsInCurrentHour >= 3600) {
        _secondsInCurrentHour = 0;
        shiftBuckets();
        Serial.println("ReceptionHistory: Hour elapsed, buckets shifted");
    }
}

void ReceptionHistory::shiftBuckets() {
    // Shift all buckets left (discard oldest)
    for (int i = 0; i < HISTORY_BUCKETS - 1; i++) {
        _buckets[i] = _buckets[i + 1];
    }
    
    // Clear the newest bucket
    _buckets[HISTORY_BUCKETS - 1] = 0;
    
    // _currentBucket stays at HISTORY_BUCKETS - 1 (always the newest)
}

// ============================================================================
// Data Retrieval
// ============================================================================
void ReceptionHistory::getHistoryData(uint8_t *data) {
    if (data == nullptr) return;
    
    // Copy buckets to output array
    // Index 0 = oldest (48h ago), Index 47 = current hour
    memcpy(data, _buckets, HISTORY_BUCKETS);
}

const uint8_t* ReceptionHistory::getHourlyData() {
    // Return pointer to internal bucket array for direct access
    return _buckets;
}

uint8_t ReceptionHistory::getMaxValue() {
    uint8_t maxVal = 0;
    
    for (int i = 0; i < HISTORY_BUCKETS; i++) {
        if (_buckets[i] > maxVal) {
            maxVal = _buckets[i];
        }
    }
    
    // Return at least 1 to avoid division by zero in charts
    return (maxVal > 0) ? maxVal : 1;
}

uint8_t ReceptionHistory::getMaxHourlyCount() {
    // Alias for getMaxValue()
    return getMaxValue();
}

int ReceptionHistory::getRecentSuccessCount() {
    int count = 0;
    
    for (int i = 0; i < HISTORY_BUCKETS; i++) {
        count += _buckets[i];
    }
    
    return count;
}

int ReceptionHistory::getTotalSuccessCount() {
    return _totalSuccess;
}

int ReceptionHistory::getTotalAttemptCount() {
    return _totalAttempts;
}

unsigned long ReceptionHistory::getLastSuccessTime() {
    return _lastSuccessTime;
}

unsigned long ReceptionHistory::getLastAttemptTime() {
    return _lastAttemptTime;
}

uint8_t ReceptionHistory::getSuccessRate() {
    if (_totalAttempts == 0) return 0;
    
    return (uint8_t)((_totalSuccess * 100) / _totalAttempts);
}
