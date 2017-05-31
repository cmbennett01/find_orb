#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include "watdefs.h"
#include "date.h"
#include "norad.h"
#include "lsquare.h"
#include "afuncs.h"

#define AU_IN_KM 1.495978707e+8
#define AU_IN_METERS (AU_IN_KM * 1000.)
#define PI 3.1415926535897932384626433832795028841971693993751058209749445923078

int vector_to_tle( tle_t *tle, const double *state_vect, const double epoch);

int verbose = 0;
int use_eight = 0, params_to_set = N_SAT_PARAMS;

#define EPHEM_TYPE_DEFAULT    '0'
#define EPHEM_TYPE_SGP        '1'
#define EPHEM_TYPE_SGP4       '2'
#define EPHEM_TYPE_SDP4       '3'
#define EPHEM_TYPE_SGP8       '4'
#define EPHEM_TYPE_SDP8       '5'
#define EPHEM_TYPE_HIGH       'h'

static int get_sxpx( const int ephem, const tle_t *tle, double *state,
                                 const double t_since_minutes)
{
   int rval = 0;

   if( tle->ephemeris_type != EPHEM_TYPE_HIGH &&
                ( tle->eo < 0. || tle->eo >= 1. || tle->xno < 0.))
      rval = -1;
   else
      {
      double params[N_SAT_PARAMS];
      int i, sxpx_rval;

      memset( params, 0, sizeof( params));
      if( ephem)
         {
         if( use_eight)
            {
            SDP8_init( params, tle);
            sxpx_rval = SDP8( t_since_minutes, tle, params, state, state + 3);
            }
         else
            {
            SDP4_init( params, tle);
            sxpx_rval = SDP4( t_since_minutes, tle, params, state, state + 3);
            }
         }
      else
         {
         SGP4_init( params, tle);
         sxpx_rval = SGP4( t_since_minutes, tle, params, state, state + 3);
         }
      if( sxpx_rval && verbose)
         {
         char buff[300];

         write_elements_in_tle_format( buff, tle);
         printf( "SXPX error: ephem %d, rval %d; e %f; tsince %f\n%s\n",
                         ephem, sxpx_rval, tle->eo, t_since_minutes, buff);
         }
      for( i = 0; i < 3; i++)
         state[i] /= AU_IN_KM;
      for( i = 3; i < 6; i++)
         state[i] *= minutes_per_day / AU_IN_KM;
      }
   return( rval);
}

#define MIN_DELTA_SQUARED 1e-22

/* A surprisingly decent way to get a TLE from a state vector is this:
Compute 'plain old Keplerian elements' from the state vector,  the
sort you would normally compute to model two-body motion,  as if you'd
never heard of TLEs or the SGP4/SDP4 orbital model.  Then,  use those
elements in a TLE and compute the corresponding state vector at epoch.

   The mismatch between two-body motion and the SGP4/SDP4 model means
that the result won't quite match the input.  However,  it'll (usually)
be fairly close,  and (usually) if you push the difference back into
the input state vector and iterate,  it will (usually) converge.

   Since it doesn't _always_ converge,  we keep track of the "best"
result (the one with the lowest root-mean-square difference from the
desired state vector).  That will usually be the last vector we compute,
but divergence happens.

   And,  of course,  the result is our best fit to the input state vector,
so we have something that may be a lovely fit to the position/velocity
at that particular epoch,  but which isn't at all good for any other
time.  Which is why the result is used only as the starting point for
a least-squares fit to the positions in an ephemeris covering the
time span of interest.  */

bool adjust_to_apogee = false;

