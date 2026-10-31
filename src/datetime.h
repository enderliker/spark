/* datetime.h - date and time formatting and parsing (T117).
 *
 * The module is named `datetime` rather than `time` because `src/time.h` would
 * shadow the system <time.h> for every file compiled with -Isrc. */
#ifndef SPARK_DATETIME_H
#define SPARK_DATETIME_H

void datetime_init(void);

#endif