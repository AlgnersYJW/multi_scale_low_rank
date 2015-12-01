/* Copyright 2013-2015. The Regents of the University of California.
 * All rights reserved. Use of this source code is governed by 
 * a BSD-style license which can be found in the LICENSE file.
 *
 * Authors:
 * 2012-2015 Martin Uecker <uecker@eecs.berkeley.edu>
 * 2013 Dara Bahri <dbahri123@gmail.com>
 * 2015 Siddharth Iyer <sid8795@gmail.com>
 *
 *
 * Uecker M, Lai P, Murphy MJ, Virtue P, Elad M, Pauly JM, Vasanawala SS, Lustig M.
 * ESPIRiT - An Eigenvalue Approach to  Autocalibrating Parallel MRI: Where SENSE 
 * meets GRAPPA. Magn Reson Med, 71:990-1001 (2014)
 *
 */

#include <assert.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>

#include "num/multind.h"
#include "num/fft.h"
#include "num/flpmath.h"
#include "num/la.h"
#include "num/lapack.h"
#include "num/casorati.h"
#include "num/rand.h"

#include "misc/misc.h"
#include "misc/mri.h"
#include "misc/resize.h"
#include "misc/debug.h"
#include "misc/utils.h"

#include "calib/calmat.h"
#include "calib/cc.h"
#include "calib/softweight.h"

#include "calib.h"

#ifdef USE_CUDA
#include "calib/calibcu.h"
#endif

#if 0
#define CALMAT_SVD
#endif

#if 0
#define FLIP
#endif


static void eigen_herm3(int M, int N, float val[M], complex float matrix[N][N]) // ordering might be different to herm2
{
	complex float mout[M][N];

	for (int li = 0; li < N; li++)
		for (int lj = 0; lj < li; lj++)
			matrix[lj][li] = conj(matrix[li][lj]);

	//mat_identity(M, N, mout);
	orthiter(M, N, 30, val, mout, matrix);

	for (int i = 0; i < M; i++)
		for (int j = 0; j < N; j++)
			matrix[i][j] = mout[i][j];
}







static float scurve(float x)
{
        if (x <= -1.)
                return 0.;

        if (x >= 1.)
                return 1.;

        return 0.5 * (1. + 2. * x / (1. + powf(x, 2.)));
}

static float crop_weight_function(float crth, float val)
{
	return scurve((sqrtf(val) - crth) / (1. - crth));
}

static float crop_thresh_function(float crth, float val)
{
	return (val <= crth) ? 0. : 1.;
}

typedef float (*weight_function)(float crth, float val);

static void crop_weight(const long dims[DIMS], complex float* ptr, weight_function fun, float crth, const complex float* map)
{
	long xx = dims[0];
	long yy = dims[1];
	long zz = dims[2];
	long cc = dims[3];
	long mm = dims[4];

	assert(DIMS >= 5);
	assert(1 == md_calc_size(DIMS - 5, dims + 5));



	for (long m = 0; m < mm; m++) {
#pragma omp parallel for
		for (long k = 0; k < zz; k++) {
			for (long i = 0; i < yy; i++) {
				for (long j = 0; j < xx; j++) {

					float val = cabsf(map[((m * zz + k) * yy + i) * xx + j]);

					for (long c = 0; c < cc; c++)
						ptr[(((m * cc + c) * zz + k) * yy + i) * xx + j] *= fun(crth, val);
				}
			}
		}
	}
}


void crop_sens(const long dims[DIMS], complex float* ptr, bool soft, float crth, const complex float* map)
{
	crop_weight(dims, ptr, soft ? crop_weight_function : crop_thresh_function, crth, map);
}







