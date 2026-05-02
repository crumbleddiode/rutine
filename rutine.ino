/*
Uncomment the following line to enable debug
Раскомментируйте строку ниже чтобы включить режим отладки
*/
#define ENABLE_SERIAL

#define UPDATE_DELAY 10
#define CHANGE_TIME 250  // in multiples of UPDATE_DELAY

#include <EEPROM.h>
#include <DS3231.h>
#include <TM1637Display.h>
#include <Wire.h>
#include <AmperkaFET.h>

#define DISPLAY_CLK A1
#define DISPLAY_DIO 2
#define RELAY_PIN A0

#define DEFAULT_BRIGHTNESS 0x0f
#define COLON_DOTS 0b01000000

#define DOW_LEDS_SERCLR_PIN A2
#define DOW_LEDS_CS_PIN A3

#define MONDAY 0b01000000
#define TUESDAY 0b00100000
#define WEDNESDAY 0b00010000
#define THURSDAY 0b00001000
#define FRIDAY 0b00000100
#define SATURDAY 0b00000010
#define SUNDAY 0b00000001

#define WORK_DAYS MONDAY | TUESDAY | WEDNESDAY | THURSDAY | FRIDAY
#define WEEKENDS SATURDAY | SUNDAY
#define ALL_DAYS 0b01111111

typedef unsigned int uint;
const char days_of_week[7] = {
  MONDAY,
  TUESDAY,
  WEDNESDAY,
  THURSDAY,
  FRIDAY,
  SATURDAY,
  SUNDAY
};
struct Schedule {
  bool is_active : 1;  // if this schedule is active
  bool mode : 1;       // where 1 means turns on light and 0 turns off light
  unsigned int total_minutes : 11;
  uint8_t days_of_week : 7;  // bitmask

  int hours() {
    return total_minutes / 60;
  }
  int minutes() {
    return total_minutes % 60;
  }
};

const uint8_t abcd[4] = {
  SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,
  SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
  SEG_A | SEG_D | SEG_E | SEG_F,
  SEG_B | SEG_D | SEG_C | SEG_D | SEG_E | SEG_G
};

const uint8_t erro[4] = {
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
  SEG_A | SEG_E | SEG_F,
  SEG_A | SEG_E | SEG_F,
  SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F
};

const int max_schedules = 1024 / (sizeof(Schedule));  // 1024 bytes of eeprom by sizeof schedule

bool last_schedule_available = false;
Schedule last_schedule;
bool next_schedule_available = false;
Schedule next_schedule;

Schedule load_schedule(int idx) {
  Schedule loaded;
  EEPROM.get(sizeof(Schedule) * idx, loaded);
  return loaded;
}

void save_schedule(Schedule &schedule, int idx) {
  EEPROM.put(sizeof(Schedule) * idx, schedule);
}
#ifdef ENABLE_SERIAL
void print_schedule(Schedule &sch) {
  Serial.print("Is active: ");
  Serial.println(sch.is_active);
  Serial.print("Mode: ");
  Serial.println(sch.mode ? "ON" : "OFF");
  Serial.print("Days of week: ");
  Serial.println(sch.days_of_week, BIN);
  Serial.print("Time: ");
  Serial.print(sch.hours());
  Serial.print(':');
  Serial.println(sch.minutes());
}
#endif

int closest_future_dow(uint8_t bitmask, uint8_t days_bitmask) {
  for (int i = 0; i < 7; i++) {
    if ((days_bitmask & bitmask) != 0) {
      return i;
    }
    days_bitmask >>= 1;
    if (days_bitmask == 0) {
      days_bitmask = MONDAY;
    }
  }
  return -1;
}

int closest_past_dow(uint8_t bitmask, uint8_t days_bitmask) {
  for (int i = 0; i < 7; i++) {
    if ((days_bitmask & bitmask) != 0) {
      return i;
    }
    days_bitmask <<= 1;
    if (days_bitmask == 0b10000000) {
      days_bitmask = SUNDAY;
    }
  }
  return -1;
}
int minutes_ago_it_happened(Schedule &schedule, int current_minutes, uint8_t current_bitmask) {
  uint8_t bitmask = schedule.days_of_week;
  if (current_bitmask & bitmask) {
    if (current_minutes == schedule.total_minutes) {
      return 0;
    }
    if (current_minutes < schedule.total_minutes) {
      int dist = closest_past_dow(bitmask & ~current_bitmask, current_bitmask);
      if (dist == -1) {
        return -1;
      }
      return (dist - 1) * 24 * 60 + (current_minutes + 24 * 60 - schedule.total_minutes);
    } else {
      return current_minutes - schedule.total_minutes;
    }
  } else {
    int dist = closest_past_dow(bitmask, current_bitmask);
    if (dist == -1) {
      return -1;
    }
    return (dist - 1) * 24 * 60 + (current_minutes + 24 * 60 - schedule.total_minutes);
  }
}

