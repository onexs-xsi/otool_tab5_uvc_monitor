#pragma once
/* Callbacks are deliberately driven sequentially in this native transport test. */
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
