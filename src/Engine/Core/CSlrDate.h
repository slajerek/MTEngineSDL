/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _CSLRDATE_
#define _CSLRDATE_

#include "SYS_Defs.h"

class CSlrDate
{
public:
	u8 hour;
	u8 minute;
	u8 second;
	u16 milliseconds;

	u8 day;
	u8 month;
	i16 year;
	CSlrDate();
	CSlrDate(u8 day, u8 month, i16 year) { this->day = day; this->month = month; this->year = year; };
	CSlrDate(u8 day, u8 month, i16 year, u8 second, u8 minute, u8 hour) { this->day = day; this->month = month; this->year = year; this->second = second; this->minute = minute; this->hour = hour; };

	void RefreshFromCurrentSystemTime();
	
	void IncreaseSecond();
	void DecreaseSecond();
	void IncreaseMinute();
	void DecreaseMinute();
	void IncreaseHour();
	void DecreaseHour();

	u8 NumDaysInMonth(u8 m);
	void IncreaseDay();
	void DecreaseDay();

	//void IncreaseDay();
	//void DecreaseDay();

	void DateToString(char *buf);
	void TimeToString(char *buf);
	void DateTimeToString(char *buf);
	void DateTimeToFileNameString(char *buf);
};

#endif