static int iterated_vector_to_tle( tle_t *tle, const double *state_vect,
                           const double jd)
{
   int i, ephem = -1, iter = 0;
   double trial_state[6], delta = 1.;
   tle_t best_tle_yet;
   double best_delta_yet = 1e+20;
   double adjustment = 1.;
   const int max_iter = 70;
   int iterations_without_improvement = 0;

   memcpy( trial_state, state_vect, 6 * sizeof( double));
   while( iter++ < max_iter && iterations_without_improvement < 5)
      if( !vector_to_tle( tle, trial_state, jd))
         {
         double state_out[6];
         const double max_accepted_delta = .2;
         double scale = 1.;

         if( adjust_to_apogee)
            {
            if( tle->xmo > PI)
               tle->xmo -= PI + PI;
            printf( "Orig epoch: %f;  MA %f;  period %f days\n",
                        tle->epoch, tle->xmo * 180. / PI,
                        2. * PI / (tle->xno * minutes_per_day));
            if( tle->xmo > 0.)
               tle->epoch += (PI - tle->xmo) / (tle->xno * minutes_per_day);
            else
               tle->epoch -= (PI + tle->xmo) / (tle->xno * minutes_per_day);
            printf( "Result : %f\n", tle->epoch);
            tle->xmo = PI;
            }
         if( iter < 4)
            ephem = 0;
         else
            ephem = select_ephemeris( tle);
         get_sxpx( ephem, tle, state_out, (jd - tle->epoch) * minutes_per_day);
#ifdef DEBUGGING_CODE
         printf( "%.5f %15.10f %15.10f %15.10f  %15.10f %15.10f %15.10f\n",
               jd, state_out[0], state_out[1], state_out[2],
               state_out[3], state_out[4], state_out[5]);
         printf( "%.4f  %15.10f %15.10f %15.10f  %15.10f %15.10f %15.10f\n",
               jd, state_out[0] - state_vect[0],
               state_out[1] - state_vect[1],
               state_out[2] - state_vect[2],
               state_out[3] - state_vect[3],
               state_out[4] - state_vect[4],
               state_out[5] - state_vect[5]);
#endif
         delta = 0.;
         for( i = 0; i < 6; i++)
            {
            state_out[i] -= state_vect[i];
            delta += state_out[i] * state_out[i];
            }
         if( delta > max_accepted_delta)
            scale = sqrt( max_accepted_delta / delta);
         for( i = 0; i < 6; i++)
            trial_state[i] -= state_out[i] * scale * adjustment;
         if( iter >= 4 && best_delta_yet > delta)
            {
            best_delta_yet = delta;
            best_tle_yet = *tle;
            iterations_without_improvement = 0;
            }
         else
            iterations_without_improvement++;
         if( verbose)
            printf( "Iteration %d worked : e = %f, t_per = %f, %g; ephem %d\n", iter,
                           tle->eo, 2. * PI / (tle->xno * minutes_per_day), delta * 1e+6, ephem);
         }
      else        /* Try slowing the object down in hopes of */
         {        /* getting a correct vector : */
         if( verbose)
            printf( "Iteration %d failed : e = %f, t_per = %f\n", iter,
                           tle->eo, 2. * PI / (tle->xno * minutes_per_day));
         memcpy( trial_state, state_vect, 6 * sizeof( double));
         adjustment *= .9;
         assert( iter > 2);
         }
   *tle = best_tle_yet;
   return( ephem);
}

static void error_exit( const int exit_value)
{
   printf( "Run as vec2tle <input filename> (options)\n\n");
   printf( "Options are:\n");
   printf( "   -i(international designator)     ex: -i97034A\n");
   printf( "   -n(NORAD designator)             ex: -n31415\n");
   printf( "   -v                               Verbose mode\n");
   printf( "   -o(output filename)\n");
   printf( "   -f(freq)                         Output freq (default = 10)\n");
   printf( "   -g                               Use SGP for all orbits,  never SDP\n");
   printf( "The input file is assumed to be an ephemeris of state vectors from Find_Orb.\n");
   exit( exit_value);
}

static char *fgets_trimmed( char *buff, const size_t buffsize, FILE *ifile)
{
   char *rval = fgets( buff, buffsize, ifile);

   if( rval)
      {
      size_t i = 0;

      while( rval[i] != 10 && rval[i] != 13 && rval[i])
         i++;
      rval[i] = '\0';
      }
   return( rval);
}

