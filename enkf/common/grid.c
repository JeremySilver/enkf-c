/******************************************************************************
 *
 * File:        grid.c        
 *
 * Created:     12/2012
 *
 * Author:      Pavel Sakov
 *              Bureau of Meteorology
 *
 * Description:
 *
 * Revisions:   15/02/2016 PS x2fi_reg() now returns NaN if estimated fi either
 *              < 0.0 or > (double) (n - 1). (In the previous version indices
 *              -0.5 < fi <= 0.0 were mapped to 0.0, and indices
 *              (double) (n - 1) < fi < (double) n - 0.5 were mapped to
 *              (double) (n - 1).)
 *
 *****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <float.h>
#include <string.h>
#include "ncw.h"
#include "definitions.h"
#include "utils.h"
#include "grid.h"
#include "gridprm.h"
#if !defined(NO_GRIDUTILS)
#include <gridnodes.h>
#include <gridmap.h>
#include <gucommon.h>
#endif

#define EPS_LON 1.0e-3
#define EPS_IJ 1.0e-3
#define CELL_CHANGE_FACTOR_MAX 0.5

typedef struct {
    int nx;
    int ny;
    int periodic_x;
    int regular_x;
    int regular_y;

    double* x;
    double* y;
    double* xc;
    double* yc;
} gxy_simple;

#if !defined(NO_GRIDUTILS)
typedef struct {
    gridnodes* gn;
    gridmap* gm;
} gxy_curv;
#else
#define NT_NONE 0
#endif

typedef struct {
    int nz;
    double* zt;
    double* zc;
} gz_simple;

typedef struct {
    int nx;
    int ny;
    int nz;
    double* a;
    double* b;
    float** p1;
    float** p2;

    double fi_prev;
    double fj_prev;
    double* pt;
    double* pc;
} gz_hybrid;

struct grid {
    char* name;
    int id;
    int htype;                  /* horizontal type */
    int vtype;                  /* vertical type */
#if !defined(NO_GRIDUTILS)
    int maptype;                /* grid map type; effective for curvilinear
                                 * grids only */
#endif

    grid_tocartesian_fn tocartesian_fn;

    void* gridnodes_xy;         /* (the structure is defined by `htype') */
    double lonbase;             /* (lon range = [lonbase, lonbase + 360)] */

    void* gridnodes_z;

    /*
     * `numlevels' can hold either the number of levels (z-model) or the
     * land mask (sigma-model)
     */
    int** numlevels;
    float** depth;

    /*
     * `stride' for calculating ensemble transforms. "0" means to use the
     * common value defined in the top prm file. 
     */
    int stride;

    /*
     * "Spread factor", normally set to 1. Introduced to adjust relative spread
     * of the ocean and atmospheric parts in climate models. Applies to all
     * variables of the grid.
     */
    double sfactor;
};

/**
 */
static gxy_simple* gxy_simple_create(int nx, int ny, double* x, double* y)
{
    gxy_simple* nodes = malloc(sizeof(gxy_simple));
    int i, ascending;
    double dx, dy;

    assert(nx >= 2 && ny >= 2);

    /*
     * x
     */
    nodes->nx = nx;
    nodes->x = x;
    nodes->xc = malloc((nx + 1) * sizeof(double));
    nodes->xc[0] = x[0] * 1.5 - x[1] * 0.5;
    for (i = 1; i < nx + 1; ++i)
        nodes->xc[i] = 2 * x[i - 1] - nodes->xc[i - 1];

    if (fabs(fmod(nodes->x[nx - 1] - nodes->x[0] + EPS_LON / 2.0, 360.0)) < EPS_LON)
        nodes->periodic_x = 1;  /* closed grid */
    else if (fabs(fmod(2.0 * nodes->x[nx - 1] - nodes->x[nx - 2] - nodes->x[0] + EPS_LON / 2.0, 360.0)) < EPS_LON) {
        nodes->periodic_x = 2;  /* non-closed grid (used e.g. by MOM) */
        nodes->x = realloc(nodes->x, (nx + 1) * sizeof(double));
        nodes->x[nx] = 2.0 * nodes->x[nx - 1] - nodes->x[nx - 2];
        nodes->xc = realloc(nodes->xc, (nx + 2) * sizeof(double));
        nodes->xc[nx + 1] = 2.0 * nodes->xc[nx] - nodes->xc[nx - 1];
    } else
        nodes->periodic_x = 0;

    dx = (nodes->x[nx - 1] - nodes->x[0]) / (double) (nx - 1);
    for (i = 1; i < (int) nx; ++i)
        if (fabs(nodes->x[i] - nodes->x[i - 1] - dx) / fabs(dx) > EPS_LON)
            break;
    nodes->regular_x = (i == nx);

    ascending = (nodes->x[nx - 1] > nodes->x[0]);
    if (ascending) {
        for (i = 0; i < nx - 1; ++i)
            if ((nodes->x[i + 1] - nodes->x[i]) < 0.0)
                enkf_quit("non-monotonic X coordinate for a simple grid\n");
    } else {
        for (i = 0; i < nx - 1; ++i)
            if ((nodes->x[i + 1] - nodes->x[i]) > 0.0)
                enkf_quit("non-monotonic X coordinate for a simple grid\n");
    }

    /*
     * y
     */
    nodes->ny = ny;
    nodes->y = y;
    nodes->yc = malloc((ny + 1) * sizeof(double));
    nodes->yc[0] = y[0] * 1.5 - y[1] * 0.5;
    for (i = 1; i < ny + 1; ++i)
        nodes->yc[i] = 2 * y[i - 1] - nodes->yc[i - 1];

    dy = (y[ny - 1] - y[0]) / (double) (ny - 1);
    for (i = 1; i < (int) ny; ++i)
        if (fabs(y[i] - y[i - 1] - dy) / fabs(dy) > EPS_LON)
            break;
    nodes->regular_y = (i == ny);

    ascending = (y[ny - 1] > y[0]);
    if (ascending) {
        for (i = 0; i < ny - 1; ++i)
            if ((y[i + 1] - y[i]) < 0.0)
                enkf_quit("non-monotonic Y coordinate for a simple grid\n");
    } else {
        for (i = 0; i < ny - 1; ++i)
            if ((y[i + 1] - y[i]) > 0.0)
                enkf_quit("non-monotonic Y coordinate for a simple grid\n");
    }

    return nodes;
}

