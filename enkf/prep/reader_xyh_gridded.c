/******************************************************************************
 *
 * File:        reader_xyh_gridded.c        
 *
 * Created:     02/01/2018
 *
 * Author:      Pavel Sakov
 *              Bureau of Meteorology
 *
 * Description: Generic reader for 3D gridded observations with hybrid vertical
 *              coordinate. Adopted from reader_xyz_gridded.c. Assumes
 *              
 *                p[k] = a[k] + b[k] * (p1 - p2),
 *
 *              where p(k) -- z coordinate (pressure) at the middle of layer k.
 *              The grid must be defined in the grid parameter file.
 *
 *              Unlike other readers this one requires the (hybrid) data grid
 *              to be defined in the grid parameter file.
 *
 *              It is currently assumed that there is only one data record
 *              (a 3D field).
 *
 *              Similarly to reader_xy_gridded(), there are a number of
 *              parameters that must (++) or can be specified if they differ
 *              from the default value (+). Some parameters are optional (-):
 *              - VARNAME (++)
 *              - GRIDNAME (++)
 *              - TIMENAME ("*[tT][iI][mM][eE]*") (+)
 *              - or TIMENAMES (when time = base_time + offset) (+)
 *              - NPOINTSNAME ("npoints") (-)
 *                  number of collated points for each datum; used basically as
 *                  a data mask n = 0
 *              - STDNAME ("std") (-)
 *                  internal variability of the collated data
 *              - ESTDNAME ("error_std") (-)
 *                  error STD; if absent then needs to be specified externally
 *                  in the oobservation data parameter file
 *              - BATCHNAME ("batch") (-)
 *                  name of the variable used for batch ID
 *              - VARSHIFT (-)
 *                  data offset to be added
 *              - MINDEPTH (-)
 *                  minimal allowed depth
 *              - MAXDEPTH (-)
 *                  maximal allowed depth
 *              - INSTRUMENT (-)
 *                  instrument string that will be used for calculating
 *                  instrument stats
 *              - QCFLAGNAME (-)
 *                  name of the QC flag variable, 0 <= qcflag <= 31
 *              - QCFLAGVALS (-)
 *                  the list of allowed values of QC flag variable
 *              Note: it is possible to have multiple entries of QCFLAGNAME and
 *                QCFLAGVALS combination, e.g.:
 *                  PARAMETER QCFLAGNAME = TEMP_quality_control
 *                  PARAMETER QCFLAGVALS = 1
 *                  PARAMETER QCFLAGNAME = DEPTH_quality_control
 *                  PARAMETER QCFLAGVALS = 1
 *                  PARAMETER QCFLAGNAME = LONGITUDE_quality_control
 *                  PARAMETER QCFLAGVALS = 1,8
 *                  PARAMETER QCFLAGNAME = LATITUDE_quality_control
 *                  PARAMETER QCFLAGVALS = 1,8
 *                An observation is considered valid if each of the specified
 *                flags takes a permitted value.
 *
 * Revisions:   PS 6/7/2018
 *                Added parameters QCFLAGNAME and QCFLAGVALS. The latter is
 *                supposed to contain a list of allowed flag values.
 *
 *****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "ncw.h"
#include "ncutils.h"
#include "definitions.h"
#include "utils.h"
#include "obsprm.h"
#include "model.h"
#include "grid.h"
#include "observations.h"
#include "prep_utils.h"
#include "allreaders.h"

#define TYPE_DOUBLE 0
#define TYPE_SHORT 1

/**
 */