void calone(const struct ecalib_conf* conf, const long cov_dims[4], complex float* imgcov, unsigned int SN, float svals[SN], const long calreg_dims[DIMS], const complex float* data)
{
	assert(1 == md_calc_size(DIMS - 5, calreg_dims + 5));

#if 1
	long nskerns_dims[5];
	complex float* nskerns;
	compute_kernels(conf, nskerns_dims, &nskerns, SN, svals, calreg_dims, data);
#else
	long channels = calreg_dims[3];

	long kx = conf->kdims[0];
	long ky = conf->kdims[1];
	long kz = conf->kdims[2];

	long nskerns_dims[5] = { kx, ky, kz, channels, 0 };
	long N = md_calc_size(4, nskerns_dims);

	assert(N > 0);
	nskerns_dims[4] = N;

	complex float* nskerns = md_alloc(5, nskerns_dims, CFL_SIZE);

	long nr_kernels = channels;
	nskerns_dims[4] = channels;
	spirit_kernel(nskerns_dims, nskerns, calreg_dims, data);
#endif

	compute_imgcov(cov_dims, imgcov, nskerns_dims, nskerns);

	md_free(nskerns);
}








/* calculate point-wise maps 
 *
 */
void eigenmaps(const long out_dims[DIMS], complex float* optr, complex float* eptr, const complex float* imgcov2, const long msk_dims[3], const bool* msk, bool orthiter, bool ecal_usegpu)
{
#ifdef USE_CUDA
	if (ecal_usegpu) {

		//FIXME cuda version should be able to return sensitivities for a subset of image-space points
		assert(!msk);
		eigenmapscu(out_dims, optr, eptr, imgcov2);
		return;
	}
#else
	assert(!ecal_usegpu);
#endif

	long channels = out_dims[3];
	long maps = out_dims[4];

	assert(DIMS >= 5);
	assert(1 == md_calc_size(DIMS - 5, out_dims + 5));
	assert(maps <= channels);

	long xx = out_dims[0];
	long yy = out_dims[1];
	long zz = out_dims[2];

	float scale = 1.; // for some reason, not

	if (msk_dims) {

		assert(msk_dims[0] == xx);
		assert(msk_dims[1] == yy);
		assert(msk_dims[2] == zz);
	}

	md_clear(5, out_dims, optr, CFL_SIZE);

#pragma omp parallel for collapse(3)
	for (long k = 0; k < zz; k++) {
		for (long j = 0; j < yy; j++) {
			for (long i = 0; i < xx; i++) {

				if (!msk || msk[i + xx * (j + yy * k)])	{

					float val[channels];
					complex float cov[channels][channels];

					complex float tmp[channels * (channels + 1) / 2];

					for (long l = 0; l < channels * (channels + 1) / 2; l++)
						tmp[l] = imgcov2[((l * zz + k) * yy + j) * xx + i] / scale;

					unpack_tri_matrix(channels, cov, tmp);

					if (orthiter) 
						eigen_herm3(maps, channels, val, cov);
					else 
						lapack_eig(channels, val, cov);

					for (long u = 0; u < maps; u++) {

						long ru = (orthiter ? maps : channels) - 1 - u;

						for (long v = 0; v < channels; v++) 
							optr[((((u * channels + v) * zz + k) * yy + j) * xx + i)] = cov[ru][v];

						if (NULL != eptr)
							eptr[((u * zz + k) * yy + j) * xx + i] = val[ru];
					}
				}
			}
		}
	}
}









void caltwo(const struct ecalib_conf* conf, const long out_dims[DIMS], complex float* out_data, complex float* emaps, const long in_dims[4], complex float* in_data, const long msk_dims[3], const bool* msk)
{

	long xx = out_dims[0];
	long yy = out_dims[1];
	long zz = out_dims[2];

	long xh = in_dims[0];
	long yh = in_dims[1];
	long zh = in_dims[2];

	long channels = out_dims[3];
	long cosize = channels * (channels + 1) / 2;
	
	assert(DIMS >= 5);
	assert(1 == md_calc_size(DIMS - 5, out_dims + 5));
	assert(in_dims[3] == cosize);

	long cov_dims[4] = { xh, yh, zh, cosize };
	long covbig_dims[4] = { xx, yy, zz, cosize };

	assert(((xx == 1) && (xh == 1)) || (xx >= xh));
	assert(((yy == 1) && (yh == 1)) || (yy >= yh));
	assert(((zz == 1) && (zh == 1)) || (zz >= zh));

	assert((1 == xh) || (0 == xh % 2));
	assert((1 == yh) || (0 == yh % 2));
	assert((1 == zh) || (0 == zh % 2));

	complex float* imgcov2 = md_alloc(4, covbig_dims, CFL_SIZE);

	debug_printf(DP_DEBUG1, "Resize...\n");

	sinc_zeropad(4, covbig_dims, imgcov2, cov_dims, in_data);

	debug_printf(DP_DEBUG1, "Point-wise eigen-decomposition...\n");

	eigenmaps(out_dims, out_data, emaps, imgcov2, msk_dims, msk, conf->orthiter, conf->usegpu);

	md_free(imgcov2);
}