/**
 */
void gxy_simple_destroy(gxy_simple* nodes)
{
    free(nodes->x);
    free(nodes->y);
    free(nodes->xc);
    free(nodes->yc);
    free(nodes);
}

#if !defined(NO_GRIDUTILS)
/**
 */
static gxy_curv* gxy_curv_create(int nodetype, int nx, int ny, double** x, double** y, int maptype)
{
    gxy_curv* nodes = malloc(sizeof(gxy_curv));

    if (nodetype == NT_CEN) {
        gridnodes* gn_new;

        nodes->gn = gridnodes_create2(nx, ny, NT_CEN, x, y);
        gn_new = gridnodes_transform(nodes->gn, NT_COR);
        gridnodes_destroy(nodes->gn);
        nodes->gn = gn_new;
    } else if (nodetype == NT_COR)
        nodes->gn = gridnodes_create2(nx, ny, NT_COR, x, y);
    else
        enkf_quit("unknown node type for horizontal curvilinear grid");
#if defined(ENKF_PREP) || defined(ENKF_CALC)
    gridnodes_validate(nodes->gn);
    gridnodes_setmaptype(nodes->gn, maptype);
    nodes->gm = gridmap_build2(nodes->gn);
#else
    nodes->gm = NULL;
#endif
    return nodes;
}

/**
 */
void gxy_curv_destroy(gxy_curv* nodes)
{
    gridnodes_destroy(nodes->gn);
    if (nodes->gm != NULL)
        gridmap_destroy(nodes->gm);
    free(nodes);
}
#endif

#define EPSZ 0.001

/**
 */
static gz_simple* gz_simple_create(int nz, double* z)
{
    gz_simple* gz = malloc(sizeof(gz_simple));
    int i;

#if !defined(ZSIGN_NOCHECK)
    /*
     * reverse z if it is negative
     */
    for (i = 0; i < nz; ++i)
        if (z[i] < -EPSZ)
            break;
    if (i < nz)
        for (i = 0; i < nz; ++i)
            z[i] = -z[i];
    for (i = 0; i < nz; ++i)
        if (z[i] < -EPSZ)
            enkf_quit("layer centre coordinates should be either positive or negative only");
#else
    if ((fabs(z[0]) > fabs(z[nz - 1]) && z[0] < 0.0) || (fabs(z[0]) < fabs(z[nz - 1]) && z[nz - 1] < 0.0))
        for (i = 0; i < nz; ++i)
            z[i] = -z[i];
#endif

    gz->zt = z;
    gz->nz = nz;

    /*
     * this code is supposed to work both for z and sigma grids
     */
    gz->zc = malloc((nz + 1) * sizeof(double));
    if (z[nz - 1] >= z[0]) {
        /*
         * layer 0 at surface
         */
#if defined(ZSIGN_NOCHECK)
        assert(nz > 2 && fabs(2 * z[1] - z[0] - z[2]) < EPSZ);
        gz->zc[0] = 1.5 * z[0] - 0.5 * z[1];
#else
        gz->zc[0] = 0.0;
#endif
        for (i = 1; i <= nz; ++i) {
            gz->zc[i] = 2.0 * z[i - 1] - gz->zc[i - 1];
            /*
             * layer boundary should be above the next layer centre
             */
            if (i < nz)
                assert(gz->zc[i] < z[i]);
        }
    } else {
        /*
         * layer 0 the deepest
         */
#if defined(ZSIGN_NOCHECK)
        assert(nz > 2 && fabs(2 * z[nz - 2] - z[nz - 1] - z[nz - 3]) < EPSZ);
        gz->zc[nz] = 1.5 * z[nz - 1] - 0.5 * z[nz - 2];
#else
        gz->zc[nz] = 0.0;
#endif
        for (i = nz - 1; i >= 0; --i) {
            gz->zc[i] = 2.0 * z[i] - gz->zc[i + 1];
            /*
             * layer boundary should be above the next layer centre
             */
            if (i > 0)
                assert(gz->zc[i] < z[i - 1]);
        }
    }

    return gz;
}

/**
 */
