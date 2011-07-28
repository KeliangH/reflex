/* -*- mode: C; c-basic-offset: 4  -*- */
/* ex: set shiftwidth=4 expandtab: */
/*
 * Copyright (c) 2010, Georgia Tech Research Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the Georgia Tech Research Corporation nor
 *       the names of its contributors may be used to endorse or
 *       promote products derived from this software without specific
 *       prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY GEORGIA TECH RESEARCH CORPORATION ''AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL GEORGIA
 * TECH RESEARCH CORPORATION BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <amino.h>
#include "reflex.h"

const char* rfx_status_string(rfx_status_t i) {
    switch(i) {
    case RFX_OK: return "OK";
    case RFX_INVAL: return "INVAL";
    case RFX_LIMIT_POSITION: return "LIMIT_POSITION";
    case RFX_LIMIT_POSITION_ERROR: return "LIMIT_POSITION_ERROR";
    case RFX_LIMIT_FORCE: return "LIMIT_FORCE";
    case RFX_LIMIT_MOMENT: return "LIMIT_MOMENT";
    case RFX_LIMIT_FORCE_ERROR: return "LIMIT_FORCE_ERROR";
    case RFX_LIMIT_MOMENT_ERROR: return "LIMIT_MOMENT_ERROR";
    case RFX_LIMIT_CONFIGURATION: return "LIMIT_CONFIGURATION";
    case RFX_LIMIT_CONFIGURATION_ERROR: return "LIMIT_CONFIGURATION_ERROR";
    }
    return "unknown";
}

void rfx_ctrl_ws_init( rfx_ctrl_ws_t *g, size_t n ) {
    memset( g, 0, sizeof(*g) );
    g->n_q = n;
    g->q = AA_NEW0_AR( double, n );
    g->dq = AA_NEW0_AR( double, n );
    g->J = AA_NEW0_AR( double, n*6 );
    g->q_r = AA_NEW0_AR( double, n );
    g->dq_r = AA_NEW0_AR( double, n );
    g->q_min = AA_NEW0_AR( double, n );
    g->q_max = AA_NEW0_AR( double, n );
}

void rfx_ctrl_ws_destroy( rfx_ctrl_ws_t *g ) {
    free(g->q);
    free(g->dq);
    free(g->J);
    free(g->q_r);
    free(g->dq_r);
    free(g->q_min);
    free(g->q_max);
}

AA_API void rfx_ctrl_ws_lin_k_init( rfx_ctrl_ws_lin_k_t *k, size_t n_q  ) {
    k->n_q = n_q;
    k->q = AA_NEW0_AR( double, k->n_q );
}
AA_API void rfx_ctrl_ws_lin_k_destroy( rfx_ctrl_ws_lin_k_t *k ) {
    free(k->q);
}

// FIXME: check directions for all limits
// FIXME: add hard limits
static int check_limit( const rfx_ctrl_t *g, const double dx[3] ) {
    // F_max
    if( (g->F_max > 0) /* has limit */ &&
        (aa_la_dot( 3, g->F, g->F ) > g->F_max*g->F_max) /* magnitude check */ &&
        0 < aa_la_dot( 3, g->F, dx ) /*direction check*/ )
    {
        return RFX_LIMIT_FORCE;
    }
    // M_max
    if( (g->M_max > 0) &&
        (aa_la_dot( 3, g->F+3, g->F+3 ) > g->M_max*g->M_max) )
        return RFX_LIMIT_MOMENT;
    // q_min, q_max
    for( size_t i = 0; i < g->n_q; i++ ) {
        if( (g->q[i] < g->q_min[i]) || (g->q[i] > g->q_max[i] ) )
            return RFX_LIMIT_CONFIGURATION;
    }
    // x_min, x_max
    for( size_t i = 0; i < 3; i++ ) {
        if( (g->x[i] < g->x_min[i]) || (g->x[i] > g->x_max[i] ) )
            return RFX_LIMIT_POSITION;
    }
    // e_q_max
    if( (g->e_q_max > 0) &&
        ( aa_la_ssd(g->n_q, g->q, g->q_r) > g->e_q_max * g->e_q_max ) )
        return RFX_LIMIT_CONFIGURATION_ERROR;
    // e_x_max
    if( (g->e_x_max > 0) &&
        ( aa_la_ssd(3, g->x, g->x_r) > g->e_x_max * g->e_x_max ) )
        return RFX_LIMIT_POSITION_ERROR;
    // e_F_max
    if( (g->e_F_max > 0) &&
        ( aa_la_ssd(3, g->F, g->F_r) > g->e_F_max * g->e_F_max ) )
        return RFX_LIMIT_FORCE_ERROR;;
    // e_M_max
    if( (g->e_M_max > 0) &&
        ( aa_la_ssd(3, g->F+3, g->F_r+3) > g->e_M_max * g->e_M_max ) )
        return RFX_LIMIT_MOMENT_ERROR;

    return RFX_OK;
}

/*
 * u = J^* * (  dx_r - k_p * (x - x_r) -  k_f * (F - F_r) )
 */
