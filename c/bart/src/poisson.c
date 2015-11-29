/* Copyright 2014. The Regents of the University of California.
 * All rights reserved. Use of this source code is governed by 
 * a BSD-style license which can be found in the LICENSE file.
 
 * 2013 Martin Uecker 
 * uecker@eecs.berkeley.edu
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <math.h>
#include <complex.h>
#include <stdbool.h>
#include <assert.h>

#include "num/multind.h"
#include "num/flpmath.h"
#include "num/rand.h"

#include "misc/misc.h"
#include "misc/mmio.h"
#include "misc/pd.h"


static void random_point(int D, float p[D])
{
	for (int i = 0; i < D; i++)
		p[i] = uniform_rand();
}

static float dist(int D, const float a[D], const float b[D])
{
	float r = 0.;

	for (int i = 0; i < D; i++)
		r += powf(a[i] - b[i], 2.);

	return sqrtf(r);
}


static float maxn(int D, const float a[D], const float b[D])
{
	float r = 0.;

	for (int i = 0; i < D; i++)
		r = MAX(fabsf(a[i] - b[i]), r);

	return r;
}



static void usage(const char* name, FILE* fp)
{
	fprintf(fp, "Usage: %s [-Y/Z dim] [-y/z acc] [-v] [-e] [-C center] <outfile>\n", name);
}

static void help(void)
{
	printf( "\n"
		"Computes Poisson-disc sampling pattern.\n"
		"\n"
		"-Y\tsize dimension 1 (phase 1)\n"
		"-Z\tsize dimension 2 (phase 2)\n"
		"-y\tacceleration (dim 1)\n"
		"-z\tacceleration (dim 2)\n"
		"-C\tsize of calibration region\n"
		"-v\tvariable density\n"
		"-e\telliptical scanning\n"
		"-h\thelp\n");
}



int main_poisson(int argc, char* argv[])
{
	int yy = 128;
	int zz = 128;
	bool cutcorners = false;
	float vardensity = 0.;
	int T = 1;
	int rnd = 0;
	bool msk = true;
	int points = -1;
	float mindist = 1. / 1.275;
	float yscale = 1.;
	float zscale = 1.;
	unsigned int calreg = 0;

	int c;
	while (-1 != (c = getopt(argc, argv, "Y:Z:hvV:eR:D:my:y:z:T:C:"))) {

		switch (c) {
		case 'Y':
			yy = atoi(optarg);
			break;

		case 'Z':
			zz = atoi(optarg);
			break;

		case 'h':
			usage(argv[0], stdout);
			help();
			exit(0);

		case 'v':
			vardensity = 20;
			break;

		case 'V':
			vardensity = atof(optarg);
			break;

		case 'T':
#ifdef BERKELEY_SVN
			T = atoi(optarg);
#else
			assert(0);
#endif
			break;	

		case 'z':
			zscale = atof(optarg);
			break;

		case 'y':
			yscale = atof(optarg);
			break;

		case 'e':
			cutcorners = true;
			break;

		case 'D':
			mindist = atof(optarg);
			break;

		case 'C':
			calreg = atoi(optarg);
			break;

		case 'R':
			rnd = 1;
			points = atoi(optarg);	
			break;

		case 'm':
			msk = false;
			break;

		default:
			exit(1);
		}
	}


	if (argc - optind != 1) {

		usage(argv[0], stderr);
		exit(1);
	}

	assert((yscale >= 1.) && (zscale >= 1.));

	// compute mindest and scaling

	float kspext = MAX(yy, zz);

	int Pest = T * (int)(1.2 * powf(kspext, 2.) / (yscale * zscale));

	mindist /= kspext;
	yscale *= (float)kspext / (float)yy;
	zscale *= (float)kspext / (float)zz;

	if (vardensity != 0.) {

		// TODO
	}


	long dims[5] = { 1, yy, zz, T, 1 };
	complex float* mask = NULL;

	if (msk) {
		
		mask = create_cfl(argv[optind], 5, dims);
		md_clear(5, dims, mask, sizeof(complex float));
	}

	int M = rnd ? (points + 1) : Pest;
	int P;
	
	while (true) {

		float (*points)[2] = xmalloc(M * sizeof(float[3]));

		int* kind = xmalloc(M * sizeof(int));
		kind[0] = 0;

		if (!rnd) {

			points[0][0] = 0.5;
			points[0][1] = 0.5;

			if (1 == T) {

				P = poissondisc(2, M, 1, vardensity, mindist, points);

			} else {

				float (*delta)[T] = xmalloc(T * T * sizeof(complex float));
				float dd[T];
				for (int i = 0; i < T; i++)
					dd[i] = mindist;

				mc_poisson_rmatrix(2, T, delta, dd);
				P = poissondisc_mc(2, T, M, 1, vardensity, (const float (*)[T])delta, points, kind);
			}

		} else { // random pattern

			P = M - 1;
			for (int i = 0; i < P; i++)
				random_point(2, points[i]);
		}

		if (P < M) {

			for (int i = 0; i < P; i++) {

				points[i][0] = (points[i][0] - 0.5) * yscale + 0.5;
				points[i][1] = (points[i][1] - 0.5) * zscale + 0.5;
			}

			// throw away points outside 
	
			float center[2] = { 0.5, 0.5 };

			int j = 0;
			for (int i = 0; i < P; i++) {

				if ((cutcorners ? dist : maxn)(2, center, points[i]) <= 0.5) {

					points[j][0] = points[i][0];
					points[j][1] = points[i][1];
					j++;
				}
			}

			P = j;


			if (msk) {

				// rethink module here
				for (int i = 0; i < P; i++) {

					int yy = (int)floorf(points[i][0] * dims[1]);
					int zz = (int)floorf(points[i][1] * dims[2]);

					if ((yy < 0) || (yy >= dims[1]) || (zz < 0) || (zz >= dims[2]))
						continue;

					if (1 == T)
					mask[zz * dims[1] + yy] = 1.;//cexpf(2.i * M_PI * (float)kind[i] / (float)T);
					else
					mask[(kind[i] * dims[2] + zz) * dims[1] + yy] = 1.;//cexpf(2.i * M_PI * (float)kind[i] / (float)T);
				}

			} else {

#if 1
				long sdims[2] = { 3, P };
				complex float* samples = create_cfl(argv[optind], 2, sdims);
				for (int i = 0; i < P; i++) {

					samples[3 * i + 0] = 0.;
					samples[3 * i + 1] = (points[i][0] - 0.5) * dims[1];
					samples[3 * i + 2] = (points[i][1] - 0.5) * dims[2];
					//	printf("%f %f\n", creal(samples[3 * i + 0]), creal(samples[3 * i + 1]));
				}
				unmap_cfl(2, sdims, (void*)samples);
#endif
			}

			break;
		}

		// repeat with more points
		M *= 2;
		free(points);
		free(kind);
	}

	// calibration region

	assert((mask != NULL) || (0 == calreg));
	assert((calreg <= dims[1]) && (calreg <= dims[2]));

	for (unsigned int i = 0; i < calreg; i++) {
		for (unsigned int j = 0; j < calreg; j++) {

			int y = (dims[1] - calreg) / 2 + i;
			int z = (dims[2] - calreg) / 2 + j;

			for (int k = 0; k < T; k++) {

				if (0. == mask[(k * dims[2] + z) * dims[1] + y]) {

					mask[(k * dims[2] + z) * dims[1] + y] = 1.;
					P++;
				}
			}
		}
	}


	printf("points: %d", P);

	if (1 != T)
		printf(", classes: %d", T);

	if (NULL != mask) {

		float f = cutcorners ? (M_PI / 4.) : 1.;
		printf(", grid size: %ldx%ld%s = %ld (R = %f)", dims[1], dims[2], cutcorners ? "x(pi/4)" : "",
				(long)(f * dims[1] * dims[2]), f * T * dims[1] * dims[2] / (float)P);

		unmap_cfl(5, dims, (void*)mask);
	}

	printf("\n");
	exit(0);
}