static gz_hybrid* gz_hybrid_create(int nx, int ny, int nz, double* a, double* b, float** p1, float** p2)
{
    gz_hybrid* gz = malloc(sizeof(gz_hybrid));

    gz->nx = nx;
    gz->ny = ny;
    gz->nz = nz;
    gz->a = a;
    gz->b = b;
    gz->p1 = p1;
    gz->p2 = p2;

    gz->fi_prev = NAN;
    gz->fj_prev = NAN;
    gz->pt = malloc(nz * sizeof(double));
    gz->pc = malloc((nz + 1) * sizeof(double));

    return gz;
}

/**
 */
static void gz_simple_destroy(gz_simple* gz)
{
    free(gz->zt);
    free(gz->zc);
    free(gz);
}

/**
 */
static void gz_hybrid_destroy(gz_hybrid* gz)
{
    free(gz->a);
    free(gz->b);
    free(gz->p1);
    free(gz->p2);
    free(gz->pt);
    free(gz->pc);
}

/**
 */
static double x2fi_reg(int n, double* v, double x, int periodic)
{
    double fi;

    if (n < 2)
        return NAN;

    if (periodic == 2)
        n += 1;

    fi = (x - v[0]) / (v[n - 1] - v[0]) * (double) (n - 1);

    if (fi < 0.0) {
        if (!periodic)
            return NAN;
        else
            fi += (double) (n - 1);
    } else if (fi >= (double) (n - 1)) {
        if (!periodic)
            return NAN;
        else
            fi -= (double) (n - 1);
    }

    return fi;
}

/**
 */
static double fi2x(int n, double* v, double fi, int periodic)
{
    double ifrac;
    int i;

    if (n < 2)
        return NAN;

    if (periodic == 2)
        n += 1;

    if (periodic) {
        if (fi < 0.0)
            fi += (double) (n - 1);
        else if (fi >= (double) (n - 1))
            fi -= (double) (n - 1);
    } else {
        if (fi < 0.0 || fi > (double) (n - 1))
            return NAN;
    }

    ifrac = fi - floor(fi);
    i = (int) fi;
    if (ifrac == 0.0)           /* (also covers i = n - 1) */
        return v[i];
    else
        return v[i] + ifrac * (v[i + 1] - v[i]);
}

/**
 */
static void gs_fij2xy(void* p, double fi, double fj, double* x, double* y)
{
    gxy_simple* nodes = (gxy_simple*) ((grid*) p)->gridnodes_xy;

    *x = fi2x(nodes->nx, nodes->x, fi, nodes->periodic_x);
    *y = fi2x(nodes->ny, nodes->y, fj, 0);
}

/** Gets fractional index of a coordinate for a 1D irregular grid.
 * @param n Number of grid nodes (vertical layers)
 * @param v Coordinates of the nodes (layer centres)
 * @param vb Coordinates of the cell/layer boundaries [n + 1]
 * @param x Input coordinate
 * @param periodic Flag for grid periodicity
 * @param lonbase lon range = [lonbase, lonbase + 360)
 * @return Fractional index for `x'
 */
static double x2fi_irreg(int n, double v[], double vb[], double x, int periodic, double lonbase)
{
    int ascending, i1, i2, imid;

    if (n < 2)
        return NAN;

    if (periodic == 2)
        n += 1;

    if (!isnan(lonbase)) {
        if (x < lonbase)
            x += 360.0;
        else if (x >= lonbase + 360.0)
            x -= 360.0;
    }

    ascending = (v[n - 1] > v[0]);

    if (ascending) {
        if ((x < vb[0]) || x > vb[n])
            return NAN;
    } else {
        if ((x > vb[0]) || x < vb[n])
            return NAN;
    }

    i1 = 0;
    i2 = n - 1;
    if (ascending) {
        while (1) {
            imid = (i1 + i2) / 2;
            if (imid == i1)
                break;
            if (x > vb[imid])
                i1 = imid;
            else
                i2 = imid;
        }
        if (x < vb[i1 + 1])
            return (double) i1 + (x - v[i1]) / (vb[i1 + 1] - vb[i1]);
        else
            return (double) i1 + 0.5 + (x - vb[i1 + 1]) / (vb[i1 + 2] - vb[i1 + 1]);
    } else {
        while (1) {
            imid = (i1 + i2) / 2;
            if (imid == i1)
                break;
            if (x > vb[imid])
                i2 = imid;
            else
                i1 = imid;
        }
        if (x > vb[i1 + 1])
            return (double) i1 + (x - v[i1]) / (vb[i1 + 1] - vb[i1]);
        else
            return (double) i1 + 0.5 + (x - vb[i1 + 1]) / (vb[i1 + 2] - vb[i1 + 1]);
    }
}

/**
 */
static void gs_xy2fij(void* p, double x, double y, double* fi, double* fj)
{
    gxy_simple* nodes = (gxy_simple*) ((grid*) p)->gridnodes_xy;
    double lonbase = ((grid*) p)->lonbase;

    if (nodes->regular_x)
        *fi = x2fi_reg(nodes->nx, nodes->x, x, nodes->periodic_x);
    else
        *fi = x2fi_irreg(nodes->nx, nodes->x, nodes->xc, x, nodes->periodic_x, lonbase);
    if (nodes->regular_y)
        *fj = x2fi_reg(nodes->ny, nodes->y, y, 0);
    else
        *fj = x2fi_irreg(nodes->ny, nodes->y, nodes->yc, y, 0, NAN);
}

