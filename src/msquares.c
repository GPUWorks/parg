// MSQUARES :: https://github.com/prideout/parg
// Converts fp32 grayscale images, or 8-bit color images, into triangles.
//
// For grayscale images, a threshold is specified to determine insideness.
// Color images can be r8, rg16, rgb24, or rgba32.  In all cases, the exact
// color of each pixel determines insideness.  For a visual overview of
// the API, see:
//
//     https://....
//
// The MIT License
// Copyright (c) 2015 Philip Rideout

#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

// -----------------------------------------------------------------------------
// BEGIN PUBLIC API
// -----------------------------------------------------------------------------

typedef uint8_t par_byte;

typedef struct par_msquares_meshlist_s par_msquares_meshlist;

// Encapsulates the results of a marching squares operation.
typedef struct {
    float* points;        // pointer to XY (or XYZ) vertex coordinates
    int npoints;          // number of vertex coordinates
    uint16_t* triangles;  // pointer to 3-tuples of vertex indices
    int ntriangles;       // number of 3-tuples
    int dim;              // number of floats per point (either 2 or 3)
} par_msquares_mesh;

#define PAR_MSQUARES_INVERT (1 << 0)
#define PAR_MSQUARES_DUAL (1 << 1)
#define PAR_MSQUARES_WELD (1 << 2)
#define PAR_MSQUARES_CONNECT (1 << 3)
#define PAR_MSQUARES_SIMPLIFY (1 << 4)
#define PAR_MSQUARES_HEIGHTS (1 << 5)

par_msquares_meshlist* par_msquares_from_grayscale(float const* data, int width,
    int height, int cellsize, float threshold, int flags);

par_msquares_meshlist* par_msquares_from_levels(float const* data, int width,
    int height, int cellsize, float const* thresholds, int nthresholds,
    int flags);

par_msquares_meshlist* par_msquares_from_color(par_byte const* data, int width,
    int height, int cellsize, par_byte color, int bpp, int flags);

par_msquares_meshlist* par_msquares_from_colors(par_byte const* data, int width,
    int height, int cellsize, par_byte const* colors, int ncolors, int bpp,
    int flags);

par_msquares_mesh* par_msquares_get_mesh(par_msquares_meshlist*, int n);

int par_msquares_get_count(par_msquares_meshlist*);

void par_msquares_free(par_msquares_meshlist*);

// -----------------------------------------------------------------------------
// END PUBLIC API
// -----------------------------------------------------------------------------

#define MIN(a, b) (a > b ? b : a)
#define MAX(a, b) (a > b ? a : b)
#define CLAMP(v, lo, hi) MAX(lo, MIN(hi, v))

struct par_msquares_meshlist_s {
    int nmeshes;
    par_msquares_mesh** meshes;
};

static int** point_table = 0;
static int** triangle_table = 0;

void init_tables()
{
    point_table = malloc(16 * sizeof(int*));
    triangle_table = malloc(16 * sizeof(int*));

    char const* CODE_TABLE =
        "0 0\n"
        "1 1 0 1 7\n"
        "2 1 1 2 3\n"
        "3 2 0 2 3 3 7 0\n"
        "4 1 7 5 6\n"
        "5 2 0 1 5 5 6 0\n"
        "6 2 1 2 3 7 5 6\n"
        "7 3 0 2 3 0 3 5 0 5 6\n"
        "8 1 3 4 5\n"
        "9 2 0 1 7 3 4 5\n"
        "a 2 1 2 4 4 5 1\n"
        "b 3 0 2 4 0 4 5 0 5 7\n"
        "c 2 7 3 4 4 6 7\n"
        "d 3 0 1 3 0 3 4 0 4 6\n"
        "e 3 1 2 4 1 4 6 1 6 7\n"
        "f 2 0 2 4 4 6 0\n";

    char const* table_token = CODE_TABLE;
    for (int i = 0; i < 16; i++) {
        char code = *table_token;
        assert(i == code - (code >= 'a' ? ('a' - 0xa) : '0'));
        table_token += 2;
        int ntris = *table_token - '0';
        table_token += 2;
        int* sqrtris = triangle_table[i] =
                malloc((ntris + 1) * 3 * sizeof(int));
        sqrtris[0] = ntris;
        int mask = 0;
        int* sqrpts = point_table[i] = malloc(7 * sizeof(int));
        sqrpts[0] = 0;
        for (int j = 0; j < ntris * 3; j++, table_token += 2) {
            int midp = *table_token - '0';
            int bit = 1 << midp;
            if (!(mask & bit)) {
                mask |= bit;
                sqrpts[++sqrpts[0]] = midp;
            }
            sqrtris[j + 1] = midp;
        }
    }
}