void calone_dims(const struct ecalib_conf* conf, long cov_dims[4], long channels)
{
	long kx = conf->kdims[0];
	long ky = conf->kdims[1];
	long kz = conf->kdims[2];

	cov_dims[0] = (1 == kx) ? 1 : (2 * kx);
	cov_dims[1] = (1 == ky) ? 1 : (2 * ky);
	cov_dims[2] = (1 == kz) ? 1 : (2 * kz);
	cov_dims[3] = channels * (channels + 1) / 2;
}



const struct ecalib_conf ecalib_defaults = { { 6, 6, 6 }, 0.001, -1, -1., false, false, 0.8, true, false, -1., false, true };






void calib2(const struct ecalib_conf* conf, const long out_dims[DIMS], complex float* out_data, complex float* eptr, unsigned int SN, float svals[SN], const long calreg_dims[DIMS], const complex float* data, const long msk_dims[3], const bool* msk)
{
	long channels = calreg_dims[3];
	long maps = out_dims[4];

	assert(calreg_dims[3] == out_dims[3]);
	assert(maps <= channels);

	assert(1 == md_calc_size(DIMS - 5, out_dims + 5));
	assert(1 == md_calc_size(DIMS - 5, calreg_dims + 5));

	complex float rot[channels][channels];

	if (conf->rotphase) {

		long scc_dims[DIMS] = MD_INIT_ARRAY(DIMS, 1);
		scc_dims[COIL_DIM] = channels;
		scc_dims[MAPS_DIM] = channels;
		scc(scc_dims, &rot[0][0], calreg_dims, data);

	} else {

		for (unsigned int i = 0; i < channels; i++)
			for (unsigned int j = 0; j < channels; j++)
				rot[i][j] = (i == j) ? 1. : 0.;
	}


	long cov_dims[4];

	calone_dims(conf, cov_dims, channels);

	complex float* imgcov = md_alloc(4, cov_dims, CFL_SIZE);

	calone(conf, cov_dims, imgcov, SN, svals, calreg_dims, data);

	caltwo(conf, out_dims, out_data, eptr, cov_dims, imgcov, msk_dims, msk);

	/* Intensity and phase normalization similar as proposed
	 * for adaptive combine (Walsh's method) in
	 * Griswold et al., ISMRM 10:2410 (2002)
	 */

	if (conf->intensity) {

		debug_printf(DP_DEBUG1, "Normalize...\n");

		/* I think the reason this works is because inhomogeneity usually
		 * comes from only a few coil elements which are close. The l1-norm
		 * is more resilient against such outliers. -- Martin
		 */

		normalizel1(DIMS, COIL_FLAG, out_dims, out_data);
		md_zsmul(DIMS, out_dims, out_data, out_data, sqrtf((float)channels));
	}

	debug_printf(DP_DEBUG1, "Crop maps... (%.2f)\n", conf->crop);

	crop_sens(out_dims, out_data, conf->softcrop, conf->crop, eptr);

	debug_printf(DP_DEBUG1, "Fix phase...\n");


	// rotate the the phase with respect to the first principle component
	fixphase2(DIMS, out_dims, COIL_DIM, rot[0], out_data, out_data);

	md_free(imgcov);
}



void calib(const struct ecalib_conf* conf, const long out_dims[DIMS], complex float* out_data, complex float* eptr, unsigned int SN, float svals[SN], const long calreg_dims[DIMS], const complex float* data)
{
	calib2(conf, out_dims, out_data, eptr, SN, svals, calreg_dims, data, NULL, NULL);
}