void reader_xyh_gridded(char* fname, int fid, obsmeta* meta, grid* gdst, observations* obs)
{
    char* varname = NULL;
    char* gridname = NULL;
    grid* gsrc = NULL;

    char* npointsname = NULL;
    char* stdname = NULL;
    char* estdname = NULL;
    char* batchname = NULL;
    char instrument[MAXSTRLEN] = "";

    int nqcflagvars = 0;
    char** qcflagvarnames = NULL;
    uint32_t* qcflagmasks = 0;

    int ni = 0, nj = 0, nk = 0;
    size_t nij = 0, nijk = 0;

    int instid = -1;
    int productid = -1;
    int typeid = -1;

    int ncid;
    int ndim;
    float* var = NULL;
    double var_estd = NAN;
    short* npoints = NULL;
    float* std = NULL;
    float* estd = NULL;
    int* batch = NULL;
    uint32_t** qcflag = NULL;
    size_t ntime = 0;
    double* time = NULL;
    int varid;
    size_t i, j, k, nobs_read;

    for (i = 0; i < meta->npars; ++i) {
        if (strcasecmp(meta->pars[i].name, "VARNAME") == 0)
            varname = meta->pars[i].value;
        else if (strcasecmp(meta->pars[i].name, "GRIDNAME") == 0)
            gridname = meta->pars[i].value;
        else if (strcasecmp(meta->pars[i].name, "NPOINTSNAME") == 0)
            npointsname = meta->pars[i].value;
        else if (strcasecmp(meta->pars[i].name, "STDNAME") == 0)
            stdname = meta->pars[i].value;
        else if (strcasecmp(meta->pars[i].name, "ESTDNAME") == 0)
            estdname = meta->pars[i].value;
        else if (strcasecmp(meta->pars[i].name, "BATCHNAME") == 0)
            batchname = meta->pars[i].value;
        else if (strcasecmp(meta->pars[i].name, "INSTRUMENT") == 0)
            strncpy(instrument, meta->pars[i].value, MAXSTRLEN - 1);
        else if (strcasecmp(meta->pars[i].name, "TIMENAME") == 0 || strcasecmp(meta->pars[i].name, "TIMENAMES") == 0)
            /*
             * TIMENAME and TIMENAMES are dealt with separately
             */
            ;
        else if (strcasecmp(meta->pars[i].name, "QCFLAGNAME") == 0 || strcasecmp(meta->pars[i].name, "QCFLAGVARNAME") == 0 || strcasecmp(meta->pars[i].name, "QCFLAGVALS") == 0)
            /*
             * QCFLAGNAME and QCFLAGVALS are dealt with separately
             */
            ;
        else
            enkf_quit("unknown PARAMETER \"%s\"\n", meta->pars[i].name);
    }

    if (varname == NULL)
        enkf_quit("reader_xyh_gridded(): %s: VARNAME not specified", fname);
    else
        enkf_printf("        VARNAME = %s\n", varname);
    if (gridname == NULL)
        enkf_quit("reader_xyh_gridded(): %s: GRIDNAME not specified", fname);
    else
        enkf_printf("        GRIDNAME = %s\n", gridname);

    gsrc = model_getgridbyname(obs->model, gridname);

    grid_getsize(gsrc, &ni, &nj, &nk);
    nij = ni * nj;
    nijk = nij * nk;

    ncw_open(fname, NC_NOWRITE, &ncid);

    ncw_inq_varid(ncid, varname, &varid);
    ncw_inq_varndims(ncid, varid, &ndim);
    {
        int dimid[4];
        size_t dimlen[4];

        ncw_inq_vardimid(ncid, varid, dimid);
        for (i = 0; i < ndim; ++i)
            ncw_inq_dimlen(ncid, dimid[i], &dimlen[i]);
        if (ndim == 4) {
            if (dimlen[0] != 1)
                enkf_quit("reader_xyh_gridded(): %d records (currently only one record is allowed)", dimlen[0]);
        } else if (ndim != 3)
            enkf_quit("reader_xyh_gridded(): %s: # dimensions = %d (must be 3 or 4 with a single record)", fname, ndim);

        if (dimlen[ndim - 1] != ni || dimlen[ndim - 2] != nj || dimlen[ndim - 3] != nk)
            enkf_quit("dimension mismatch between grid \"%s\" (%d x %d x %d) and variable \"%s\" in \"%s\" (%d x %d x %d)", gridname, ni, nj, nk, varname, fname, dimlen[ndim - 1], dimlen[ndim - 2], dimlen[ndim - 3]);
    }

    var = malloc(nijk * sizeof(float));
    ncu_readvarfloat(ncid, varid, nijk, var);

    /*
     * npoints
     */
    varid = -1;
    if (npointsname != NULL)
        ncw_inq_varid(ncid, npointsname, &varid);
    else if (ncw_var_exists(ncid, "npoints"))
        ncw_inq_varid(ncid, "npoints", &varid);
    if (varid >= 0) {
        npoints = malloc(nijk * sizeof(short));
        ncw_get_var_short(ncid, varid, npoints);
    }

    /*
     * std
     */
    varid = -1;
    if (stdname != NULL)
        ncw_inq_varid(ncid, stdname, &varid);
    else if (ncw_var_exists(ncid, "std"))
        ncw_inq_varid(ncid, "std", &varid);
    if (varid >= 0) {
        std = malloc(nijk * sizeof(float));
        ncu_readvarfloat(ncid, varid, nijk, std);
    }

    /*
     * etsd
     */
    varid = -1;
    if (estdname != NULL)
        ncw_inq_varid(ncid, estdname, &varid);
    else if (ncw_var_exists(ncid, "error_std"))
        ncw_inq_varid(ncid, "error_std", &varid);
    if (varid >= 0) {
        estd = malloc(nijk * sizeof(float));
        ncu_readvarfloat(ncid, varid, nijk, estd);
    }

    if (std == NULL && estd == NULL) {
        ncw_inq_varid(ncid, varname, &varid);
        if (ncw_att_exists(ncid, varid, "error_std")) {
            ncw_check_attlen(ncid, varid, "error_std", 1);
            ncw_get_att_double(ncid, varid, "error_std", &var_estd);
        }
    }

    /*
     * batch
     */
    varid = -1;
    if (batchname != NULL)
        ncw_inq_varid(ncid, batchname, &varid);
    else if (ncw_var_exists(ncid, "batch"))
        ncw_inq_varid(ncid, "batch", &varid);
    if (varid >= 0) {
        ncw_check_varsize(ncid, varid, nijk);
        batch = malloc(nijk * sizeof(int));
        ncw_get_var_int(ncid, varid, batch);
    }

    /*
     * qcflag
     */
    get_qcflags(meta, &nqcflagvars, &qcflagvarnames, &qcflagmasks);
    if (nqcflagvars > 0) {
        qcflag = alloc2d(nqcflagvars, nijk, sizeof(int32_t));
        for (i = 0; i < nqcflagvars; ++i) {
            ncw_inq_varid(ncid, qcflagvarnames[i], &varid);
            ncw_get_var_uint(ncid, varid, qcflag[i]);
        }
    }

    /*
     * time
     */
    get_time(meta, ncid, &ntime, &time);
    assert(ntime == nijk || ntime <= 1);

    /*
     * instrument
     */
    if (strlen(instrument) == 0 && !get_insttag(ncid, varname, instrument))
        strncpy(instrument, meta->product, MAXSTRLEN - 1);

    ncw_close(ncid);

    instid = st_add_ifabsent(obs->instruments, instrument, -1);
    productid = st_findindexbystring(obs->products, meta->product);
    assert(productid >= 0);
    typeid = obstype_getid(obs->nobstypes, obs->obstypes, meta->type, 1);
    nobs_read = 0;
    for (i = 0; i < ni; ++i) {
        for (j = 0; j < nj; ++j) {
            for (k = 0; k < nk; ++k) {
                int ii = k * nij + j * ni + i;
                observation* o;
                int qcid;

                if ((npoints != NULL && npoints[ii] == 0) || isnan(var[ii]) || (std != NULL && isnan(std[ii])) || (estd != NULL && isnan(estd[ii])) || (ntime == nijk && isnan(time[ii])))
                    continue;
                for (qcid = 0; qcid < nqcflagvars; ++qcid)
                    if (!((1 << qcflag[qcid][ii]) & qcflagmasks[qcid]))
                        goto nextob;

                nobs_read++;
                obs_checkalloc(obs);
                o = &obs->data[obs->nobs];

                o->product = productid;
                o->type = typeid;
                o->instrument = instid;
                o->id = obs->nobs;
                o->fid = fid;
                o->batch = (batch == NULL) ? 0 : batch[ii];
                o->value = (double) var[ii];
                if (estd == NULL)
                    o->estd = var_estd;
                else {
                    if (std == NULL)
                        o->estd = estd[ii];
                    else
                        o->estd = (std[ii] > estd[ii]) ? std[ii] : estd[ii];
                }
                {
                    double lon_d, lat_d;

                    grid_ij2xy(gsrc, i, j, &lon_d, &lat_d);
                    o->lon = (float) lon_d;
                    o->lat = (float) lat_d;
                }
                assert(isfinite(o->lon + o->lat));
                o->status = grid_xy2fij_f(gdst, o->lon, o->lat, &o->fi, &o->fj);
                if (!obs->allobs && o->status == STATUS_OUTSIDEGRID)
                    continue;
                {
                    double depth_d;

                    o->status = grid_fk2z(gsrc, i, j, (double) k, &depth_d);
                    o->depth = (float) depth_d;
                }
                if (o->status == STATUS_OK)
                    o->status = grid_z2fk_f(gdst, o->fi, o->fj, o->depth, &o->fk);
                else
                    o->fk = NAN;
                o->model_depth = NAN;   /* set in obs_add() */
                if (ntime > 0)
                    o->time = (ntime == 1) ? time[0] : time[ii];
                else
                    o->time = NAN;

                o->aux = -1;

                obs->nobs++;
              nextob:
                ;
            }
        }
    }
    enkf_printf("        nobs = %d\n", nobs_read);

    free(var);
    if (std != NULL)
        free(std);
    if (estd != NULL)
        free(estd);
    if (batch != NULL)
        free(batch);
    if (npoints != NULL)
        free(npoints);
    if (time != NULL)
        free(time);
    if (nqcflagvars > 0) {
        free(qcflagvarnames);
        free(qcflagmasks);
        free(qcflag);
    }
}