int minutes_it_will_happen_in(Schedule &schedule, int current_minutes, uint8_t current_bitmask) {
  uint8_t bitmask = schedule.days_of_week;
  if (current_bitmask & bitmask) {
    if (current_minutes > schedule.total_minutes) {
      int dist = closest_future_dow(bitmask & ~current_bitmask, current_bitmask);
      if (dist == -1) {
        return -1;
      }
      return (dist - 1) * 24 * 60 + (schedule.total_minutes + 24 * 60 - current_minutes);
    } else {
      return schedule.total_minutes - current_minutes;
    }
  } else {
    int dist = closest_future_dow(bitmask, current_bitmask);
    if (dist == -1) {
      return -1;
    }
    return (dist - 1) * 24 * 60 + (schedule.total_minutes + 24 * 60 - current_minutes);
  }
}
void find_last_next_schedules(uint8_t hour, uint8_t minute, uint8_t day_of_week) {
  uint8_t days_bitmask = days_of_week[day_of_week - 1];
  int best_past_distance = -1;
  int best_future_distance = -1;
  int current_total_minutes = (int)hour * 60 + minute;
  last_schedule_available = false;
  next_schedule_available = false;
  int current_minutes = hour * 60 + minute;
  for (int i = 0; i < max_schedules; i++) {
    Schedule schedule = load_schedule(i);
    if (!schedule.is_active) continue;
    int this_future_distance = minutes_it_will_happen_in(schedule, current_minutes, days_bitmask);
    int this_past_distance = minutes_ago_it_happened(schedule, current_minutes, days_bitmask);
#ifdef ENABLE_SERIAL
    Serial.print("Schedule ");
    Serial.println(i);
    print_schedule(schedule);
    Serial.print("Future distance:");
    Serial.println(this_future_distance);
    Serial.print("Past distance:");
    Serial.println(this_past_distance);
#endif

    if (last_schedule_available) {
      if (this_past_distance < best_past_distance) {
        last_schedule = schedule;
        best_past_distance = this_past_distance;
      }
    } else {
      last_schedule_available = true;
      last_schedule = schedule;
      best_past_distance = this_past_distance;
    }
    if (this_future_distance == 0)
      continue;
    if (next_schedule_available) {
      if (this_future_distance < best_future_distance) {
        next_schedule = schedule;
        best_future_distance = this_future_distance;
      }
    } else {
      next_schedule_available = true;
      next_schedule = schedule;
      best_future_distance = this_future_distance;
    }
  }
}

TM1637Display number_display(DISPLAY_CLK, DISPLAY_DIO);

RTClib rtc;
DS3231 Clock;
FET dow_display(DOW_LEDS_CS_PIN);

void dow_led_clear() {
  digitalWrite(DOW_LEDS_SERCLR_PIN, LOW);
  delay(10);
  digitalWrite(DOW_LEDS_SERCLR_PIN, HIGH);
}
void dow_led_light_bitmask(uint8_t bitmask) {
  for (int i = 0; i < 7; i++) {
    dow_display.digitalWrite(i, (MONDAY >> i) & bitmask ? HIGH : LOW);
  }
}

bool relay_state = false;
void setRelayState(bool to) {
  relay_state = to;
  digitalWrite(RELAY_PIN, !to);
}
#define FIRST_KEYPAD_OUTPUT_PIN 7
#define FIRST_KEYPAD_INPUT_PIN 3
#define KEYPAD_WIDTH 4
#define KEYPAD_HEIGHT 4