static void perturb(const long dims[2], complex float* vecs, float amt)
{
	complex float* noise = md_alloc(2, dims, CFL_SIZE);

	md_gaussian_rand(2, dims, noise);

	for (long j = 0; j < dims[1]; j++) {

		float nrm = md_znorm(1, dims, noise + j * dims[0]);
		complex float val = amt / nrm;
		md_zsmul(1, dims, noise + j * dims[0], noise + j * dims[0], val);
	}

	md_zadd(2, dims, vecs, vecs, noise);

	for (long j = 0; j < dims[1]; j++) {

		float nrm = md_znorm(1, dims, vecs + j * dims[0]);
		complex float val = 1 / nrm;
		md_zsmul(1, dims, vecs + j * dims[0], vecs + j * dims[0], val);
	}

	md_free(noise);
}


static int number_of_kernels(const struct ecalib_conf* conf, unsigned int N, const float val[N])
{
	unsigned int n = 0;
	if (-1 != conf->numsv) {

		n = conf->numsv;
		assert(-1. == conf->percentsv);
		assert(-1. == conf->threshold);

	} else if (conf->percentsv != -1.) {

		n = (unsigned int)(N * conf->percentsv / 100.);
		assert(-1 == conf->numsv);
		assert(-1. == conf->threshold);

	} else {

		assert(-1 == conf->numsv);
		assert(-1. == conf->percentsv);

		for (unsigned int i = 0; i < N; i++) {

			if (val[i] / val[0] > sqrtf(conf->threshold))
				n++;
		}
	}

	if (val[0] <= 0.)
		error("No signal.\n");

	debug_printf(DP_DEBUG1, "Using %d/%ld kernels (%.2f%%, last SV: %f%s).\n", n, N, (float)n / (float)N * 100., (n > 0) ? (val[n - 1] / val[0]) : 1., conf->weighting ? ", weighted" : "");

	float tr = 0.;
	for (unsigned int i = 0; i < N; i++) {

		tr += powf(val[i], 2.);
		debug_printf(DP_DEBUG3, "SVALS %f (%f)\n", val[i], val[i] / val[0]);
	}

	debug_printf(DP_DEBUG3, "\nTRACE: %f (%f)\n", tr, tr / (float)N);

	assert(n <= N);
	return n;
}


