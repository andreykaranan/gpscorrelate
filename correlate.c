/* correlate.c
 * Written by Daniel Foote.
 * Started Feb 2005.
 *
 * The functions in this file match the timestamps on
 * the photos to the GPS data, and then, if a match
 * is found, writes the GPS data into the EXIF data
 * in the photo. For future reference... */

/* Copyright 2005 Daniel Foote.
 *
 * This file is part of gpscorrelate.
 *
 * gpscorrelate is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * gpscorrelate is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gpscorrelate; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "gpsstructure.h"
#include "exif-gps.h"
#include "correlate.h"
#include "unixtime.h"

/* Internal functions used to make it work. */
void Round(struct GPSPoint* First, struct GPSPoint* Result,
		struct CorrelateOptions* Options, time_t PhotoTime);
void Interpolate(struct GPSPoint* First, struct GPSPoint* Result,
		struct CorrelateOptions* Options, time_t PhotoTime);

/* This function returns a GPSPoint with the point selected for the
 * file. This allows us to do funky stuff like not actually write
 * the files - ie, just correlate and keep into memory... */

struct GPSPoint* CorrelatePhoto(char* Filename,
		struct CorrelateOptions* Options)
{
	/* Read out the timestamp from the EXIF data. */
	char* TimeTemp; int IncludesGPS = 0;
	TimeTemp = ReadExifDate(Filename, &IncludesGPS);
	if (!TimeTemp)
	{
		/* Error reading the time from the file. Abort. */
		/* If this was a read error, then a seperate message
		 * will appear on the console. Otherwise, we were
		 * returned here due to the lack of exif tags. */
		Options->Result = CORR_NOEXIFINPUT;
		return 0;
	}
	if (IncludesGPS)
	{
		/* Already have GPS data in the file!
		 * So we can't do this again... */
		Options->Result = CORR_GPSDATAEXISTS;
		return 0;
	}
	if (Options->AutoTimeZone)
	{
		/* Use the local time zone as of the date of first picture
		 * as the time for correlating all the remainder. */
		time_t RealTime;

		/* PhotoTime isn't actually Epoch-based, but will be wrong by
		 * an amount equal to the the local time zone offset. */
		time_t PhotoTime =
			ConvertToUnixTime(TimeTemp, EXIF_DATE_FORMAT, 0, 0);
		struct tm *PhotoTm = gmtime(&PhotoTime);
		PhotoTm->tm_isdst = -1;
		RealTime = mktime(PhotoTm);

		/* Finally, RealTime is the proper Epoch time of the photo.
		 * The difference from PhotoTime is the time zone offset. */
		Options->TimeZoneHours = (PhotoTime - RealTime) / 3600;
		Options->TimeZoneMins = ((PhotoTime - RealTime) % 3600) / 60;
		Options->AutoTimeZone = 0;
	}
	//printf("Using offset %02d:%02d\n", Options->TimeZoneHours, Options->TimeZoneMins);

	/* Now convert the time into Unixtime. */
	time_t PhotoTime =
		ConvertToUnixTime(TimeTemp, EXIF_DATE_FORMAT,
			Options->TimeZoneHours, Options->TimeZoneMins);

	/* Add the PhotoOffset time. This is to make the Photo time match
	 * the GPS time - ie, it is (GPS - Photo). */
	PhotoTime += Options->PhotoOffset;

	/* Free the memory for the time string - it won't otherwise
	 * be freed for us. */
	free(TimeTemp);

	/* Check to see if MinTime and MaxTime are filled in.
	 * If not, fill them in. */
	if (Options->MinTime == 0 && Options->MaxTime == 0)
	{
		/* Alright, fill them in! 
		 * Requires us to go through the list and keeping
		 * the biggest and smallest. The list should,
		 * however, be sorted. But we do it this way anyway. */
		struct GPSPoint* Fill = NULL;
		Options->MinTime = Options->Points->Time;
		for (Fill = Options->Points; Fill; Fill = Fill->Next)
		{
			/* Ignore trackseg markers... */
			if (Fill->Lat == 1000 && Fill->Long == 1000)
				continue;
			/* Check the Min time */
			if (Fill->Time < Options->MinTime)
				Options->MinTime = Fill->Time;
			/* Check the Max time */
			if (Fill->Time > Options->MaxTime) 
				Options->MaxTime = Fill->Time;
		}
	}

	/* Check that the photo is within the times that
	 * our tracks are for. Can't really match it if
	 * we were not logging when it was taken. */
	/* Note: photos taken between logging sessions of the
	 * same file will still make it inside of this. In
	 * some cases, it won't matter, but if it does, then
	 * keep this in mind!! */
	if ((PhotoTime < Options->MinTime) ||
			(PhotoTime > Options->MaxTime))
	{
		/* Outside the range. Abort. */
		Options->Result = CORR_NOMATCH;
		return 0;
	}

	/* Time to run through the list, and see if our PhotoTime
	 * is in between two points. Alternately, it might be
	 * exactly on a point... even better... */
	struct GPSPoint* Search;
	struct GPSPoint* Actual = malloc(sizeof(struct GPSPoint));

	Options->Result = CORR_NOMATCH; /* For convenience later */
	
