#ifndef __PGS_UTILS_H__
#define __PGS_UTILS_H__

#include "pg_sphere.h"

/* This file contains declarations for additional functions. */

/*
 * Converts steradians to square degrees.
 */
Datum	square_degrees(PG_FUNCTION_ARGS);

/*
 * Converts square_degrees to steradians.
 */
Datum	steradians(PG_FUNCTION_ARGS);
#endif