void compute_kernels(const struct ecalib_conf* conf, long nskerns_dims[5], complex float** nskerns_ptr, unsigned int SN, float svals[SN], const long caldims[DIMS], const complex float* caldata)
{
	assert(1 == md_calc_size(DIMS - 5, caldims + 5));

	nskerns_dims[0] = conf->kdims[0];
	nskerns_dims[1] = conf->kdims[1];
	nskerns_dims[2] = conf->kdims[2];
	nskerns_dims[3] = caldims[3];

	long N = md_calc_size(4, nskerns_dims);

	assert(N > 0);
	nskerns_dims[4] = N;

	complex float* nskerns = md_alloc(5, nskerns_dims, CFL_SIZE);
	*nskerns_ptr = nskerns;

	complex float (*vec)[N] = xmalloc(N * N * sizeof(complex float));

	assert((NULL == svals) || (SN == N));
	float* val = (NULL != svals) ? svals : xmalloc(N * FL_SIZE);

	debug_printf(DP_DEBUG1, "Build calibration matrix and SVD...\n");

#ifdef CALMAT_SVD
	calmat_svd(conf->kdims, N, vec, val, caldims, caldata);

        if (conf->weighting)
		soft_weight_singular_vectors(N, conf->kdims, caldims, val, val);

	for (int i = 0; i < N; i++)
		for (int j = 0; j < N; j++) 
#ifndef FLIP
			nskerns[i * N + j] = (vec[j][i]) * (conf->weighting ? val[i] : 1.);
#else
			nskerns[i * N + j] = (vec[j][N - 1 - i]) * (conf->weighting ? val[N - 1 - i] : 1.);
#endif
#else
	covariance_function(conf->kdims, N, vec, caldims, caldata);

	debug_printf(DP_DEBUG1, "Eigen decomposition... (size: %ld)\n", N);

	// we could apply Nystroem method here to speed it up

	float tmp_val[N];
	lapack_eig(N, tmp_val, vec);

	// reverse and square root, test for smaller null to avoid NaNs
	for (int i = 0; i < N; i++)
		val[i] = (tmp_val[N - 1 - i] < 0.) ? 0. : sqrtf(tmp_val[N - 1 - i]);

        if (conf->weighting)
		soft_weight_singular_vectors(N, conf->kdims, caldims, val, val);

	for (int i = 0; i < N; i++)
		for (int j = 0; j < N; j++) 
#ifndef FLIP
			nskerns[i * N + j] = vec[N - 1 - i][j] * (conf->weighting ? val[i] : 1.);	// flip
#else
			nskerns[i * N + j] = vec[i][j] * (conf->weighting ? val[N - 1 - i] : 1.);	// flip
#endif
#endif

	if (conf->perturb > 0.) {

		long dims[2] = { N, N };
		perturb(dims, nskerns, conf->perturb);
	}

#ifndef FLIP
	nskerns_dims[4] = number_of_kernels(conf, N, val);
#else
	nskerns_dims[4] = N - number_of_kernels(conf, N, val);
#endif

	if (NULL == svals)
		free(val);

	free(vec);
}




	
void compute_imgcov(const long cov_dims[4], complex float* imgcov, const long nskerns_dims[5], const complex float* nskerns)
{
	debug_printf(DP_DEBUG1, "Zeropad...\n");

	long xh = cov_dims[0];
	long yh = cov_dims[1];
	long zh = cov_dims[2];

	long kx = nskerns_dims[0];
	long ky = nskerns_dims[1];
	long kz = nskerns_dims[2];

	long channels = nskerns_dims[3];
	long nr_kernels = nskerns_dims[4];

	long imgkern_dims[5] = { xh, yh, zh, channels, nr_kernels };

	complex float* imgkern1 = md_alloc(5, imgkern_dims, CFL_SIZE);
	complex float* imgkern2 = md_alloc(5, imgkern_dims, CFL_SIZE);

	md_resize_center(5, imgkern_dims, imgkern1, nskerns_dims, nskerns, CFL_SIZE);

	// resort array

	debug_printf(DP_DEBUG1, "FFT (juggling)...\n");

	long istr[5];
	long mstr[5];

	long idim[5] = { xh, yh, zh, channels, nr_kernels };
	long mdim[5] = { nr_kernels, channels, xh, yh, zh };

	md_calc_strides(5, istr, idim, CFL_SIZE);
	md_calc_strides(5, mstr, mdim, CFL_SIZE);

	long m2str[5] = { mstr[2], mstr[3], mstr[4], mstr[1], mstr[0] };

	ifftmod(5, imgkern_dims, FFT_FLAGS, imgkern1, imgkern1);
	ifft2(5, imgkern_dims, FFT_FLAGS, m2str, imgkern2, istr, imgkern1);

	float scalesq = (kx * ky * kz) * (xh * yh * zh); // second part for FFT scaling

	md_free(imgkern1);

	debug_printf(DP_DEBUG1, "Calculate Gram matrix...\n");

	int cosize = channels * (channels + 1) / 2;

	assert(cov_dims[3] == cosize);

#pragma omp parallel for collapse(3)
	for (int k = 0; k < zh; k++) {
		for (int j = 0; j < yh; j++) {
			for (int i = 0; i < xh; i++) {

				complex float gram[cosize];
				gram_matrix2(channels, gram, nr_kernels, (const complex float (*)[nr_kernels])(imgkern2 + ((k * yh + j) * xh + i) * (channels * nr_kernels)));

#ifdef FLIP
				// add (scaled) identity matrix
				for (int i = 0, l = 0; i < channels; i++)
					for (int j = 0; j <= i; j++, l++)
						gram[l] = ((i == j) ? (kx * ky * kz) : 0.) - gram[l];
#endif
				for (int l = 0; l < cosize; l++)
					imgcov[(((l * zh) + k) * yh + j) * xh + i] = gram[l] / scalesq;
			}
		}
	}

	md_free(imgkern2);
}