#if !defined(NO_GRIDUTILS)
/**
 */
static void gc_xy2fij(void* p, double x, double y, double* fi, double* fj)
{
    gxy_curv* nodes = (gxy_curv*) ((grid*) p)->gridnodes_xy;

    if (gridmap_xy2fij(nodes->gm, x, y, fi, fj) == 1)
        return;                 /* success */

    *fi = NAN;
    *fj = NAN;
}

/**
 */
static void gc_fij2xy(void* p, double fi, double fj, double* x, double* y)
{
    gxy_curv* nodes = (gxy_curv*) ((grid*) p)->gridnodes_xy;

    gridmap_fij2xy(nodes->gm, fi, fj, x, y);
}
#endif

/**
 */
static double z2fk_basic(int n, double* zt, double* zc, double z)
{
    int ascending, i1, i2, imid;

    ascending = (zt[n - 1] > zt[0]) ? 1 : 0;

    if (ascending) {
        if (z < zc[0])
            return 0.0;
        if (z > zc[n])
            return NAN;
    } else {
        if (z < zc[n])
            return 0.0;
        if (z > zc[0])
            return NAN;
    }

    i1 = 0;
    i2 = n - 1;
    if (ascending) {
        while (1) {
            imid = (i1 + i2) / 2;
            if (imid == i1)
                break;
            if (z > zc[imid])
                i1 = imid;
            else
                i2 = imid;
        }
        if (z < zc[i1 + 1])
            return (double) i1 + (z - zt[i1]) / (zc[i1 + 1] - zc[i1]);
        else
            return (double) i1 + 0.5 + (z - zc[i1 + 1]) / (zc[i1 + 2] - zc[i1 + 1]);
    } else {
        while (1) {
            imid = (i1 + i2) / 2;
            if (imid == i1)
                break;
            if (z > zc[imid])
                i2 = imid;
            else
                i1 = imid;
        }
        if (z > zc[i1 + 1])
            return (double) i1 + (z - zt[i1]) / (zc[i1 + 1] - zc[i1]);
        else
            return (double) i1 + 0.5 + (z - zc[i1 + 1]) / (zc[i1 + 2] - zc[i1 + 1]);
    }
}

/**
 */
static void gz_simple_z2fk(void* p, double fi, double fj, double z, double* fk)
{
    grid* g = (grid*) p;
    gz_simple* nodes = g->gridnodes_z;

    /*
     * for sigma coordinates convert `z' to sigma
     */
    if (g->vtype == GRIDVTYPE_SIGMA) {
        int ni = 0, nj = 0;
        double depth;

        if (g->htype == GRIDHTYPE_LATLON) {
            gxy_simple* nodes = (gxy_simple*) g->gridnodes_xy;

            ni = nodes->nx;
            nj = nodes->ny;
#if !defined(NO_GRIDUTILS)
        } else if (g->htype == GRIDHTYPE_CURVILINEAR) {
            gxy_curv* nodes = (gxy_curv*) g->gridnodes_xy;

            ni = gridnodes_getnx(nodes->gn);
            nj = gridnodes_getny(nodes->gn);
#endif
        } else
            enkf_quit("programming error");

        depth = (double) interpolate2d(fi, fj, ni, nj, g->depth, g->numlevels, grid_isperiodic_x(g));
        z /= depth;
    }

    *fk = z2fk_basic(nodes->nz, nodes->zt, nodes->zc, z);
}

/**
 */
static void gz_hybrid_z2fk(void* p, double fi, double fj, double z, double* fk)
{
    grid* g = (grid*) p;
    gz_hybrid* gz = (gz_hybrid*) g->gridnodes_z;

    if (isnan(gz->fi_prev) || fabs(fi - gz->fi_prev) > EPS_IJ || fabs(fj - gz->fj_prev) > EPS_IJ) {
        double p1 = interpolate2d(fi, fj, gz->nx, gz->ny, gz->p1, g->numlevels, grid_isperiodic_x(g));
        double p2 = interpolate2d(fi, fj, gz->nx, gz->ny, gz->p2, g->numlevels, grid_isperiodic_x(g));

        int i;

        for (i = 0; i < gz->nz; ++i)
            gz->pt[i] = gz->a[i] + gz->b[i] * (p1 - p2);        /* for now
                                                                 * assume p1
                                                                 * > p2 */
        gz->pc[0] = 1.5 * gz->pt[0] - 0.5 * gz->pt[1];
	if (gz->pc[0] < 0.0)
	    gz->pc[0] = 0.0;
        /*
         * (some implicit assumptions about directions here)
         */
        for (i = 1; i <= gz->nz; ++i)
            gz->pc[i] = 2.0 * gz->pt[i - 1] - gz->pc[i - 1];
        gz->fi_prev = fi;
        gz->fj_prev = fj;
    }
    *fk = z2fk_basic(gz->nz, gz->pt, gz->pc, z);
}

/**
 */