void setup() {
#ifdef ENABLE_SERIAL
  Serial.begin(9600);
#endif
  Wire.begin();
  dow_display.begin();
  dow_led_clear();
  digitalWrite(DOW_LEDS_SERCLR_PIN, HIGH);
  digitalWrite(DOW_LEDS_SERCLR_PIN, HIGH);
  number_display.setBrightness(DEFAULT_BRIGHTNESS);
  pinMode(RELAY_PIN, OUTPUT);
  Clock.setClockMode(false);
  setRelayState(false);
  for (int i = FIRST_KEYPAD_OUTPUT_PIN; i < FIRST_KEYPAD_OUTPUT_PIN + KEYPAD_HEIGHT; i++) {
    pinMode(i, OUTPUT);
    digitalWrite(i, HIGH);
  }
  for (int i = FIRST_KEYPAD_INPUT_PIN; i < FIRST_KEYPAD_INPUT_PIN + KEYPAD_WIDTH; i++)
    pinMode(i, INPUT_PULLUP);

  DateTime now = rtc.now();
  find_last_next_schedules(now.hour(), now.minute(), now.dayOfTheWeek());
#ifdef ENABLE_SERIAL
  Serial.print("Schedules available: ");
  Serial.println(max_schedules);
  if (last_schedule_available) {
    Serial.println("Last schedule:");
    print_schedule(last_schedule);
  } else {
    Serial.println("No last schedule");
  }

  if (next_schedule_available) {
    Serial.println("Next schedule:");
    print_schedule(next_schedule);
  } else {
    Serial.println("No next schedule");
  }
#endif
  if (last_schedule_available) {
    setRelayState(last_schedule.mode);
  }
}


void show_time(uint8_t hour, uint8_t minute) {
  // number_display.showNumberDecEx((int)hour * 100 + minute, COLON_DOTS, true);
  number_display.showNumberDecEx(hour, COLON_DOTS, true, 2, 0);
  number_display.showNumberDecEx(minute, COLON_DOTS, true, 2, 2);
}

void show_date(uint8_t day, uint8_t month) {
  number_display.showNumberDecEx(day, COLON_DOTS, false, 2, 0);
  number_display.showNumberDecEx(month, COLON_DOTS, false, 2, 2);
}


const char letter_c = SEG_A | SEG_D | SEG_E | SEG_F;
void show_temperature(float temp) {
  int tempi = (int)roundf(temp * 10);
  number_display.showNumberDecEx(tempi, COLON_DOTS, false, 3);
  number_display.setSegments(&letter_c, 1, 3);
}

void idle_status_show() {
}

const char keypad_keys[16] = {
  '1', '2', '3', 'A',
  '4', '5', '6', 'B',
  '7', '8', '9', 'C',
  '*', '0', '#', 'D'
};

/*
  O
  11 1 2 3 A
  10 4 5 6 B
  9  7 8 9 C
  8  * 0 # D
     7 6 5 4 I
*/
char get_input() {
  int row = 0;
  int column = 0;
  for (int x = FIRST_KEYPAD_OUTPUT_PIN + KEYPAD_HEIGHT - 1; x >= FIRST_KEYPAD_OUTPUT_PIN; x--) {
    digitalWrite(x, LOW);
    column = 0;
    for (int y = FIRST_KEYPAD_INPUT_PIN + KEYPAD_WIDTH - 1; y >= FIRST_KEYPAD_INPUT_PIN; y--) {
      delay(5);
      if (digitalRead(y) == LOW) {
        digitalWrite(x, HIGH);
        return keypad_keys[row * KEYPAD_HEIGHT + column];
      }
      column++;
    }
    digitalWrite(x, HIGH);
    row++;
  }
  return 0;
}

char get_just_pressed() {
  static bool requires_release = false;
  static char just_pressed = 0;
  char sym = get_input();
  if (requires_release) {
    just_pressed = 0;
    if (sym == 0) {
      requires_release = false;
    }
  } else {
    just_pressed = sym;
    if (sym != 0) {
      requires_release = true;
    }
  }
  return just_pressed;
}

char await_first_press() {
  char just_pressed;
  for (;;) {
    just_pressed = get_just_pressed();
    if (just_pressed != 0) {
      return just_pressed;
    }
    delay(UPDATE_DELAY);
  }
}

#define CANCEL_BUTTON 'A'
#define ACCEPT_BUTTON 'B'
#define BACKSPACE_BUTTON '#'