/* The six "parameters" to be set _can_ just be,  say,  inclination,  Omega,
omega, eccentricity,  semimajor axis,  and mean anomaly.  But there are
singularities in these for low inclinations and eccentricities.  To avoid
these,  we can make the six parameters the "equinoctial" elements

params[0] = h = e sin(lon_perihelion)
params[1] = k = e cos(lon_perihelion)
params[2]=  p = tan(incl/2) * sin(lon_asc_node)
params[3]=  p = tan(incl/2) * cos(lon_asc_node)
params[4] = mean longitude = omega + Omega + mean_anomaly
params[5] = semimajor axis

   Same orbit,  expressed in a manner that avoids singularities.  Except
that because TLEs can't handle hyperbolic orbits,  we do still have
singularities for negative a or e >= 1.  There's a risk that an iteration
step will take us to,  say,  e=1.2,  something SGP4/SDP4 won't understand.
So we really want any set of six real "params" to map to a valid TLE,
with e < 1.

   To eliminate those singularities as well,  we use the log of the mean
motion of a,  and revise h and k in a somewhat odd manner as well,  to
map eccentricities between 0 and 1 to the entire (h, k) plane :

params[0] = h = e sin(lon_perihelion) / (1-e)
params[1] = k = e cos(lon_perihelion) / (1-e)
params[5] = log( mean_motion)

   With these changes,  any valid TLE will map to a set of params,  and
any set of six real values put into params[] will map to a TLE.
*/


static void set_params_from_tle( double *params, const tle_t *tle)
{
   const double lon_perih = tle->omegao + tle->xnodeo;
   const double mean_lon = lon_perih + tle->xmo;
   const double r = tle->eo / (1. - tle->eo);
   const double tan_half_incl = tan( tle->xincl * .5);

   params[0] = r * sin( lon_perih);    /* (modified) h */
   params[1] = r * cos( lon_perih);    /* (modified) k */
   params[2] = tan_half_incl * sin( tle->xnodeo);     /* p */
   params[3] = tan_half_incl * cos( tle->xnodeo);     /* q */
   params[4] = mean_lon;
   params[5] = log( tle->xno);
// params[6] = tle->bstar;
}

static double zero_to_two_pi( double ival)
{
   ival = fmod( ival, 2. * PI);
   if( ival < 0.)
      ival += 2. * PI;
   return( ival);
}

static void set_tle_from_params( tle_t *tle, const double *params)
{
   const double r = sqrt( params[0] * params[0] + params[1] * params[1]);
   const double lon_perih = atan2( params[0], params[1]);
   const double tan_half_incl =
                    sqrt( params[2] * params[2] + params[3] * params[3]);

   tle->xincl  = 2 * atan( tan_half_incl);
   tle->xnodeo = atan2( params[2], params[3]);
   tle->eo     = r / (1. + r);
   tle->omegao = lon_perih - tle->xnodeo;
   tle->xmo    = params[4] - lon_perih;
   tle->xno    = exp( params[5]);
// tle->bstar  = params[6];
   tle->xmo = zero_to_two_pi( tle->xmo);
   tle->xnodeo = zero_to_two_pi( tle->xnodeo);
   tle->omegao = zero_to_two_pi( tle->omegao);
}

static void set_simplex_value( tle_t *tle, double *simp,
                  const double *state_vect, const int ephem,
                  const unsigned n_steps, const double step_size)
{
   double err = 0.;
   unsigned j;

   set_tle_from_params( tle, simp);
   for( j = 0; j < n_steps; j++)
      {
      double state_out[6];
      size_t i;

      get_sxpx( ephem, tle, state_out, (double)(int)(j - n_steps / 2) * step_size);
      for( i = 0; i < (n_steps > 1 ? 3 : 6); i++)
         {
         const double delta = state_out[i] - state_vect[i];

         err += delta * delta;
         }
      state_vect += 6;
      }
   simp[6] = err;
}

/* Tries a simplex 'extrapolation'.  If the result is an improvement,
the sixth ("high") point is replaced.   */

#define MAX_PARAMS 10