	for (Search = Options->Points; Search; Search = Search->Next)
	{
		/* Sanity check: we need to peek at the next point.
		 * Make sure we can. */
		if (Search->Next == NULL) break;
		/* Sanity check: does this point have the same
		 * timestamp as the next? If so, skip onward. */
		if (Search->Time == Search->Next->Time) continue;
		/* Sanity check: does this point have a later
		 * timestamp than the next point? If so, skip. */
		if (Search->Time > Search->Next->Time) continue;

		if (Options->DoBetweenTrkSeg)
		{
			/* Righto, we are interpolating between segments.
			 * So simply do nothing! Simple! */
		} else {
			/* Don't check between track segments.
			 * If the end of segment marker is set, then simply
			 * "jump" over this point. */
			if (Search->EndOfSegment)
			{
				continue;
			}
		}

		/* Sanity check / track segment fix: is the photo time before
		 * the current point? If so, we've gone past it. Hrm. */
		if (Search->Time > PhotoTime)
		{
			Options->Result = CORR_NOMATCH;
			break;
		}
		
		/* Sort of sanity check: is this photo inside our
		 * "feather" time? If not, abort. */
		if (Options->FeatherTime)
		{
			/* Is the point between these two? */
			if ((PhotoTime > Search->Time) &&
				(PhotoTime < Search->Next->Time))
			{
				/* It is. Now is it too far
				 * from these two? */
				if (((Search->Time + Options->FeatherTime) < PhotoTime) &&
					((Search->Next->Time - Options->FeatherTime) > PhotoTime))
				{ 
					/* We are inside the feather
					 * time between two points.
					 * Abort. */
					Options->Result = CORR_TOOFAR;
					free(Actual);
					return NULL;
				} 
			}
		} /* endif (Options->Feather) */
		
		/* First test: is it exactly this point? */
		if (PhotoTime == Search->Time)
		{
			/* This is the point, exactly.
			 * Copy out the data and return that. */
			Actual->Lat = Search->Lat;
			Actual->Long = Search->Long;
			Actual->Elev = Search->Elev;
			Actual->Time = Search->Time;

			Options->Result = CORR_OK;
			break;
		}
		
		/* Second test: is it between this and the
		 * next point? */
		if ((PhotoTime > Search->Time) &&
				(PhotoTime < Search->Next->Time))
		{
			/* It is between these points.
			 * Unless told otherwise, we interpolate.
			 * If not interpolating, we round to nearest.
			 * If points are eqidistant, we round down. */
			if (Options->NoInterpolate)
			{
				/* No interpolation. Round. */
				Round(Search, Actual,
						Options, PhotoTime);
				Options->Result = CORR_ROUND;
				break;
			} else {
				/* Interpolate away! */
				Interpolate(Search, Actual,
						Options, PhotoTime);
				Options->Result = CORR_INTERPOLATED;
				break;
			}
		}
	} /* End for() loop to search. */

	/* Did we actually match it at all? */
	if (Options->Result == CORR_NOMATCH)
	{
		/* Nope, no match at all. */
		/* Return with nothing. */
		free(Actual);
		return NULL;
	}

	/* Write the data back into the Exif info. If we're allowed. */
	if (Options->NoWriteExif)
	{
		/* Don't write exif tags. Just return. */
		return Actual;
	} else {
		/* Do write the exif tags. And then return. */
		if (WriteGPSData(Filename, Actual, Options->Datum, Options->NoChangeMtime, Options->DegMinSecs))
		{
			/* All ok. Good! Return. */
			return Actual;
		} else {
			/* Not good. Return point, but note failure. */
			Options->Result = CORR_EXIFWRITEFAIL;
			return Actual;
		}
	}
	
	/* Looks like nothing matched. Free the prepared memory,
	 * and return nothing. */
	free(Actual);
	return NULL;
};

void Round(struct GPSPoint* First, struct GPSPoint* Result,
		struct CorrelateOptions* Options, time_t PhotoTime)
{
	/* Round the point between the two points - ie, it will end
	 * up being one or the other point. */
	struct GPSPoint* CopyFrom = NULL;

	/* Determine the difference between the two points. 
	 * We're using the scale function used by interpolate.
	 * This gives us a good view of where we are... */
	double Scale = (double)First->Next->Time - (double)First->Time;
	Scale = ((double)PhotoTime - (double)First->Time) / Scale;

	/* Compare our scale. */
	if (Scale <= 0.5)
	{
		/* Closer to the first point. */
		CopyFrom = First;
	} else {
		/* Closer to the second point. */
		CopyFrom = First->Next;
	}

	/* Copy the numbers over... */
	Result->Lat = CopyFrom->Lat;
	Result->Long = CopyFrom->Long;
	Result->Elev = CopyFrom->Elev;
	Result->Time = CopyFrom->Time;

	/* Done! */
	
}

void Interpolate(struct GPSPoint* First, struct GPSPoint* Result,
		struct CorrelateOptions* Options, time_t PhotoTime)
{
	/* Interpolate between the two points. The first point
	 * is First, the other First->Next. Results into Result. */

	/* Calculate the "scale": a decimal giving the relative distance
	 * in time between the two points. Ie, a number between 0 and 1 - 
	 * 0 is the first point, 1 is the next point, and 0.5 would be
	 * half way. */
	double Scale = (double)First->Next->Time - (double)First->Time;
	Scale = ((double)PhotoTime - (double)First->Time) / Scale;

	/* Now calculate the Lattitude. */
	Result->Lat = First->Lat + ((First->Next->Lat - First->Lat) * Scale);

	/* And the longitude. */
	Result->Long = First->Long + ((First->Next->Long - First->Long) * Scale);

	/* And the elevation. If elevation wasn't set, it should be zero.
	 * Which works quite fine for us. */
	Result->Elev = First->Elev + ((First->Next->Elev - First->Elev) * Scale);

	/* The time is not interpolated, but matches photo. */
	Result->Time = PhotoTime;

	/* And that should have fixed us... */

}
