/* datetime.c - date and time formatting and parsing (T117).
 *
 * Everything is UTC unless the caller asks for the local zone with the
 * `local` flag, so results are reproducible and independent of the host's
 * time zone and locale. The calendar arithmetic is done here (days from civil
 * date and back) rather than with strftime/strptime: that keeps the format
 * fixed to English names, avoids the locale entirely, and lets %j, %w, %a, %A,
 * %b, %B, %z and %Z share one implementation. Only localtime_r, clock_gettime
 * and strftime("%Z") are used for local-zone information.
 *
 * The supported range is 0001-01-01T00:00:00Z .. 9999-12-31T23:59:59Z. */
#include "datetime.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "buf.h"
#include "object.h"
#include "vm.h"

#define DT_MIN_SECONDS (-62135596800LL) /* 0001-01-01T00:00:00Z */
#define DT_MAX_SECONDS 253402300799LL   /* 9999-12-31T23:59:59Z */

static const char *const MONTHS_SHORT[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const MONTHS_LONG[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *const WEEKDAYS_SHORT[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *const WEEKDAYS_LONG[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static const char *datetime_type_name(Value value)
{
    switch (value.type) {
    case VAL_NIL: return "nil";
    case VAL_BOOL: return "bool";
    case VAL_NUMBER: return "number";
    case VAL_INT: return "number";
    case VAL_OBJ: return obj_type_name(AS_OBJ(value)->type);
    }
    return "unknown";
}

/* ---- Calendar arithmetic ------------------------------------------------- */

static int64_t floor_div(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;

    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        quotient--;
    }
    return quotient;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
 * algorithm). `month` is 1..12. */
static int64_t days_from_civil(int64_t year, int month, int day)
{
    int64_t era;
    unsigned year_of_era;
    unsigned day_of_year;
    unsigned day_of_era;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = (unsigned)(year - era * 400);
    day_of_year = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + (int64_t)day_of_era - 719468;
}

static void civil_from_days(int64_t days, int64_t *year, int *month, int *day)
{
    int64_t era;
    unsigned day_of_era;
    unsigned year_of_era;
    unsigned day_of_year;
    unsigned month_index;

    days += 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    day_of_era = (unsigned)(days - era * 146097);
    year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
                   day_of_era / 146096) / 365;
    *year = (int64_t)year_of_era + era * 400;
    day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    month_index = (5 * day_of_year + 2) / 153;
    *day = (int)(day_of_year - (153 * month_index + 2) / 5 + 1);
    *month = (int)(month_index + (month_index < 10 ? 3 : -9));
    *year += *month <= 2;
}

static bool is_leap_year(int64_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int month_length(int64_t year, int month)
{
    static const int LENGTHS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return LENGTHS[month - 1];
}

static int64_t civil_to_seconds(int64_t year, int month, int day, int hour, int minute,
                                int second)
{
    return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

/* ---- Time-zone helpers --------------------------------------------------- */

static int64_t now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

/* Seconds east of UTC at that instant, from the C library's local zone. */
static bool local_offset_seconds(int64_t seconds, const char *function, int64_t *offset)
{
    time_t when = (time_t)seconds;
    struct tm tm;

    if (!localtime_r(&when, &tm)) {
        vm_runtime_error(ERROR_VALUE, "'%s' timestamp is out of range", function);
        return false;
    }
    *offset = civil_to_seconds(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                               tm.tm_min, tm.tm_sec) - seconds;
    return true;
}

/* ---- Formatting ---------------------------------------------------------- */

typedef struct {
    int64_t year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int weekday; /* 0 = Sunday */
    int day_of_year;
    int64_t offset;
    bool local_mode;
    struct tm tm;
} DateTimeParts;

static bool datetime_seconds(Value value, const char *function, int64_t *out)
{
    if (is_int(value)) {
        *out = AS_INT(value);
    } else if (is_number(value)) {
        double raw = AS_NUMBER(value);

        if (!isfinite(raw) || raw < (double)DT_MIN_SECONDS || raw > (double)DT_MAX_SECONDS) {
            vm_runtime_error(ERROR_VALUE, "'%s' timestamp is out of range", function);
            return false;
        }
        *out = (int64_t)floor(raw);
    } else {
        vm_runtime_error(ERROR_TYPE, "'%s' expects a timestamp, received %s", function,
                         datetime_type_name(value));
        return false;
    }
    if (*out < DT_MIN_SECONDS || *out > DT_MAX_SECONDS) {
        vm_runtime_error(ERROR_VALUE, "'%s' timestamp is out of range", function);
        return false;
    }
    return true;
}

static bool parts_for(Value timestamp, bool local_mode, const char *function,
                      DateTimeParts *parts)
{
    int64_t seconds;

    if (!datetime_seconds(timestamp, function, &seconds)) {
        return false;
    }
    if (local_mode) {
        time_t when = (time_t)seconds;
        struct tm tm;

        if (!localtime_r(&when, &tm)) {
            vm_runtime_error(ERROR_VALUE, "'%s' timestamp is out of range", function);
            return false;
        }
        parts->year = tm.tm_year + 1900;
        parts->month = tm.tm_mon + 1;
        parts->day = tm.tm_mday;
        parts->hour = tm.tm_hour;
        parts->minute = tm.tm_min;
        parts->second = tm.tm_sec;
        parts->weekday = tm.tm_wday;
        parts->day_of_year = tm.tm_yday + 1;
        parts->offset = civil_to_seconds(parts->year, parts->month, parts->day, parts->hour,
                                         parts->minute, parts->second) - seconds;
        parts->local_mode = true;
        parts->tm = tm;
    } else {
        int64_t days = floor_div(seconds, 86400);
        int64_t seconds_of_day = seconds - days * 86400;

        civil_from_days(days, &parts->year, &parts->month, &parts->day);
        parts->hour = (int)(seconds_of_day / 3600);
        parts->minute = (int)((seconds_of_day % 3600) / 60);
        parts->second = (int)(seconds_of_day % 60);
        parts->weekday = (int)(((days + 4) % 7 + 7) % 7);
        parts->day_of_year = (int)(days - days_from_civil(parts->year, 1, 1)) + 1;
        parts->offset = 0;
        parts->local_mode = false;
        memset(&parts->tm, 0, sizeof(parts->tm));
    }
    return true;
}

static bool format_append(buf_t *out, const char *text)
{
    if (buf_append_str(out, text) != 0) {
        vm_runtime_error(ERROR_MEMORY, "could not allocate memory for 'time_format'");
        return false;
    }
    return true;
}

static bool format_appendf(buf_t *out, const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        vm_runtime_error(ERROR_VALUE, "'time_format' could not render a value");
        return false;
    }
    if (buf_reserve(out, (size_t)needed) != 0) {
        va_end(args);
        vm_runtime_error(ERROR_MEMORY, "could not allocate memory for 'time_format'");
        return false;
    }
    vsnprintf(out->data + out->len, (size_t)needed + 1, format, args);
    va_end(args);
    out->len += (size_t)needed;
    return true;
}

static bool format_directive(buf_t *out, char spec, const DateTimeParts *parts)
{
    switch (spec) {
    case 'Y':
        return format_appendf(out, "%04lld", (long long)parts->year);
    case 'm':
        return format_appendf(out, "%02d", parts->month);
    case 'd':
        return format_appendf(out, "%02d", parts->day);
    case 'H':
        return format_appendf(out, "%02d", parts->hour);
    case 'M':
        return format_appendf(out, "%02d", parts->minute);
    case 'S':
        return format_appendf(out, "%02d", parts->second);
    case 'j':
        return format_appendf(out, "%03d", parts->day_of_year);
    case 'w':
        return format_appendf(out, "%d", parts->weekday);
    case 'a':
        return format_append(out, WEEKDAYS_SHORT[parts->weekday]);
    case 'A':
        return format_append(out, WEEKDAYS_LONG[parts->weekday]);
    case 'b':
        return format_append(out, MONTHS_SHORT[parts->month - 1]);
    case 'B':
        return format_append(out, MONTHS_LONG[parts->month - 1]);
    case 'z': {
        int64_t magnitude = parts->offset < 0 ? -parts->offset : parts->offset;

        return format_appendf(out, "%c%02lld%02lld", parts->offset < 0 ? '-' : '+',
                              (long long)(magnitude / 3600), (long long)((magnitude % 3600) / 60));
    }
    case 'Z': {
        char zone[64];

        if (!parts->local_mode) {
            return format_append(out, "UTC");
        }
        /* Best effort: the C library's abbreviation, which depends on the
         * host's zone database. An empty result is allowed. */
        zone[0] = '\0';
        if (strftime(zone, sizeof(zone), "%Z", &parts->tm) == 0) {
            zone[0] = '\0';
        }
        return format_append(out, zone);
    }
    case '%':
        return format_append(out, "%");
    }
    vm_runtime_error(ERROR_VALUE, "'time_format' does not know the '%%%c' directive", spec);
    return false;
}

static bool format_into(buf_t *out, const ObjString *format, const DateTimeParts *parts)
{
    for (int i = 0; i < format->length; i++) {
        char current = format->chars[i];

        if (current != '%') {
            if (buf_append(out, &current, 1) != 0) {
                vm_runtime_error(ERROR_MEMORY, "could not allocate memory for 'time_format'");
                return false;
            }
            continue;
        }
        i++;
        if (i >= format->length) {
            vm_runtime_error(ERROR_VALUE, "'time_format' ends with an incomplete '%%'");
            return false;
        }
        if (!format_directive(out, format->chars[i], parts)) {
            return false;
        }
    }
    return true;
}

/* ---- Parsing ------------------------------------------------------------- */

typedef struct {
    int64_t year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int day_of_year; /* 0 = absent */
    bool has_month_day;
    int64_t offset; /* only meaningful when has_offset */
    bool has_offset;
} ParsedTime;

static bool consume_name(const char *text, int *position, const char *name)
{
    size_t length = strlen(name);

    if (strlen(text + *position) < length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char left = text[*position + (int)i];
        char right = name[i];

        if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
        if (left != right) {
            return false;
        }
    }
    *position += (int)length;
    return true;
}

static bool parse_number(const char *text, int *position, int minimum_digits, int maximum_digits,
                         int *out)
{
    int digits = 0;
    int value = 0;

    while (digits < maximum_digits && text[*position + digits] >= '0' &&
           text[*position + digits] <= '9') {
        value = value * 10 + (text[*position + digits] - '0');
        digits++;
    }
    if (digits < minimum_digits) {
        return false;
    }
    *position += digits;
    *out = value;
    return true;
}

static bool match_named(const char *text, int *position, const char *const *names, int count,
                        bool allow_short, int *out)
{
    for (int i = 0; i < count; i++) {
        if (consume_name(text, position, names[i])) {
            *out = i;
            return true;
        }
        if (allow_short) {
            size_t length = strlen(names[i]);
            char short_name[4] = { names[i][0], names[i][1], names[i][2], '\0' };

            if (length >= 3 && consume_name(text, position, short_name)) {
                *out = i;
                return true;
            }
        }
    }
    return false;
}

static bool parse_directive(const char *text, int *position, char spec, ParsedTime *parsed,
                            const char *function)
{
    int value = 0;

    switch (spec) {
    case 'Y':
        if (!parse_number(text, position, 1, 4, &value)) {
            goto mismatch;
        }
        parsed->year = value;
        return true;
    case 'm':
        if (!parse_number(text, position, 1, 2, &value)) goto mismatch;
        parsed->month = value;
        parsed->has_month_day = true;
        return true;
    case 'd':
        if (!parse_number(text, position, 1, 2, &value)) goto mismatch;
        parsed->day = value;
        parsed->has_month_day = true;
        return true;
    case 'H':
        if (!parse_number(text, position, 1, 2, &value)) goto mismatch;
        parsed->hour = value;
        return true;
    case 'M':
        if (!parse_number(text, position, 1, 2, &value)) goto mismatch;
        parsed->minute = value;
        return true;
    case 'S':
        if (!parse_number(text, position, 1, 2, &value)) goto mismatch;
        parsed->second = value;
        return true;
    case 'j':
        if (!parse_number(text, position, 1, 3, &value)) goto mismatch;
        parsed->day_of_year = value;
        return true;
    case 'w':
        if (!parse_number(text, position, 1, 1, &value)) goto mismatch;
        return true; /* derived from the date */
    case 'a':
    case 'A':
        if (!match_named(text, position, WEEKDAYS_LONG, 7, true, &value) &&
            !match_named(text, position, WEEKDAYS_SHORT, 7, false, &value)) {
            goto mismatch;
        }
        return true; /* derived from the date */
    case 'b':
    case 'B':
        if (!match_named(text, position, MONTHS_LONG, 12, true, &value) &&
            !match_named(text, position, MONTHS_SHORT, 12, false, &value)) {
            goto mismatch;
        }
        parsed->month = value + 1;
        parsed->has_month_day = true;
        return true;
    case 'z': {
        int sign;
        int hours;
        int minutes;

        if (text[*position] == '+') {
            sign = 1;
        } else if (text[*position] == '-') {
            sign = -1;
        } else {
            goto mismatch;
        }
        (*position)++;
        if (!parse_number(text, position, 4, 4, &value)) goto mismatch;
        hours = value / 100;
        minutes = value % 100;
        if (hours > 23 || minutes > 59) {
            vm_runtime_error(ERROR_VALUE, "'%s' has an invalid %%z offset in the text", function);
            return false;
        }
        parsed->offset = (int64_t)sign * (hours * 3600 + minutes * 60);
        parsed->has_offset = true;
        return true;
    }
    case 'Z':
        /* Best effort: consume the zone abbreviation and ignore it, since the
         * text carries no reliable offset. */
        while ((text[*position] >= 'A' && text[*position] <= 'Z') ||
               (text[*position] >= 'a' && text[*position] <= 'z') ||
               text[*position] == '_' || text[*position] == '/') {
            (*position)++;
        }
        return true;
    case '%':
        if (text[*position] != '%') goto mismatch;
        (*position)++;
        return true;
    }
    vm_runtime_error(ERROR_VALUE, "'%s' does not know the '%%%c' directive", function, spec);
    return false;

mismatch:
    vm_runtime_error(ERROR_VALUE, "'%s' text does not match the format at offset %d", function,
                     *position);
    return false;
}

static bool parse_into(const ObjString *text, const ObjString *format, const char *function,
                       ParsedTime *parsed)
{
    int position = 0;

    memset(parsed, 0, sizeof(*parsed));
    parsed->year = 1970;
    parsed->month = 1;
    parsed->day = 1;

    for (int i = 0; i < format->length; i++) {
        char current = format->chars[i];

        if (current != '%') {
            if (text->chars[position] != current) {
                vm_runtime_error(ERROR_VALUE,
                                 "'%s' text does not match the format at offset %d", function,
                                 position);
                return false;
            }
            position++;
            continue;
        }
        i++;
        if (i >= format->length) {
            vm_runtime_error(ERROR_VALUE, "'%s' format ends with an incomplete '%%'", function);
            return false;
        }
        if (!parse_directive(text->chars, &position, format->chars[i], parsed, function)) {
            return false;
        }
    }
    if (text->chars[position] != '\0') {
        vm_runtime_error(ERROR_VALUE, "'%s' text has trailing characters at offset %d", function,
                         position);
        return false;
    }
    return true;
}

static bool resolve_parsed(const ParsedTime *parsed, bool local_mode, const char *function,
                           Value *out)
{
    int64_t year = parsed->year;
    int month = parsed->month;
    int day = parsed->day;
    int64_t civil;
    int64_t seconds;

    if (parsed->day_of_year != 0) {
        int remaining = parsed->day_of_year;
        int length = is_leap_year(year) ? 366 : 365;

        if (parsed->has_month_day) {
            vm_runtime_error(ERROR_VALUE,
                             "'%s' cannot combine %%j with %%m/%%d in the same format", function);
            return false;
        }
        if (remaining < 1 || remaining > length) {
            vm_runtime_error(ERROR_VALUE, "'%s' day of year is out of range: %d", function,
                             parsed->day_of_year);
            return false;
        }
        month = 1;
        while (remaining > month_length(year, month)) {
            remaining -= month_length(year, month);
            month++;
        }
        day = remaining;
    }
    if (year < 1 || year > 9999) {
        vm_runtime_error(ERROR_VALUE, "'%s' year is out of range: %lld (1..9999)", function,
                         (long long)year);
        return false;
    }
    if (month < 1 || month > 12) {
        vm_runtime_error(ERROR_VALUE, "'%s' month is out of range: %d (1..12)", function, month);
        return false;
    }
    if (day < 1 || day > month_length(year, month)) {
        vm_runtime_error(ERROR_VALUE, "'%s' day is out of range for that month: %d", function,
                         day);
        return false;
    }
    if (parsed->hour < 0 || parsed->hour > 23 || parsed->minute < 0 || parsed->minute > 59 ||
        parsed->second < 0 || parsed->second > 59) {
        vm_runtime_error(ERROR_VALUE, "'%s' time of day is out of range", function);
        return false;
    }

    civil = civil_to_seconds(year, month, day, parsed->hour, parsed->minute, parsed->second);
    if (parsed->has_offset) {
        /* An explicit %z wins over the local flag. */
        seconds = civil - parsed->offset;
    } else if (local_mode) {
        int64_t offset;

        if (!local_offset_seconds(civil, function, &offset)) {
            return false;
        }
        seconds = civil - offset;
        /* Near a DST change the offset may differ after the first guess. */
        if (!local_offset_seconds(seconds, function, &offset)) {
            return false;
        }
        seconds = civil - offset;
    } else {
        seconds = civil;
    }
    if (seconds < DT_MIN_SECONDS || seconds > DT_MAX_SECONDS) {
        vm_runtime_error(ERROR_VALUE, "'%s' result is out of range", function);
        return false;
    }
    *out = INT_VAL(seconds);
    return true;
}

/* ---- Natives ------------------------------------------------------------- */

static bool datetime_local_flag(Value value, const char *function, bool *out)
{
    if (!is_bool(value)) {
        vm_runtime_error(ERROR_TYPE,
                         "'%s' expects a boolean as the local flag, received %s", function,
                         datetime_type_name(value));
        return false;
    }
    *out = AS_BOOL(value);
    return true;
}

/* time_format(timestamp?, format, local?) - UTC by default. */
static Value native_time_format(int arg_count, Value *args)
{
    Value timestamp = NIL_VAL;
    bool has_timestamp = false;
    bool local_mode = false;
    ObjString *format;
    DateTimeParts parts;
    buf_t out;
    ObjString *result;

    if (arg_count < 1 || arg_count > 3) {
        vm_runtime_error(ERROR_ARITY, "'time_format' expects 1 to 3 arguments, received %d",
                         arg_count);
        return NIL_VAL;
    }
    if (is_obj_type(args[0], OBJ_STRING)) {
        format = AS_STRING(args[0]);
        if (arg_count == 2 && !datetime_local_flag(args[1], "time_format", &local_mode)) {
            return NIL_VAL;
        }
        if (arg_count > 2) {
            vm_runtime_error(ERROR_ARITY,
                             "'time_format' expects (timestamp, format) or (format, local?)");
            return NIL_VAL;
        }
    } else {
        timestamp = args[0];
        has_timestamp = true;
        if (arg_count < 2 || !is_obj_type(args[1], OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE,
                             "'time_format' expects a format string as second argument, "
                             "received %s", arg_count < 2 ? "nothing" : datetime_type_name(args[1]));
            return NIL_VAL;
        }
        format = AS_STRING(args[1]);
        if (arg_count == 3 && !datetime_local_flag(args[2], "time_format", &local_mode)) {
            return NIL_VAL;
        }
    }
    if (!has_timestamp) {
        timestamp = NUMBER_VAL((double)now_seconds());
    }
    if (!parts_for(timestamp, local_mode, "time_format", &parts)) {
        return NIL_VAL;
    }

    buf_init(&out);
    if (!format_into(&out, format, &parts)) {
        buf_free(&out);
        return NIL_VAL;
    }
    if (out.len > (size_t)INT_MAX) {
        buf_free(&out);
        vm_runtime_error(ERROR_MEMORY, "'time_format' result is too long");
        return NIL_VAL;
    }
    result = object_string(out.data ? out.data : "", (int)out.len);
    buf_free(&out);
    return OBJ_VAL(result);
}

/* time_parse(text, format, local?) - UTC by default. */
static Value native_time_parse(int arg_count, Value *args)
{
    bool local_mode = false;
    ParsedTime parsed;
    Value result;

    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error(ERROR_ARITY, "'time_parse' expects 2 or 3 arguments, received %d",
                         arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'time_parse' expects a string as first argument, "
                         "received %s", datetime_type_name(args[0]));
        return NIL_VAL;
    }
    if (!is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'time_parse' expects a format string as second argument, "
                         "received %s", datetime_type_name(args[1]));
        return NIL_VAL;
    }
    if (arg_count == 3 && !datetime_local_flag(args[2], "time_parse", &local_mode)) {
        return NIL_VAL;
    }
    if (!parse_into(AS_STRING(args[0]), AS_STRING(args[1]), "time_parse", &parsed)) {
        return NIL_VAL;
    }
    if (!resolve_parsed(&parsed, local_mode, "time_parse", &result)) {
        return NIL_VAL;
    }
    return result;
}

/* time_utc(timestamp) -> 0; time_local(timestamp) -> seconds east of UTC. */
static Value native_time_utc(int arg_count, Value *args)
{
    int64_t seconds;

    (void)arg_count;
    if (!datetime_seconds(args[0], "time_utc", &seconds)) {
        return NIL_VAL;
    }
    return INT_VAL(0);
}

static Value native_time_local(int arg_count, Value *args)
{
    int64_t seconds;
    int64_t offset;

    (void)arg_count;
    if (!datetime_seconds(args[0], "time_local", &seconds)) {
        return NIL_VAL;
    }
    if (!local_offset_seconds(seconds, "time_local", &offset)) {
        return NIL_VAL;
    }
    return INT_VAL(offset);
}

/* Wall-clock milliseconds since the Unix epoch (clock() remains the monotonic
 * clock for measuring durations). */
static Value native_time_ms(int arg_count, Value *args)
{
    struct timeval tv;

    (void)arg_count;
    (void)args;
    gettimeofday(&tv, NULL);
    return INT_VAL((int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

void datetime_init(void)
{
    vm_define_native("time_format", native_time_format, -1);
    vm_define_native("time_parse", native_time_parse, -1);
    vm_define_native("time_utc", native_time_utc, 1);
    vm_define_native("time_local", native_time_local, 1);
    vm_define_native("time_ms", native_time_ms, 0);
}