static double try_new_simplex( tle_t *tle, double simp[MAX_PARAMS][MAX_PARAMS],
             const double *state_vect, const double extrap, const int ephem,
                        const unsigned n_steps, const double step_size)
{
   size_t i, j;
   const double frac = (1. - extrap) / 6.;
   double new_simplex[MAX_PARAMS];

   for( i = 0; i < 6; i++)
      new_simplex[i] = extrap * simp[6][i];
   for( i = 0; i < 6; i++)
      for( j = 0; j < 6; j++)
         new_simplex[i] += frac * simp[j][i];
   set_simplex_value( tle, new_simplex, state_vect, ephem, n_steps, step_size);
   if( new_simplex[6] < simp[6][6])   /* it's a "better" (lower) point */
      memcpy( simp[6], new_simplex, 7 * sizeof( double));
   return( new_simplex[6]);
}

static void sort_simplices( double simp[MAX_PARAMS][MAX_PARAMS])
{
   size_t i = 0;

   while( i < 6)     /* sort simplices,  low to high */
      if( simp[i][6] > simp[i + 1][6])
         {
         double swap_array[MAX_PARAMS];

         memcpy( swap_array, simp[i], 7 * sizeof( double));
         memcpy( simp[i], simp[i + 1], 7 * sizeof( double));
         memcpy( simp[i + 1], swap_array, 7 * sizeof( double));
         if( i)
            i--;
         }
      else
         i++;
}

int simplex_search( tle_t *tle, const double *starting_params,
                        const double *state_vect, const int ephem,
                        const unsigned n_steps, const double step_size)
{
   double simp[MAX_PARAMS][MAX_PARAMS];
   size_t i, j, iter;
   const size_t max_iter = 3000;
   bool done = false;

   for( i = 0; i < 7; i++)
      {
      const double delta = .1;

      memcpy( simp[i], starting_params, 6 * sizeof( double));
      if( i == 1 || i == 2)      /* terms involving eccentricity; */
         simp[i][i - 1] *= 1. - delta;    /* nudge to smaller e */
      if( i)
         simp[i][i - 1] += delta;
      set_simplex_value( tle, simp[i], state_vect, ephem, n_steps, step_size);
      }
   for( iter = 0; !done && iter < max_iter; iter++)
      {
      double new_score, orig_score;

      sort_simplices( simp);
      orig_score = simp[6][6];
      if( orig_score / simp[0][6] < 1.00001 || simp[0][6] < MIN_DELTA_SQUARED)
         done = true;
      new_score = try_new_simplex( tle, simp, state_vect, -1., ephem,
                                                      n_steps, step_size);
      if( new_score < simp[0][6])     /* best point so far;  try an extrap */
         {
         try_new_simplex( tle, simp, state_vect, 2., ephem,
                        n_steps, step_size);  /* try expansion */
         }
      else if( new_score >= simp[5][6])
         {
         const double fraction = (new_score < orig_score ? .5 : -.5);

         if( try_new_simplex( tle, simp, state_vect, fraction, ephem,
                        n_steps, step_size) > simp[5][6])
            {
//          printf( "Contract (iter %d)\n", (int)iter);
            for( i = 1; i < 7; i++)
               {              /* contract around lowest point */
               for( j = 0; j < 6; j++)
                  simp[i][j] = (simp[i][j] + simp[0][j]) / 2.;
               set_simplex_value( tle, simp[i], state_vect, ephem, n_steps, step_size);
               }
            }
         }
      }
   set_tle_from_params( tle, simp[0]);
   return( 0);
}

/* NOTE:  this precesses input J2000 state vectors to mean equator/ecliptic
of date.  I _think_ that's right,  but it's possible that nutation should be
included as well,  and even possible that SxPx assumes true orientation of
date:  i.e.,  the full set of earth orientation parameters,  including
proper motions and offsets from the IAU nutation theories,  ought to be used.
*/

#define N_HIST_BINS 10

