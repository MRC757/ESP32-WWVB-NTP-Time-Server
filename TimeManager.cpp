/**
 * @file      TimeManager.cpp
 * @brief     Internal Time Management Implementation
 */

#include "TimeManager.h"

// Days in each month (non-leap year)
static const uint8_t DAYS_IN_MONTH[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

// ============================================================================
// Constructor
// ============================================================================
TimeManager::TimeManager() 
    : _year(2000), _month(1), _day(1), 
      _hour(0), _minute(0), _second(0),
      _lastTickMillis(0), _syncMillis(0), 
      _timeSet(false), _accumMillis(0) {
}

// ============================================================================
// Time Setting
// ============================================================================
void TimeManager::setTime(uint16_t year, uint8_t month, uint8_t day,
                          uint8_t hour, uint8_t minute, uint8_t second) {
    _year = year;
    _month = month;
    _day = day;
    _hour = hour;
    _minute = minute;
    _second = second;
    
    _lastTickMillis = millis();
    _syncMillis = _lastTickMillis;
    _accumMillis = 0;
    _timeSet = true;
    
    Serial.printf("TimeManager: Time set to %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  _year, _month, _day, _hour, _minute, _second);
}

void TimeManager::setUnixTime(uint32_t unixTime) {
    // Convert Unix timestamp to date/time
    // This is a simplified conversion - accurate for dates from 2000-2099
    
    uint32_t seconds = unixTime;
    uint16_t year = 1970;
    
    // Count years
    while (true) {
        uint32_t secondsInYear = isLeapYear(year) ? 366UL * 86400UL : 365UL * 86400UL;
        if (seconds < secondsInYear) break;
        seconds -= secondsInYear;
        year++;
    }
    
    // Count months
    uint8_t month = 1;
    while (month <= 12) {
        uint32_t secondsInMonth = (uint32_t)daysInMonth(year, month) * 86400UL;
        if (seconds < secondsInMonth) break;
        seconds -= secondsInMonth;
        month++;
    }
    
    // Count days
    uint8_t day = 1 + (seconds / 86400);
    seconds %= 86400;
    
    // Hours, minutes, seconds
    uint8_t hour = seconds / 3600;
    seconds %= 3600;
    uint8_t minute = seconds / 60;
    uint8_t second = seconds % 60;
    
    setTime(year, month, day, hour, minute, second);
}

// ============================================================================
// Time Retrieval
// ============================================================================
ClockTime TimeManager::getUTCTime() {
    ClockTime dt;
    dt.year = _year;
    dt.month = _month;
    dt.day = _day;
    dt.hour = _hour;
    dt.minute = _minute;
    dt.second = _second;
    return dt;
}

ClockTime TimeManager::getLocalTime(int8_t utcOffset, bool dst) {
    // Create a copy to work with
    ClockTime dt = getUTCTime();
    
    // Calculate total offset
    int8_t totalOffset = utcOffset;
    if (dst) {
        totalOffset += 1;  // DST adds 1 hour
    }
    
    // Add the offset
    int16_t newHour = (int16_t)dt.hour + totalOffset;
    
    // Handle day rollover
    while (newHour < 0) {
        newHour += 24;
        // Go back one day
        if (dt.day > 1) {
            dt.day--;
        } else {
            // Go back one month
            if (dt.month > 1) {
                dt.month--;
            } else {
                dt.month = 12;
                dt.year--;
            }
            dt.day = daysInMonth(dt.year, dt.month);
        }
    }
    
    while (newHour >= 24) {
        newHour -= 24;
        // Go forward one day
        if (dt.day < daysInMonth(dt.year, dt.month)) {
            dt.day++;
        } else {
            dt.day = 1;
            if (dt.month < 12) {
                dt.month++;
            } else {
                dt.month = 1;
                dt.year++;
            }
        }
    }
    
    dt.hour = (uint8_t)newHour;
    
    return dt;
}

uint32_t TimeManager::getUnixTime() {
    uint32_t unixTime = 0;
    
    // Add years since 1970
    for (uint16_t y = 1970; y < _year; y++) {
        unixTime += isLeapYear(y) ? 366UL * 86400UL : 365UL * 86400UL;
    }
    
    // Add months of current year
    for (uint8_t m = 1; m < _month; m++) {
        unixTime += (uint32_t)daysInMonth(_year, m) * 86400UL;
    }
    
    // Add days, hours, minutes, seconds
    unixTime += (uint32_t)(_day - 1) * 86400UL;
    unixTime += (uint32_t)_hour * 3600UL;
    unixTime += (uint32_t)_minute * 60UL;
    unixTime += _second;
    
    return unixTime;
}

uint16_t TimeManager::getMilliseconds() {
    if (!_timeSet) return 0;
    unsigned long elapsed = millis() - _lastTickMillis;
    return (uint16_t)((_accumMillis + elapsed) % 1000);
}

bool TimeManager::isTimeSet() {
    return _timeSet;
}

uint32_t TimeManager::getSecondsSinceSync() {
    if (!_timeSet) return 0;
    return (millis() - _syncMillis) / 1000;
}

// ============================================================================
// Time Update (call once per second from main loop)
// ============================================================================
void TimeManager::tick() {
    if (!_timeSet) return;
    
    unsigned long currentMillis = millis();
    unsigned long elapsed = currentMillis - _lastTickMillis;
    
    // Accumulate milliseconds
    _accumMillis += elapsed;
    _lastTickMillis = currentMillis;
    
    // Increment seconds for each 1000ms accumulated
    while (_accumMillis >= 1000) {
        _accumMillis -= 1000;
        incrementSecond();
    }
}

void TimeManager::incrementSecond() {
    _second++;
    
    if (_second >= 60) {
        _second = 0;
        _minute++;
        
        if (_minute >= 60) {
            _minute = 0;
            _hour++;
            
            if (_hour >= 24) {
                _hour = 0;
                _day++;
                
                if (_day > daysInMonth(_year, _month)) {
                    _day = 1;
                    _month++;
                    
                    if (_month > 12) {
                        _month = 1;
                        _year++;
                    }
                }
            }
        }
    }
}

// ============================================================================
// Static Utility Functions
// ============================================================================
int TimeManager::calculateDayOfWeek(uint16_t year, uint8_t month, uint8_t day) {
    // Zeller's congruence for Gregorian calendar
    // Returns 0=Sunday, 1=Monday, ..., 6=Saturday
    
    if (month < 3) {
        month += 12;
        year--;
    }
    
    int q = day;
    int m = month;
    int k = year % 100;
    int j = year / 100;
    
    int h = (q + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 - 2 * j) % 7;
    
    // Convert from Zeller's (0=Saturday) to standard (0=Sunday)
    int dow = ((h + 6) % 7);
    
    return dow;
}

bool TimeManager::isLeapYear(uint16_t year) {
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

uint8_t TimeManager::daysInMonth(uint16_t year, uint8_t month) {
    if (month < 1 || month > 12) return 30;  // Invalid month, return safe default
    
    uint8_t days = DAYS_IN_MONTH[month - 1];
    
    // February in leap year
    if (month == 2 && isLeapYear(year)) {
        days = 29;
    }
    
    return days;
}

// ============================================================================
// Private Helper Functions
// ============================================================================
void TimeManager::normalizeTime() {
    // Ensure all time values are within valid ranges
    while (_second >= 60) {
        _second -= 60;
        _minute++;
    }
    
    while (_minute >= 60) {
        _minute -= 60;
        _hour++;
    }
    
    while (_hour >= 24) {
        _hour -= 24;
        _day++;
    }
    
    while (_day > daysInMonth(_year, _month)) {
        _day -= daysInMonth(_year, _month);
        _month++;
        if (_month > 12) {
            _month = 1;
            _year++;
        }
    }
}

void TimeManager::addHours(int8_t hours) {
    int16_t newHour = (int16_t)_hour + hours;
    
    while (newHour < 0) {
        newHour += 24;
        // Go back one day
        if (_day > 1) {
            _day--;
        } else {
            // Go back one month
            if (_month > 1) {
                _month--;
            } else {
                _month = 12;
                _year--;
            }
            _day = daysInMonth(_year, _month);
        }
    }
    
    while (newHour >= 24) {
        newHour -= 24;
        // Go forward one day
        if (_day < daysInMonth(_year, _month)) {
            _day++;
        } else {
            _day = 1;
            if (_month < 12) {
                _month++;
            } else {
                _month = 1;
                _year++;
            }
        }
    }
    
    _hour = (uint8_t)newHour;
}