static void grid_setlonbase(grid* g)
{
    double xmin = DBL_MAX;
    double xmax = -DBL_MAX;

    if (g->htype == GRIDHTYPE_LATLON) {
        double* x = ((gxy_simple*) g->gridnodes_xy)->x;
        int nx = ((gxy_simple*) g->gridnodes_xy)->nx;

        if (xmin > x[0])
            xmin = x[0];
        if (xmin > x[nx])
            xmin = x[nx];
        if (xmax < x[0])
            xmax = x[0];
        if (xmax < x[nx])
            xmax = x[nx];
#if !defined(NO_GRIDUTILS)
    } else if (g->htype == GRIDHTYPE_CURVILINEAR) {
        double** x = gridnodes_getx(((gxy_curv*) g->gridnodes_xy)->gn);
        int nx = gridnodes_getnce1(((gxy_curv*) g->gridnodes_xy)->gn);
        int ny = gridnodes_getnce2(((gxy_curv*) g->gridnodes_xy)->gn);
        int i, j;

        for (j = 0; j < ny; ++j) {
            for (i = 0; i < nx; ++i) {
                if (xmin > x[j][i])
                    xmin = x[j][i];
                if (xmax < x[j][i])
                    xmax = x[j][i];
            }
        }
#endif
    }
    if (xmin >= 0.0 && xmax <= 360.0)
        g->lonbase = 0.0;
    else if (xmin >= -180.0 && xmax <= 180.0)
        g->lonbase = -180.0;
    else
        g->lonbase = xmin;
}

/**
 */
static void grid_sethgrid(grid* g, int htype, int hnodetype, int nx, int ny, void* x, void* y)
{
    g->htype = htype;
    if (htype == GRIDHTYPE_LATLON)
        g->gridnodes_xy = gxy_simple_create(nx, ny, x, y);
#if !defined(NO_GRIDUTILS)
    else if (htype == GRIDHTYPE_CURVILINEAR) {
        g->gridnodes_xy = gxy_curv_create(hnodetype, nx, ny, x, y, g->maptype);
#if defined(GRIDNODES_WRITE)
        {
            char fname[MAXSTRLEN];

            snprintf(fname, MAXSTRLEN, "gridnodes-%d.txt", grid_getid(g));
            if (!file_exists(fname))
                gridnodes_write(((gxy_curv*) g->gridnodes_xy)->gn, fname, CT_XY);
        }
#endif
    }
#endif
    else
        enkf_quit("programming error");
    grid_setlonbase(g);
}

/**
 */
