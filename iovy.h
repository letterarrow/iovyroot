#ifndef IOVY_H
#define IOVY_H

#include <stdbool.h>

extern bool iovy_run_exploit(unsigned long address, int value,
	bool(*exploit_callback)(void* user_data), void *user_data);

#endif