int main( const int argc, const char **argv)
{
   FILE *ifile = fopen( "vec2tle.txt", "rb");
   FILE *ofile = stdout;
   int i, j, count = 0, output_freq = 10, line = 0;
   int tles_written = 0;
   int n_params = 6, n_iterations = 15;
   const int max_n_params = 8;
   char buff[200], obj_name[100];
   const char *default_intl_desig = "00000", *norad_desig = "99999";
   const char *intl_desig = default_intl_desig;
   double *slopes = (double *)calloc( max_n_params * 6, sizeof( double));
   double *vectors, worst_resid_in_run = 0., worst_mjd = 0.;
   double tdt = 0.;
   int ephem;
   tle_t tle;
   const time_t t0 = time( NULL);
   double step;
   unsigned n_steps, total_lines;
   int histo_counts[N_HIST_BINS];
   static int histo_divs[N_HIST_BINS] = { 1, 3, 10, 30, 100, 300, 1000, 3000, 10000, 30000 };
   double levenberg_marquardt_lambda0 = 0.;
   int n_damped = 0;

   if( argc < 2)
      error_exit( -1);

   setvbuf( stdout, NULL, _IONBF, 0);
   memset( &tle, 0, sizeof( tle_t));
   tle.classification = 'U';
   tle.ephemeris_type = EPHEM_TYPE_DEFAULT;
   for( i = 1; i < argc; i++)
      if( argv[i][0] == '-')
         switch( argv[i][1])
            {
            case 'a': case 'A':
               adjust_to_apogee = true;
               break;
            case 'v': case 'V':
               verbose = 1 + atoi( argv[i] + 2);
               break;
            case '7':
               n_params = 7;        /* fit bstar,  too */
            case '8':
               use_eight = 1;
               break;
            case 'p': case 'P':
               params_to_set = atoi( argv[i] + 2);
               break;
            case 'o': case 'O':
               {
               const char *output_filename = argv[i] + 2;

               if( !*output_filename && i < argc - 1)
                  output_filename = argv[i + 1];
               ofile = fopen( output_filename, "wb");
               printf( "Output directed to %s\n", output_filename);
               if( !ofile)
                  {
                  perror( "Output not opened");
                  return( -1);
                  }
               }
               break;
            case 'f': case 'F':
               output_freq = atoi( argv[i] + 2);
               break;
            case 'n': case 'N':
               norad_desig = argv[i] + 2;
               break;
            case 'i': case 'I':
               intl_desig = argv[i] + 2;
               break;
            case 'l': case 'L':
               sscanf( argv[i] + 2, "%lf,%d",
                  &levenberg_marquardt_lambda0, &n_damped);
            case 'r':
               srand( atoi( argv[i] + 2));
               break;
            case 'z':
               n_iterations = atoi( argv[i] + 2);
               break;
            case 'g':
               tle.ephemeris_type = EPHEM_TYPE_SGP4;       /* force use of SGP4 */
               break;
            case 'h':
               tle.ephemeris_type = EPHEM_TYPE_HIGH;
               break;
            default:
               printf( "'%s' is not a valid command line option\n", argv[i]);
               error_exit( -2);
            }

   vectors = (double *)calloc( 6 * output_freq, sizeof( double));
   tle.norad_number = atoi( norad_desig);
   strcpy( tle.intl_desig, intl_desig);
   if( !ifile)
      {
      printf( "vec2tle.txt not found\n");
      error_exit( -4);
      }
   fprintf( ofile, "# Made by vec2tle, compiled " __DATE__ " " __TIME__ "\n");
   fprintf( ofile, "# Run at %s#\n", ctime( &t0));
   while( fgets_trimmed( buff, sizeof( buff), ifile))
      fprintf( ofile, "%s\n", buff);
   fclose( ifile);
   ifile = fopen( argv[1], "rb");
   if( !ifile)
      {
      printf( "%s not found\n", argv[1]);
      error_exit( -3);
      }
   if( fgets_trimmed( buff, sizeof( buff), ifile))
      {
      bool writing_data = false;
      double mjdt;

      sscanf( buff, "%lf %lf %u\n", &tdt, &step, &total_lines);
      mjdt = tdt - 2400000.5;
      fprintf( ofile, "# Ephem range: %f %f %f\n",
            mjdt, mjdt + step * (double)total_lines, step * (double)output_freq);
      while( fgets_trimmed( buff, sizeof( buff), ifile))
         {
         if( !memcmp( buff, "Created ", 8))
            writing_data = true;
         if( writing_data && *buff != '#')
            fprintf( ofile, "# %s\n", buff);
         if( !memcmp( buff, "Orbital elements: ", 18))
            {
            char *tptr;

            strcpy( obj_name, buff + 19);
            printf( "Object: %s\n", obj_name);
            if( tle.norad_number == 99999)
               {
               tptr = strstr( obj_name, "NORAD ");
               if( tptr)
                  tle.norad_number = atoi( tptr + 6);
               }
            if( intl_desig == default_intl_desig)
               for( tptr = obj_name; *tptr; tptr++)
                  if( atoi( tptr) > 1900 && tptr[4] == '-' &&
                        atoi( tptr + 5) > 0)
                     {
                     memcpy( tle.intl_desig, tptr + 2, 2);    /* get year */
                     memcpy( tle.intl_desig + 2, tptr + 5, 4); /* launch # */
                     tle.intl_desig[6] = '\0';
                     }
            }
         }
      }
   for( i = 0; i < N_HIST_BINS; i++)
      histo_counts[i] = 0;
   if( tle.ephemeris_type == EPHEM_TYPE_SGP4)
      {
      fprintf( ofile, "# SGP4 only: these TLEs are _not_ fitted to SDP4,  even for\n");
      fprintf( ofile, "# deep-space TLEs.  These may not work with your software.\n");
      }
   fprintf( ofile, "#\n");
   fprintf( ofile, "# 1 NoradU COSPAR   Epoch.epoch     dn/dt/2  d2n/dt2/6 BSTAR    T El# C\n");
   fprintf( ofile, "# 2 NoradU Inclina RAAscNode Eccent  ArgPeri MeanAno  MeanMotion Rev# C\n");
   fseek( ifile, 0L, SEEK_SET);
   if( !fgets( buff, sizeof( buff), ifile))
      {
      printf( "Couldn't re-read the header\n");
      return( -1);
      }
   n_steps = total_lines / output_freq;
   while( n_steps--)
      {
      double *sptr = vectors;
      const double jan_1956 = 2435473.5, jan_2050 = 2469807.5;
      const double jd_utc = tdt - td_minus_utc( tdt) / seconds_per_day;

      for( i = 0; i < output_freq && fgets( buff, sizeof( buff), ifile);
                                                      i++, sptr += 6)
         {
         double jdt, jd_utc;
         double precession_matrix[9], ivect[6];

         if( sscanf( buff, "%lf%lf%lf%lf%lf%lf%lf", &jdt,
                        ivect, ivect + 1, ivect + 2,
                        ivect + 3, ivect + 4, ivect + 5) != 7
                 || jdt < jan_1956 || jdt > jan_2050)
            {
            printf( "Error reading input ephem:\n%s\n", buff);
            return( -2);
            }
                     /* I don't think TLEs work outside this range: */
         assert( jdt > jan_1956 && jdt < jan_2050);
         jd_utc = jdt - td_minus_utc( jdt) / seconds_per_day;
         setup_precession( precession_matrix, 2000.,
                                      2000. + (jd_utc - 2451545.) / 365.25);
         precess_vector( precession_matrix, ivect, sptr);
         precess_vector( precession_matrix, ivect + 3, sptr + 3);
         }
      assert( i == output_freq);

      tle.epoch = jd_utc;
      if( tle.ephemeris_type == EPHEM_TYPE_HIGH)
         {
         double *svect = &tle.xincl;

         ephem = 1;
         for( i = 0; i < 3; i++)
            {
            svect[i] = vectors[i] * AU_IN_METERS;
            svect[i + 3] = vectors[i + 3] * AU_IN_METERS / seconds_per_day;
            }
         }
      else
         {
         double start_params[6];

         i = output_freq / 2;    /* use middle vector;  it improves the */
         tle.epoch += (double)i * step;      /* convergence for simplex */
         ephem = iterated_vector_to_tle( &tle, vectors + i * 6, tle.epoch);
         if( ephem != -1)
            ephem = select_ephemeris( &tle);
         if( verbose)
            printf( "   ephem selected = %d\n", ephem);
         if( ephem != -1 && tle.ephemeris_type == EPHEM_TYPE_SGP4)
            ephem = 0;
         set_params_from_tle( start_params, &tle);
         simplex_search( &tle, start_params, vectors, ephem,
                     output_freq, step * minutes_per_day);
         }

      int lsquare_rval, failure = 0, iter;
      char obuff[200];
      double worst_resid = 1e+20;
      tle_t tle_to_output = tle;

      if( verbose)
         printf( "   least-square fitting\n");
      if( ephem == -1)     /* failed from the get-go */
         failure = -1;
      for( iter = 0; iter < n_iterations && !failure; iter++)
         {
         void *lsquare = lsquare_init( n_params);
         double state0[6], params[max_n_params];
         double differences[max_n_params], rms_change = 0.;
         double this_worst_resid = 0.;
         extern double levenberg_marquardt_lambda;

         if( !iter)
            levenberg_marquardt_lambda = levenberg_marquardt_lambda0;
         else if( iter == n_damped)
            levenberg_marquardt_lambda = 0.;
         if( verbose)
            {
            write_elements_in_tle_format( obuff, &tle);
            printf( "Iter %d:\n%s\n", iter, obuff);
//          if( verbose > 1)
//             getch( );
            }
         set_params_from_tle( params, &tle);
         for( j = 0; j < output_freq; j++)
            {
            double resid2 = 0.;
//          const double time_diff_in_minutes = (double)j
//                                     * step * minutes_per_day;
            const double time_diff_in_minutes = (double)(j - output_freq / 2)
                                       * step * minutes_per_day;

            for( i = 0; i < n_params; i++)
               {
               double state1[6], state2[6];
               double delta = (i == 6 ? 1.e-5 : 1.e-4);
               int k;

               if( tle.ephemeris_type == EPHEM_TYPE_HIGH)
                  delta = (i >= 3 ? 1e-4 : 1.);    /* one meter or 10^-4 m/s */
               params[i] -= delta;
               set_tle_from_params( &tle, params);
               get_sxpx( ephem, &tle, state1, time_diff_in_minutes);
               params[i] += delta + delta;
               set_tle_from_params( &tle, params);
               get_sxpx( ephem, &tle, state2, time_diff_in_minutes);
               params[i] -= delta;
               set_tle_from_params( &tle, params);
               for( k = 0; k < 6; k++)
                  slopes[k * n_params + i] = (state2[k] - state1[k]) / (2. * delta);
               if( verbose > 2)
                  {
                  for( k = 0; k < 6; k++)
                     printf( "%10.3g ", slopes[k * n_params + i]);
                  printf( "\n");
                  }
               }
            get_sxpx( ephem, &tle, state0, time_diff_in_minutes);
            if( verbose > 1)
               printf( "JD %f: ", jd_utc);
            for( i = 0; i < 3; i++)
               {
               const double residual = vectors[j * 6 + i] - state0[i];

               if( verbose == 2)
                  printf( "%f ", residual * AU_IN_KM);
               if( verbose == 3)
                  printf( "   %f (%f %f)\n", residual * AU_IN_KM,
                              vectors[j * 6 + i] * AU_IN_KM,
                              state0[i] * AU_IN_KM);
               resid2 += residual * residual;
               lsquare_add_observation( lsquare, residual,
                        1., slopes + i * n_params);
               }
            rms_change += resid2;
            if( resid2 > this_worst_resid)
               this_worst_resid = resid2;
            if( verbose > 1)
               printf( "\n");
            }

         rms_change = sqrt( rms_change / (double)output_freq);
         this_worst_resid = sqrt( this_worst_resid) * AU_IN_KM;
         if( verbose)
            printf( "Change = %f; worst = %f;  bstar %f\n",
                           rms_change * AU_IN_KM,
                           this_worst_resid, tle.bstar);
         lsquare_rval = lsquare_solve( lsquare, differences);
         lsquare_free( lsquare);
         if( lsquare_rval)
            {
            printf( "ERROR %d in lsquare soln: MJD %f\n",
                           lsquare_rval, tdt - 2400000.5);
            failure = 1;
            }
         else if( tle.ephemeris_type == EPHEM_TYPE_HIGH)
            {
            for( i = 0; i < n_params; i++)
               params[i] += differences[i];
            set_tle_from_params( &tle, params);
            }
         else
            {
            rms_change = 0.;
            for( i = 0; i < 6; i++)
               rms_change += differences[i] * differences[i];
            rms_change = sqrt( rms_change);
            for( i = 0; i < n_params; i++)
               params[i] += differences[i];
            set_tle_from_params( &tle, params);
            if( verbose)
               printf( "  change in TLE = %f\n", rms_change);
            }
         if( !iter || (!failure && this_worst_resid < worst_resid))
            {     /* this is 'our best TLE yet' */
            tle_to_output = tle;
            worst_resid = this_worst_resid;
            }
         }

      full_ctime( buff, tdt,
                    FULL_CTIME_YMD | FULL_CTIME_FORMAT_HH_MM);
//    if( !failure)
         {
//       if( tle.ephemeris_type != EPHEM_TYPE_HIGH)
            fprintf( ofile, "\n# Worst residual: %.2f km\n",
                          worst_resid);
//       else
//          fprintf( ofile, "\n");
         write_elements_in_tle_format( obuff, &tle_to_output);
         if( verbose)
            {
            double params[N_SAT_PARAMS], state[6];

            SDP4_init( params, &tle_to_output);
            SDP4( 0, &tle_to_output, params, state, state + 3);
            printf( "   Node: %f\n", params[25] * 180. / PI);
            printf( "   xinc: %f\n", params[27] * 180. / PI);
            printf( "   em:   %f\n", params[26]);
            printf( "%s", obuff);
            }
         fprintf( ofile, "# MJD %f (%s)\n", tdt - 2400000.5, buff);
         if( *obj_name)
            fprintf( ofile, "%s\n", obj_name);
         fprintf( ofile, "%s", obuff);
         if( worst_resid_in_run < worst_resid)
            {
            worst_resid_in_run = worst_resid;
            worst_mjd = tdt - 2400000.5;
            }
         i = 0;
         while( i < N_HIST_BINS - 1 && worst_resid > (double)histo_divs[i])
            i++;
         histo_counts[i]++;
         }
//    else
//       fprintf( ofile, "FAILED (%d) for JD %.2f = %s\n", failure,
//                      jd_utc, buff);
      count = -1;
      tles_written++;
      count++;
      line++;
      if( ofile != stdout && !(line % 50))
         {
         printf( "Line %d of %u (%u%% done): %d written, JD %f\r",
                  line, total_lines, line * 100 * output_freq / total_lines,
                  tles_written, tdt);
         fflush( stdout);
         }
      tdt += step * (double)output_freq;
      }
   if( ifile)
      fclose( ifile);
   while( 1)
      {
      fprintf( ofile, "Worst residual in entire run: %.2f km on MJD %.1f\n",
                                   worst_resid_in_run, worst_mjd);
      fprintf( ofile, "       ");
      for( i = 0; i < N_HIST_BINS - 2; i++)
         fprintf( ofile, "%-6d", histo_divs[i]);
      fprintf( ofile, "km\n");
      for( i = 0; i < N_HIST_BINS - 1; i++)
         fprintf( ofile, "%6d", histo_counts[i]);
      fprintf( ofile, "\n");
      if( ofile != stdout)
         {
         fclose( ofile);
         ofile = stdout;
         }
      else
         break;
      }
   printf( "Freeing vectors\n");
   free( vectors);
   free( slopes);
   printf( "All done\n");
   return( 0);
}