grid* grid_create(void* p, int id)
{
    gridprm* prm = (gridprm*) p;
    grid* g = calloc(1, sizeof(grid));
    char* fname = prm->fname;
    int ncid;
    int dimid_x, dimid_y, dimid_z;
    int varid_x, varid_y;
    int ndims_x, ndims_y;
    size_t nx, ny, nz;
    int varid_depth, varid_numlevels;

    g->name = strdup(prm->name);
    g->id = id;
    g->vtype = gridprm_getvtype(prm);
    if (prm->stride != 0)
        g->stride = prm->stride;
    g->sfactor = prm->sfactor;
#if !defined(NO_GRIDUTILS)
#if !defined(GRIDMAP_TYPE_DEF)
#error("GRIDMAP_TYPE_DEF not defined; please update gridutils-c");
#endif
    if (prm->maptype == 'b' || prm->maptype == 'B')
        g->maptype = GRIDMAP_TYPE_BINARY;
    else if (prm->maptype == 'k' || prm->maptype == 'K')
        g->maptype = GRIDMAP_TYPE_KDTREE;
    else
        enkf_quit("unknown grid map type \"%c\"", prm->maptype);
#endif

    ncw_open(fname, NC_NOWRITE, &ncid);
    ncw_inq_dimid(ncid, prm->xdimname, &dimid_x);
    ncw_inq_dimid(ncid, prm->ydimname, &dimid_y);
    ncw_inq_dimlen(ncid, dimid_x, &nx);
    ncw_inq_dimlen(ncid, dimid_y, &ny);

    ncw_inq_varid(ncid, prm->xvarname, &varid_x);
    ncw_inq_varid(ncid, prm->yvarname, &varid_y);

    ncw_inq_varndims(ncid, varid_x, &ndims_x);
    ncw_inq_varndims(ncid, varid_y, &ndims_y);

    /*
     * set horizontal grid
     */
    if (ndims_x == 1 && ndims_y == 1) {
        double* x;
        double* y;

        x = malloc(nx * sizeof(double));
        y = malloc(ny * sizeof(double));

        ncw_get_var_double(ncid, varid_x, x);
        ncw_get_var_double(ncid, varid_y, y);

        grid_sethgrid(g, GRIDHTYPE_LATLON, NT_NONE, nx, ny, x, y);
    } else if (ndims_x == 2 && ndims_y == 2) {
#if defined(NO_GRIDUTILS)
        enkf_quit("%s: grid \"%s\" seems to be of curvilinear type; can not handle it due to flag NO_GRIDUTILS", fname, g->name);
#else
        double** x;
        double** y;

        x = alloc2d(ny, nx, sizeof(double));
        y = alloc2d(ny, nx, sizeof(double));

        ncw_get_var_double(ncid, varid_x, x[0]);
        ncw_get_var_double(ncid, varid_y, y[0]);

        grid_sethgrid(g, GRIDHTYPE_CURVILINEAR, NT_COR, nx, ny, x, y);
    }
#endif
    else
        enkf_quit("%s: could not determine the horizontal grid type", fname);

    /*
     * set vertical grid
     */
    if (g->vtype == GRIDVTYPE_Z || g->vtype == GRIDVTYPE_SIGMA) {
        int varid_z;
        double* z;

        ncw_inq_dimid(ncid, prm->zdimname, &dimid_z);
        ncw_inq_dimlen(ncid, dimid_z, &nz);
        z = malloc(nz * sizeof(double));
        ncw_inq_varid(ncid, prm->zvarname, &varid_z);
        ncw_check_vardims(ncid, varid_z, 1, &nz);
        ncw_get_var_double(ncid, varid_z, z);
        g->gridnodes_z = gz_simple_create(nz, z);
    } else if (g->vtype == GRIDVTYPE_HYBRID) {
        int ndims;
        double* a;
        double* b;
        float** p1 = alloc2d(ny, nx, sizeof(float));
        float** p2 = alloc2d(ny, nx, sizeof(float));
        int varid_a, varid_b, varid_p1, varid_p2;
        size_t dimlen[2];

        ncw_inq_varid(ncid, prm->avarname, &varid_a);
        ncw_inq_varndims(ncid, varid_a, &ndims);
        assert(ndims == 1);
        ncw_inq_vardimid(ncid, varid_a, &dimid_z);
        ncw_inq_dimlen(ncid, dimid_z, &nz);
        a = malloc(nz * sizeof(double));
        ncw_get_var_double(ncid, varid_a, a);

        ncw_inq_varid(ncid, prm->bvarname, &varid_b);
        b = malloc(nz * sizeof(double));
        ncw_get_var_double(ncid, varid_b, b);

        ncw_inq_varid(ncid, prm->p1varname, &varid_p1);
        dimlen[0] = ny;
        dimlen[1] = nx;
        ncw_check_vardims(ncid, varid_p1, 2, dimlen);
        ncw_get_var_float(ncid, varid_p1, p1[0]);

        ncw_inq_varid(ncid, prm->p2varname, &varid_p2);
        ncw_check_vardims(ncid, varid_p2, 2, dimlen);
        ncw_get_var_float(ncid, varid_p2, p2[0]);

        g->gridnodes_z = gz_hybrid_create(nx, ny, nz, a, b, p1, p2);
    } else
        enkf_quit("not implemented");

    if (prm->depthvarname != NULL) {
        size_t dimlen[2] = { ny, nx };

        g->depth = alloc2d(ny, nx, sizeof(float));
        ncw_inq_varid(ncid, prm->depthvarname, &varid_depth);
        ncw_check_vardims(ncid, varid_depth, 2, dimlen);
        ncw_get_var_float(ncid, varid_depth, g->depth[0]);
    }
    if (prm->levelvarname != NULL) {
        size_t dimlen[2] = { ny, nx };

        g->numlevels = alloc2d(ny, nx, sizeof(int));
        ncw_inq_varid(ncid, prm->levelvarname, &varid_numlevels);
        ncw_check_varndims(ncid, varid_numlevels, 2);
        ncw_check_vardims(ncid, varid_numlevels, 2, dimlen);
        ncw_get_var_int(ncid, varid_numlevels, g->numlevels[0]);
        if (g->vtype == GRIDVTYPE_SIGMA || g->vtype == GRIDVTYPE_HYBRID) {
            int i, j;

            for (j = 0; j < ny; ++j)
                for (i = 0; i < nx; ++i)
                    g->numlevels[j][i] *= nz;
        }
    }
    ncw_close(ncid);

    if (g->numlevels == NULL) {
        g->numlevels = alloc2d(ny, nx, sizeof(int));
        if (g->vtype == GRIDVTYPE_SIGMA || g->vtype == GRIDVTYPE_HYBRID) {
            int i, j;

            for (j = 0; j < ny; ++j)
                for (i = 0; i < nx; ++i)
                    if (g->depth == NULL || g->depth[j][i] > 0.0)
                        g->numlevels[j][i] = nz;
        } else {
            int i, j;

            if (g->depth != NULL) {
                for (j = 0; j < ny; ++j) {
                    for (i = 0; i < nx; ++i) {
                        double depth = g->depth[j][i];
                        double fk = NAN;

                        if (depth > 0.0) {
                            gz_simple_z2fk(g, j, i, depth, &fk);
                            g->numlevels[j][i] = ceil(fk + 0.5);
                        }
                    }
                }
            } else {
                for (j = 0; j < ny; ++j)
                    for (i = 0; i < nx; ++i)
                        g->numlevels[j][i] = nz;
            }
        }
    }

    gridprm_print(prm, "    ");
    grid_print(g, "    ");

    return g;
}

/**
 */
void grid_destroy(grid* g)
{
    free(g->name);
    if (g->htype == GRIDHTYPE_LATLON)
        gxy_simple_destroy(g->gridnodes_xy);
#if !defined(NO_GRIDUTILS)
    else if (g->htype == GRIDHTYPE_CURVILINEAR)
        gxy_curv_destroy(g->gridnodes_xy);
#endif
    else
        enkf_quit("programming_error");
    if (g->gridnodes_z != NULL) {
        if (g->vtype == GRIDVTYPE_Z || g->vtype == GRIDVTYPE_SIGMA)
            gz_simple_destroy(g->gridnodes_z);
        else if (g->vtype == GRIDVTYPE_HYBRID)
            gz_hybrid_destroy(g->gridnodes_z);
        else
            enkf_quit("not implemented");
    }
    if (g->numlevels != NULL)
        free(g->numlevels);
    if (g->depth != NULL)
        free(g->depth);

    free(g);
}