// 'A' cancels input and '#' is used as backspace, 'B' accepts
int read_input(int at_most, bool left_justified, bool display_colon, bool accept_less = false) {
  number_display.clear();
  int number = 0;
  int digits = 0;
  for (;;) {
    char just_pressed = await_first_press();
    if (just_pressed == CANCEL_BUTTON) {
      return -1;
    }
    if (just_pressed == BACKSPACE_BUTTON) {
      if (digits > 0) {
        number /= 10;
        digits -= 1;
      }
    }
    if (just_pressed == ACCEPT_BUTTON && (accept_less || digits == at_most)) {
      return number;
    }
    if (digits < at_most && just_pressed >= '0' && just_pressed <= '9') {
      uint inputted_num = just_pressed - '0';
      number *= 10;
      number += inputted_num;
      digits += 1;
#ifdef ENABLE_SERIAL
      Serial.println(just_pressed);
      Serial.println(inputted_num);
      Serial.print("Current number: ");
      Serial.println(number);
      Serial.print("Current digits: ");
      Serial.println(digits);
#endif
    }
    number_display.clear();
    if (digits > 0) {
      if (left_justified) {
        number_display.showNumberDecEx(number, display_colon ? COLON_DOTS : 0, true, digits);
      } else {
        number_display.showNumberDecEx(number, display_colon ? COLON_DOTS : 0, true, digits, 4 - digits);
      }
    }
  }
}

int read_two(int left_max, int right_max) {
  number_display.clear();
  int number = 0;
  int digits = 0;
  int first_left_max = left_max / 10;
  int first_right_max = right_max / 10;
  for (;;) {
    char just_pressed = await_first_press();
    if (just_pressed == CANCEL_BUTTON) {
      return -1;
    }
    if (just_pressed == BACKSPACE_BUTTON) {
      if (digits > 0) {
        number /= 10;
        digits -= 1;
      }
    }
    if (just_pressed == ACCEPT_BUTTON && digits == 4) {
      return number;
    }
    if (digits < 4 && just_pressed >= '0' && just_pressed <= '9') {
      uint inputted_num = just_pressed - '0';
      uint predicted_num = number * 10 + inputted_num;
      bool allowed = true;
      switch (digits) {
        case 0:
          if (inputted_num > first_left_max) {
            allowed = false;
          }
          break;
        case 1:
          if (predicted_num > left_max) {
            allowed = false;
          }
          break;
        case 2:
          if (inputted_num > first_right_max) {
            allowed = false;
          }
          break;
        case 3:
          uint predicted_right_num = predicted_num - (predicted_num / 100) * 100;
          if (predicted_right_num > right_max) {
            allowed = false;
          }
      }
      if (allowed) {
        number = predicted_num;
        digits += 1;
      }
#ifdef ENABLE_SERIAL
      Serial.println(inputted_num);
      Serial.print("Current number: ");
      Serial.println(number);
      Serial.print("Current digits: ");
      Serial.println(digits);
#endif
    }
    number_display.clear();
    if (digits > 0) {
      number_display.showNumberDecEx(number, COLON_DOTS, true, digits);
    }
  }
}

void show_error() {
  number_display.setSegments(*erro);
  await_first_press();
}

int count_to_change = CHANGE_TIME;
char display_mode = 0;  // 0 - time, 1 - date, 2 - temperature

void idle_cycle_count() {
  count_to_change--;
  if (count_to_change == 0) {
    count_to_change = CHANGE_TIME;
    display_mode = (display_mode + 1) % 3;
  }
}


void time_setting_mode() {
  number_display.setSegments(abcd);
  char just_pressed = await_first_press();
  int res;
  switch (just_pressed) {
    case 'A':
      {
        res = read_two(23, 59);
        if (res == -1) {
          return;
        }
        char hours = res / 100;
        char minutes = res % 100;
#ifdef ENABLE_SERIAL
        Serial.println(res);
        Serial.print((int)hours);
        Serial.print(':');
        Serial.println((int)minutes);
#endif
        Clock.setHour(hours);
        Clock.setMinute(minutes);
        Clock.setSecond(0);
        break;
      }
    case 'B':
      {
        res = read_two(31, 12);
        if (res == -1) {
          return;
        }
        char day = res / 100;
        char month = res % 100;
        Clock.setDate(day);
        Clock.setMonth(month);
        break;
      }
    case 'C':
      {
        int res = read_input(4, true, true);
        if (res < 2000 || res > 2099) {
          show_error();
          return;
        }
        byte year = res / 100;
        Clock.setYear(year);
        break;
      }
    case 'D':
      {

        break;
      }
  }
}
const uint8_t edit[4] = {
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
  SEG_B | SEG_D | SEG_C | SEG_D | SEG_E | SEG_G,
  SEG_E | SEG_F,
  SEG_D | SEG_E | SEG_F | SEG_G
};