rfx_status_t rfx_ctrl_ws_lin_vfwd( const rfx_ctrl_ws_t *ws, const rfx_ctrl_ws_lin_k_t *k, double *u ) {
    double dx_u[6], x_e[6] ;
    double dq_r[ws->n_q];

    assert( ws->n_q == k->n_q );

    // check force limits
    {
        int r = check_limit( ws, ws->dx_r );
        if( RFX_OK != r ) {
            aa_fzero( u, ws->n_q );
            return r;
        }
    }


    // find position error
    aa_la_vsub( 3, ws->x, ws->x_r, x_e );

    // find orientation error
    {
        double r_e[4];
        aa_tf_qrel(ws->r, ws->r_r, r_e);
        aa_tf_quat2rotvec_near( r_e, AA_FAR(0,0,0), x_e+3 );    // axis-angle conversion
    }

    // find workspace velocity
    // dx_u = dx_r - k_p * x_e -  k_f * (F - F_r)
    for(size_t i = 0; i < 6; i ++ ) {
        dx_u[i] = ws->dx_r[i]
            - k->p[i] * x_e[i]
            - k->f[i] * (ws->F[i] - ws->F_r[i]);
    }
    // jointspace reference velocity
    for( size_t i = 0; i < ws->n_q; i ++ ) {
        dq_r[i] = -k->q[i] * (ws->q[i] - ws->q_r[i]);// + ws->dq_r[i];
    }
    // find jointspace velocity
    aa_la_dlsnp( 6, ws->n_q, k->dls, ws->J, dx_u, dq_r, u );

    return RFX_OK;
}

/*******
 * LQG *
 *******/

AA_API void rfx_clqg_init( rfx_clqg_t *lqg, size_t n_x, size_t n_u, size_t n_z );
AA_API void rfx_clqg_destroy( rfx_clqg_t *lqg, size_t n_x, size_t n_u, size_t n_z );

AA_API void rfx_clqg_observe_euler( rfx_clqg_t *lqg, double dt, aa_region_t *reg ) {
    // --- calculate kalman gain ---

    // dP = A*P + P*A' - P*C'*W^{-1}*C*P + V
    // solve ARE with dP = 0, result is P
    double *Ct = (double*)aa_region_alloc(reg, sizeof(double)*lqg->n_x*lqg->n_z);
    aa_la_transpose2( lqg->n_z, lqg->n_x, lqg->C, Ct );
    double *P = (double*)aa_region_alloc(reg, sizeof(double)*lqg->n_x*lqg->n_x);
    aa_la_care_laub( lqg->n_x, lqg->n_u, lqg->n_z,
                     lqg->A, lqg->B, Ct, P, reg );
    // K = P * C' * W^{-1}  :  (nx*nx) * (nx*nz) * (nz*nz)
    double *PCt = (double*)aa_region_alloc( reg, sizeof(double)*lqg->n_x*lqg->n_z );
    // PCt := P * C'
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans,
                (int)lqg->n_x, (int)lqg->n_z, (int)lqg->n_x,
                1.0, P, (int)lqg->n_x, lqg->C, (int)lqg->n_z,
                0.0, PCt, (int)lqg->n_x);
    // K := PCt * W^{-1}
    double *K = (double*)aa_region_alloc(reg, sizeof(double)*lqg->n_x*lqg->n_z);
    double *Winv = (double*)aa_region_alloc(reg, sizeof(double)*lqg->n_z*lqg->n_z);
    memcpy(Winv, lqg->W, sizeof(double)*lqg->n_z*lqg->n_z);
    aa_la_inv( lqg->n_z, Winv);
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
                (int)lqg->n_x, (int)lqg->n_z, (int)lqg->n_z,
                1.0, PCt, (int)lqg->n_x, Winv, (int)lqg->n_z,
                0.0, K, (int)lqg->n_x);


    // --- predict from previous input ---
    // dx = A*x + B*u + K * ( z - C*xh )
    double *dx = (double*)aa_region_alloc(reg, sizeof(double)*lqg->n_x);
    // zz := z
    //double *zz = (double*)aa_region_alloc(reg, sizeof(double)*lqg->n_z);


    // x = x + dx * dt
    aa_la_axpy( lqg->n_x, dt, dx, lqg->x );
    aa_region_pop(reg, Ct );
}

//AA_API void rfx_clqg_ctrl( rfx_clqg_t *lqg, double dt, aa_region_t *reg ) {
    // compute optimal gain
    //  -dS = A'*S + S*A - S*B*R^{-1}*B' + Q
    // solve ARE, result is S
    // L = R^{-1} * B' * S  : (nu*nu) * (nu*nx) * (nx*nx)

    // compute current input
    // u = -Lx
//}


AA_API void rfx_lqg_observe
( size_t n_x, size_t n_u, size_t n_z,
  const double *A, const double *B, const double *C,
  const double *x, const double *u, const double *z,
  const double *K,
  double *dx, double *zwork )
{
    //zwork := z
    memcpy(zwork, z, sizeof(double)*n_z);
    // zz := -C*x + zz
    cblas_dgemv( CblasColMajor, CblasNoTrans,
                 (int)n_z, (int)n_x,
                 -1.0, C, (int)n_z,
                 x, 1, 1.0, zwork, 1 );
    // dx := K * zwork
    cblas_dgemv( CblasColMajor, CblasNoTrans,
                 (int)n_x, (int)n_z,
                 1.0, K, (int)n_x,
                 zwork, 1, 0.0, dx, 1 );
    // dx := B*u + dx
    cblas_dgemv( CblasColMajor, CblasNoTrans,
                 (int)n_x, (int)n_u,
                 1.0, B, (int)n_x,
                 u, 1, 1.0, dx, 1 );
    // dx := A*x + dx
    cblas_dgemv( CblasColMajor, CblasNoTrans,
                 (int)n_x, (int)n_x,
                 1.0, A, (int)n_x,
                 x, 1, 1.0, dx, 1 );
}