/**
 */
void grid_print(grid* g, char offset[])
{
    int nx, ny, nz;

    enkf_printf("%sgrid info:\n", offset);
    switch (g->htype) {
    case GRIDHTYPE_LATLON:
        enkf_printf("%s  hor type = LATLON\n", offset);
        enkf_printf("%s  periodic by X = %s\n", offset, grid_isperiodic_x(g) ? "yes" : "no");
        break;
#if !defined(NO_GRIDUTILS)
    case GRIDHTYPE_CURVILINEAR:
        enkf_printf("%s  hor type = CURVILINEAR\n", offset);
        if (g->maptype == GRIDMAP_TYPE_BINARY)
            enkf_printf("%s  map type = BINARY TREE\n", offset);
        else if (g->maptype == GRIDMAP_TYPE_KDTREE)
            enkf_printf("%s  map type = KD-TREE\n", offset);
        else
            enkf_quit("unknown grid map type");
        break;
#endif
    default:
        enkf_printf("%s  h type = NONE\n", offset);
    }
    grid_getdims(g, &nx, &ny, &nz);
    enkf_printf("%s  dims = %d x %d x %d\n", offset, nx, ny, nz);
    if (!isnan(g->lonbase))
        enkf_printf("%s  longitude range = [%.3f, %.3f]\n", offset, g->lonbase, g->lonbase + 360.0);
    else
        enkf_printf("%s  longitude range = any\n", offset);
    switch (g->vtype) {
    case GRIDVTYPE_Z:
        enkf_printf("%s  vert type = Z\n", offset);
        break;
    case GRIDVTYPE_SIGMA:
        enkf_printf("%s  vert type = SIGMA\n", offset);
        break;
    case GRIDVTYPE_HYBRID:
        enkf_printf("%s  vert type = HYBRID\n", offset);
        break;
    default:
        enkf_printf("%s  vert type = NONE\n", offset);
    }
    if (g->stride != 0)
        enkf_printf("%s  STRIDE = \"%d\"\n", offset, g->stride);
    if (g->sfactor != 1.0)
        enkf_printf("%s  SFACTOR = \"%f\"\n", offset, g->sfactor);
}

/**
 */
void grid_describeprm(void)
{
    enkf_printf("\n");
    enkf_printf("  Grid parameter file format:\n");
    enkf_printf("\n");
    enkf_printf("    NAME             = <name> [ PREP | CALC ]\n");
    enkf_printf("    VTYPE            = { z | sigma }\n");
#if !defined(NO_GRIDUTILS)
    enkf_printf("  [ MAPTYPE          = { binary* | kdtree } (curvilinear grids) ]\n");
#endif
    enkf_printf("    DATA             = <data file name>\n");
    enkf_printf("    XDIMNAME         = <x dimension name>\n");
    enkf_printf("    YDIMNAME         = <y dimension name>\n");
    enkf_printf("    ZDIMNAME         = <z dimension name>\n");
    enkf_printf("    XVARNAME         = <x variable name>\n");
    enkf_printf("    YVARNAME         = <y variable name>\n");
    enkf_printf("    ZVARNAME         = <z variable name>\n");
    enkf_printf("    DEPTHVARNAME     = <depth variable name>\n");
    enkf_printf("  [ NUMLEVELSVARNAME = <# of levels variable name> (z) ]\n");
    enkf_printf("  [ MASKVARNAME      = <land mask variable name> (sigma) ]\n");
    enkf_printf("  [ STRIDE           = <stride> (common*) ]\n");
    enkf_printf("  [ SFACTOR          = <spread factor> (1.0*) ]\n");
    enkf_printf("\n");
    enkf_printf("  [ <more of the above blocks> ]\n");
    enkf_printf("\n");
    enkf_printf("  Notes:\n");
    enkf_printf("    1. < ... > denotes a description of an entry\n");
    enkf_printf("    2. [ ... ] denotes an optional input\n");
    enkf_printf("    3. (...) is a note\n");
    enkf_printf("    4. * denotes the default value\n");
    enkf_printf("\n");
}

/**
 */
void grid_getdims(grid* g, int* ni, int* nj, int* nk)
{
    if (ni != NULL) {
        if (g->htype == GRIDHTYPE_LATLON) {
            gxy_simple* nodes = (gxy_simple*) g->gridnodes_xy;

            *ni = nodes->nx;
            *nj = nodes->ny;
#if !defined(NO_GRIDUTILS)
        } else if (g->htype == GRIDHTYPE_CURVILINEAR) {
            gxy_curv* nodes = (gxy_curv*) g->gridnodes_xy;

            *ni = gridnodes_getnx(nodes->gn);
            *nj = gridnodes_getny(nodes->gn);
#endif
        } else
            enkf_quit("programming error");
    }
    if (nk != NULL) {
        if (g->vtype == GRIDVTYPE_Z || g->vtype == GRIDVTYPE_SIGMA)
            *nk = ((gz_simple*) g->gridnodes_z)->nz;
        else if (g->vtype == GRIDVTYPE_HYBRID)
	    *nk = ((gz_hybrid*) g->gridnodes_z)->nz;
	else
	    enkf_quit("programming error");
    }
}