uint8_t edit_bitmask(uint8_t bitmask) {
  uint8_t result = bitmask;
  dow_led_light_bitmask(result);
  number_display.setSegments(edit);
  for (;;) {
    char just_pressed = await_first_press();
    switch (just_pressed) {
      case ACCEPT_BUTTON:
        {
          return result;
        }
      case CANCEL_BUTTON:
        {
          return bitmask;
        }
      case '8':
        {
          result = ALL_DAYS;
          break;
        }
      case '9':
        {
          result = 0;
          break;
        }
      case '0':
        {
          result ^= 0b01111111;
          break;
        }
      case '*':
        {
          result = WEEKENDS;
          break;
        }
      case '#':
        {
          result = WORK_DAYS;
          break;
        }
      default:
        {
          if (just_pressed >= '1' && just_pressed <= '7') {
            uint8_t dow = just_pressed - '0' - 1;
            result = result ^ days_of_week[dow];
          }
          break;
        }
    }
    dow_led_light_bitmask(result);
  }
}

#define LETTER_A SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G
#define LETTER_E SEG_A | SEG_D | SEG_E | SEG_F | SEG_G
// returns true if accepted
bool edit_schedule(Schedule &schedule) {
  Schedule copy = schedule;
  uint8_t status_report[2] = { schedule.is_active ? LETTER_E : 0, schedule.mode ? LETTER_A : 0 };
  number_display.clear();
  number_display.setSegments(status_report, 2);
  uint8_t current_bitmask_displayed = schedule.days_of_week;
  dow_led_light_bitmask(schedule.days_of_week);
  for (;;) {
    char just_pressed = await_first_press();
    switch (just_pressed) {
      case ACCEPT_BUTTON:
        {
          return true;
        }
      case CANCEL_BUTTON:
        {
          schedule = copy;
          return false;
        }
      case '1':
        {
          int time = read_two(23, 59);
          if (time != -1) {
            uint8_t hour = time / 100;
            uint8_t minute = time % 100;
            uint total_minutes = hour * 60 + minute;
            schedule.total_minutes = total_minutes;
          }
          number_display.clear();
        }
        break;
      case '2':
        {
          uint8_t new_bitmask = edit_bitmask(schedule.days_of_week);
          schedule.days_of_week = new_bitmask;
          number_display.clear();
          break;
        }
      case '3':
        {
          schedule.mode = !schedule.mode;
          status_report[1] = schedule.mode ? LETTER_A : 0;
          break;
        }
      case '4':
        {
          schedule.is_active = !schedule.is_active;
          status_report[0] = schedule.is_active ? LETTER_E : 0;
          break;
        }
      case '5':
        {
          show_time(schedule.total_minutes / 60, schedule.total_minutes % 60);
          await_first_press();
          number_display.clear();
          break;
        }
    }
    number_display.setSegments(status_report, 2);
    if (current_bitmask_displayed != schedule.days_of_week) {
      dow_led_light_bitmask(schedule.days_of_week);
      current_bitmask_displayed = schedule.days_of_week;
    }
  }
}


void schedule_editing_mode() {
  int schedule_index = read_input(3, true, false, true);
  if (schedule_index == -1) {
    return;
  }
  schedule_index--;
  if (schedule_index >= max_schedules) {
    show_error();
    return;
  }
  Schedule loaded = load_schedule(schedule_index);
  if (edit_schedule(loaded)) {
    save_schedule(loaded, schedule_index);
  }
}

int override_minutes_left = 0;
// amount in minutes
void set_override_time(int amount) {
}

uint8_t current_dow = 0;
bool dow_requires_update = true;