par_msquares_meshlist* par_msquares_from_grayscale(float const* data, int width,
    int height, int cellsize, float threshold, int flags)
{
    assert(width > 0 && width % cellsize == 0);
    assert(height > 0 && height % cellsize == 0);

    // Create the two code tables if we haven't already.  These are tables of
    // fixed constants, so it's embarassing that we use dynamic memory
    // allocation for them.  However it's easy and it's one-time-only.

    if (!point_table) {
        init_tables();
    }

    // Allocate the meshlist and the first mesh.

    par_msquares_meshlist* mlist = malloc(sizeof(par_msquares_meshlist));
    mlist->nmeshes = 1;
    mlist->meshes = malloc(sizeof(par_msquares_mesh*));
    mlist->meshes[0] = malloc(sizeof(par_msquares_mesh));
    par_msquares_mesh* mesh = mlist->meshes[0];
    mesh->dim = 3;
    int ncols = width / cellsize;
    int nrows = height / cellsize;
    int ncorners = (ncols + 1) * (nrows + 1);

    // Worst case is three triangles and six verts per cell, so allocate that
    // much.

    int maxtris = ncols * nrows * 3;
    uint16_t* tris = malloc(maxtris * 3 * sizeof(uint16_t));
    int ntris = 0;
    int maxpts = ncols * nrows * 6;
    float* pts = malloc(maxpts * mesh->dim * sizeof(float));
    int npts = 0;

    // The "verts" x/y/z arrays are the 4 corners and 4 midpoints around the
    // square,
    // in counter-clockwise order.  The origin of "triangle space" is at the
    // lower-left, although we expect the image data to be in raster order
    // (starts at top-left).

    float normalized_cellsize = (float) cellsize / MAX(width, height);
    int maxrow = (height - 1) * width;
    uint16_t* ptris = tris;
    float* ppts = pts;
    float vertsx[8], vertsy[8], vertsz[8];
    for (int i = 0; i < 8; i++) {
        vertsz[0] = 0;
    }

    // Do the march!

    for (int row = 0; row < nrows; row++) {
        vertsx[0] = vertsx[6] = vertsx[7] = 0;
        vertsx[1] = vertsx[5] = 0.5 * normalized_cellsize;
        vertsx[2] = vertsx[3] = vertsx[4] = normalized_cellsize;
        vertsy[0] = vertsy[1] = vertsy[2] = normalized_cellsize * (row + 1);
        vertsy[4] = vertsy[5] = vertsy[6] = normalized_cellsize * row;
        vertsy[3] = vertsy[7] = normalized_cellsize * (row + 0.5);

        int northi = row * cellsize * width;
        int southi = MIN(northi + cellsize * width, maxrow);
        int northwest = data[northi] > threshold;
        int southwest = data[southi] > threshold;
        int previnds[8] = {0};
        int prevmask = 0;

        for (int col = 0; col < ncols; col++) {
            northi += cellsize;
            southi += cellsize;
            if (col == ncols - 1) {
                northi--;
                southi--;
            }

            int northeast = data[northi] > threshold;
            int southeast = data[southi] > threshold;
            int code = southwest | (southeast << 1) | (northwest << 2) |
                (northeast << 3);

            int const* pointspec = point_table[code];
            int ptspeclength = *pointspec++;
            int currinds[8] = {0};
            int mask = 0;
            while (ptspeclength--) {
                int midp = *pointspec++;
                int bit = 1 << midp;
                mask |= bit;
                if (bit == 1 && (prevmask & 4)) {
                    currinds[midp] = previnds[2];
                    continue;
                }
                if (bit == 128 && (prevmask & 8)) {
                    currinds[midp] = previnds[3];
                    continue;
                }
                if (bit == 64 && (prevmask & 16)) {
                    currinds[midp] = previnds[4];
                    continue;
                }
                *ppts++ = vertsx[midp];
                *ppts++ = vertsy[midp];
                if (mesh->dim == 3) {
                    *ppts++ = vertsz[midp];
                }
                currinds[midp] = npts++;
            }

            int const* trianglespec = triangle_table[code];
            int trispeclength = *trianglespec++;
            while (trispeclength--) {
                int a = *trianglespec++;
                int b = *trianglespec++;
                int c = *trianglespec++;
                *ptris++ = currinds[c];
                *ptris++ = currinds[b];
                *ptris++ = currinds[a];
                ntris++;
            }

            prevmask = mask;
            northwest = northeast;
            southwest = southeast;
            for (int i = 0; i < 8; i++) {
                previnds[i] = currinds[i];
                vertsx[i] += normalized_cellsize;
            }
        }
    }

    assert(npts <= maxpts);
    assert(ntris <= maxtris);
    mesh->npoints = npts;
    mesh->points = pts;
    mesh->ntriangles = ntris;
    mesh->triangles = tris;
    for (int i = 0; i < 16; i++) {
        free(point_table[i]);
        free(triangle_table[i]);
    }
    return mlist;
}

par_msquares_mesh* par_msquares_get_mesh(
    par_msquares_meshlist* mlist, int mindex)
{
    assert(mlist && mindex < mlist->nmeshes);
    return mlist->meshes[mindex];
}

int par_msquares_get_count(par_msquares_meshlist* mlist)
{
    assert(mlist);
    return mlist->nmeshes;
}

void par_msquares_free(par_msquares_meshlist* mlist)
{
    par_msquares_mesh** meshes = mlist->meshes;
    for (int i = 0; i < mlist->nmeshes; i++) {
        free(meshes[i]->points);
        free(meshes[i]->triangles);
        free(meshes[i]);
    }
    free(meshes);
    free(mlist);
}
