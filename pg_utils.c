#include "pg_utils.h"


PG_FUNCTION_INFO_V1(square_degrees);
PG_FUNCTION_INFO_V1(steradians);

Datum	square_degrees(PG_FUNCTION_ARGS)
{
	float8		sr = PG_GETARG_FLOAT8(0);
	float8		sd;

	sd = sr * RADIANS * RADIANS;
	PG_RETURN_FLOAT8(sd);
}

Datum	steradians(PG_FUNCTION_ARGS)
{
	float8		sd = PG_GETARG_FLOAT8(0);
	float8		sr;

	sr = sd / RADIANS / RADIANS;
	PG_RETURN_FLOAT8(sr);
}