/**
 */
int grid_gettoplayerid(grid* g)
{
    if (g->vtype == GRIDVTYPE_Z || g->vtype == GRIDVTYPE_SIGMA) {
        gz_simple* nodes = g->gridnodes_z;
        int kmax = nodes->nz - 1;
        double* z = nodes->zt;

        return (fabs(z[0]) < fabs(z[kmax])) ? 0 : kmax;
    } else
        enkf_quit("not implemented");

    return -1;
}

/**
 */
char* grid_getname(grid* g)
{
    return g->name;
}

/**
 */
int grid_getid(grid* g)
{
    return g->id;
}

/**
 */
int grid_gethtype(grid* g)
{
    return g->htype;
}

/**
 */
int grid_getvtype(grid* g)
{
    return g->vtype;
}

/**
 */
float** grid_getdepth(grid* g)
{
    return g->depth;
}

/**
 */
int** grid_getnumlevels(grid* g)
{
    return g->numlevels;
}

/**
 */
double grid_getlonbase(grid* g)
{
    return g->lonbase;
}

/**
 */
int grid_getstride(grid* g)
{
    return g->stride;
}

/**
 */
void grid_setstride(grid* g, int stride)
{
    g->stride = stride;
}

/**
 */
double grid_getsfactor(grid* g)
{
    return g->sfactor;
}

/**
 */
void grid_xy2fij(grid* g, double x, double y, double* fi, double* fj)
{
    if (g->htype == GRIDHTYPE_LATLON)
        gs_xy2fij(g, x, y, fi, fj);
#if !defined(NO_GRIDUTILS)
    else if (g->htype == GRIDHTYPE_CURVILINEAR)
        gc_xy2fij(g, x, y, fi, fj);
#endif
    else
        enkf_quit("programming error");
}

/**
 */
void grid_z2fk(grid* g, double fi, double fj, double z, double* fk)
{
    if (g->vtype == GRIDVTYPE_Z || g->vtype == GRIDVTYPE_SIGMA)
        gz_simple_z2fk(g, fi, fj, z, fk);
    else if (g->vtype == GRIDVTYPE_HYBRID)
        gz_hybrid_z2fk(g, fi, fj, z, fk);
    else
        enkf_quit("not implemented");
}

/**
 */
void grid_fij2xy(grid* g, double fi, double fj, double* x, double* y)
{
    if (g->htype == GRIDHTYPE_LATLON)
        gs_fij2xy(g, fi, fj, x, y);
#if !defined(NO_GRIDUTILS)
    else if (g->htype == GRIDHTYPE_CURVILINEAR)
        gc_fij2xy(g, fi, fj, x, y);
#endif
    else
        enkf_quit("programming error");
}

/**
 */
void grid_ij2xy(grid* g, int i, int j, double* x, double* y)
{
    if (g->htype == GRIDHTYPE_LATLON) {
        gxy_simple* gs = (gxy_simple*) g->gridnodes_xy;

        if (i < 0 || j < 0 || i >= gs->nx || j >= gs->ny) {
            *x = NAN;
            *y = NAN;
        } else {
            *x = gs->x[i];
            *y = gs->y[j];
        }
    }
#if !defined(NO_GRIDUTILS)
    else if (g->htype == GRIDHTYPE_CURVILINEAR) {
        gridnodes* gn = ((gxy_curv*) g->gridnodes_xy)->gn;

        if (i < 0 || j < 0 || i >= gridnodes_getnce1(gn) || j >= gridnodes_getnce2(gn)) {
            *x = NAN;
            *y = NAN;
        } else {
            *x = gridnodes_getx(gn)[j][i];
            *y = gridnodes_gety(gn)[j][i];
        }
    }
#endif
    else
        enkf_quit("programming error");
}

/**
 */
void grid_fk2z(grid* g, int i, int j, double fk, double* z)
{
    if (g->vtype == GRIDVTYPE_Z || g->vtype == GRIDVTYPE_SIGMA) {
        gz_simple* nodes = g->gridnodes_z;
        double* zc = nodes->zc;
        int nt = nodes->nz;

        fk += 0.5;

        if (fk <= 0.0)
            *z = zc[0];
        else if (fk >= nt)
            *z = zc[nt];
        else {
            int k = (int) floor(fk);

            *z = zc[k] + (fk - (double) k) * (zc[k + 1] - zc[k]);
        }
        if (g->vtype == GRIDVTYPE_SIGMA)
            *z *= g->depth[j][i];
    } else
        enkf_quit("not implemented");
}

/**
 */
int grid_isperiodic_x(grid* g)
{
    if (g->htype == GRIDHTYPE_LATLON)
        return ((gxy_simple*) ((grid*) g)->gridnodes_xy)->periodic_x;

    return 0;
}

/**
 */
void grid_settocartesian_fn(grid* g, grid_tocartesian_fn fn)
{
    g->tocartesian_fn = fn;
}

/**
 */
void grid_tocartesian(grid* g, double* in, double* out)
{
    g->tocartesian_fn(in, out);
}