int last_checked = -1;  // only a minute
int toggle_in_minutes = -1;
void loop() {
  DateTime now = rtc.now();
#ifdef ENABLE_SERIAL
  if (Serial.available()) {
    String str = Serial.readString();
    if (str.equals("get\n")) {
      Serial.print(now.year(), DEC);
      Serial.print('/');
      Serial.print(now.month(), DEC);
      Serial.print('/');
      Serial.print(now.day(), DEC);
      Serial.print(' ');
      Serial.print(now.hour(), DEC);
      Serial.print(':');
      Serial.print(now.minute(), DEC);
      Serial.print(':');
      Serial.print(now.second(), DEC);
      Serial.println();

      Serial.print(" since midnight 1/1/1970 = ");
      Serial.print(now.unixtime());
      Serial.print("s = ");
      Serial.print(now.unixtime() / 86400L);
      Serial.println("d");
    } else if (str.equals("RESS\n")) {
      Serial.println("!!! Starting to reset all schedules !!!");
      Schedule empty_schedule = {
        .is_active = false,
        .mode = false,
        .total_minutes = 0,
        .days_of_week = 0
      };
      for (int i = 0; i < max_schedules; i++) {
        save_schedule(empty_schedule, i);
      }
      Serial.println("!!! Finished resetting all schedules !!!");
    } else if (str.equals("LIST\n")) {
      for (int i = 0; i < max_schedules; i++) {
        Serial.print("Schedule ");
        Serial.println(i);
        Schedule schedule = load_schedule(i);
        print_schedule(schedule);
      }
    } else if (str.equals("LAST\n")) {
      print_schedule(last_schedule);
    } else if (str.equals("NEXT\n")) {
      print_schedule(next_schedule);
    } else if (str[0] == 's') {
      int idx = str.substring(1).toInt();
      Schedule schedule = load_schedule(idx);
      print_schedule(schedule);
    } else if (str[0] == 'd') {
      int day = str.substring(1).toInt();
      Clock.setDate(day);
    } else if (str[0] == 'm') {
      int month = str.substring(1).toInt();
      Clock.setMonth(month);
    } else if (str[0] == 'e') {
      time_t epoch = str.substring(1).toInt();
      Clock.setEpoch(epoch);
    } else {
      int sepidx = str.indexOf(':');
      if (sepidx != -1 && sepidx != (str.length() - 1)) {
        int hours = str.substring(0, sepidx).toInt();
        int minute = str.substring(sepidx + 1).toInt();
        Clock.setHour(hours);
        Clock.setMinute(minute);
        Clock.setSecond(0);
      }
    }
  }
#endif
  switch (display_mode) {
    case 0:
      show_time(now.hour(), now.minute());
      break;
    case 1:
      show_date(now.day(), now.month());
      break;
    case 2:
      show_temperature(Clock.getTemperature());
      break;
  }
  if (dow_requires_update || current_dow != now.dayOfTheWeek()) {
    dow_requires_update = false;
    current_dow = now.dayOfTheWeek();
    dow_led_light_bitmask(days_of_week[current_dow - 1]);
  }
  if (last_checked != now.minute()) {
#ifdef ENABLE_SERIAL
    Serial.println("Doing check");
#endif
    last_checked = now.minute();
    if (override_minutes_left > 0) {
      override_minutes_left--;
    }
    if (next_schedule_available) {
      if (toggle_in_minutes == -1) {
        toggle_in_minutes = minutes_it_will_happen_in(next_schedule, now.hour() * 60 + now.minute(), days_of_week[now.dayOfTheWeek() - 1]);
      } else {
        toggle_in_minutes--;
      }
      if (toggle_in_minutes == 0) {
        if (override_minutes_left == 0) {
          setRelayState(next_schedule.mode);
        }
        find_last_next_schedules(now.hour(), now.minute(), now.dayOfTheWeek());
        if (next_schedule_available) {
          toggle_in_minutes = minutes_it_will_happen_in(next_schedule, now.hour() * 60 + now.minute(), days_of_week[now.dayOfTheWeek() - 1]);
        } else {
          toggle_in_minutes = -1;
        }
      }
    }
#ifdef ENABLE_SERIAL
    Serial.print("Minutes to activate left: ");
    Serial.println(toggle_in_minutes);
#endif
  }
  idle_cycle_count();
  char just_pressed = get_just_pressed();
#ifdef ENABLE_SERIAL
  if (just_pressed) {
    Serial.println(just_pressed);
  }
#endif
#define UPDATE_EVERYTHING \
  dow_requires_update = true; \
  last_checked = -1; \
  toggle_in_minutes = -1; \
  override_minutes_left++;
  switch (just_pressed) {
    case 'A':
      {
        time_setting_mode();
        UPDATE_EVERYTHING
        break;
      }
    case 'B':
      {
        schedule_editing_mode();
        find_last_next_schedules(now.hour(), now.minute(), now.dayOfTheWeek());
        if (last_schedule_available) {
          setRelayState(last_schedule.mode);
        }
        UPDATE_EVERYTHING
        break;
      }
    case 'C':
      {
        int new_override = read_two(23, 59);
        if (new_override != -1) {
          override_minutes_left = new_override;
        }
        break;
      }
    case 'D':
      {
        display_mode = (display_mode + 1) % 3;
        count_to_change = CHANGE_TIME;
        break;
      }
    case '*':
      {
        setRelayState(!relay_state);
        break;
      }
    case '#':
      {
        if (last_schedule_available) {
          setRelayState(last_schedule.mode);
        }
        break;
      }
  }
  delay(UPDATE_DELAY);
}
