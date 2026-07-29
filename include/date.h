// Real-time clock reading, filled in by cmostime() (lapic.c) from the
// CMOS/RTC hardware.
struct rtcdate {
  uint second;
  uint minute;
  uint hour;
  uint day;
  uint month;
  uint year;
};
