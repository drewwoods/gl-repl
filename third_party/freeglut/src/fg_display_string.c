/*
 * fg_display_string.c
 *
 * Shared glutInitDisplayString criteria model: default resolution, hard
 * filtering, and left-to-right lexicographic ranking. Backends supply a
 * per-capability value array (indexed by FGCapability) and reuse this logic
 * so matching semantics stay identical across platforms.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#define FREEGLUT_BUILDING_LIB
#include <GL/freeglut.h>
#include "fg_internal.h"

FGCriterion fghMakeCriterion( FGCriterionComparison comparison, int value )
{
    FGCriterion c;

    c.comparison = comparison;
    c.value      = value;
    return c;
}

GLboolean fghCriterionIsConcrete( FGCriterionComparison comparison )
{
    /* Concrete comparators are the contiguous range FG_EQ..FG_MIN; this also
     * rejects the (FGCriterionComparison)(-1) invalid sentinel. */
    return ( comparison >= FG_EQ && comparison <= FG_MIN ) ? GL_TRUE : GL_FALSE;
}

void fghAddCriterion( FGDisplayStringCriteria *c, FGCapability capability,
                      FGCriterion parsed, FGCriterion deflt )
{
    if ( c->count >= FG_MAX_DISPLAY_CRITERIA )
        return;

    c->entries[ c->count ].capability = capability;
    c->entries[ c->count ].criterion  =
        fghCriterionIsConcrete( parsed.comparison ) ? parsed : deflt;
    c->count++;
}

static GLboolean fghCriterionPasses( FGCriterion crit, int fbvalue )
{
    switch ( crit.comparison ) {
    case FG_EQ:   return ( fbvalue == crit.value ) ? GL_TRUE : GL_FALSE;
    case FG_NEQ:  return ( fbvalue != crit.value ) ? GL_TRUE : GL_FALSE;
    case FG_LT:   return ( fbvalue <  crit.value ) ? GL_TRUE : GL_FALSE;
    case FG_GT:   return ( fbvalue >  crit.value ) ? GL_TRUE : GL_FALSE;
    case FG_LTE:  return ( fbvalue <= crit.value ) ? GL_TRUE : GL_FALSE;
    case FG_GTE:  return ( fbvalue >= crit.value ) ? GL_TRUE : GL_FALSE;
    case FG_MIN:  return ( fbvalue >= crit.value ) ? GL_TRUE : GL_FALSE; /* ~ */
    default:      return GL_TRUE;  /* FG_NONE / unresolved: no constraint */
    }
}

GLboolean fghCriteriaPass( const FGDisplayStringCriteria *c, const int *values )
{
    int i;

    for ( i = 0; i < c->count; i++ ) {
        const FGCapabilityCriterion *e = &c->entries[ i ];

        if ( !fghCriterionPasses( e->criterion, values[ e->capability ] ) )
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* Per the man page: > and >= prefer more, < <= and ~ prefer less, = and !=
 * are exact (no ranking contribution). */
static int fghPreferenceDir( FGCriterionComparison comparison )
{
    switch ( comparison ) {
    case FG_GT:
    case FG_GTE:
        return 1;
    case FG_LT:
    case FG_LTE:
    case FG_MIN:
        return -1;
    default:
        return 0;
    }
}

int fghCriteriaCompare( const FGDisplayStringCriteria *c,
                        const int *aValues, const int *bValues )
{
    int i;

    for ( i = 0; i < c->count; i++ ) {
        const FGCapabilityCriterion *e = &c->entries[ i ];
        int dir = fghPreferenceDir( e->criterion.comparison );
        int va, vb;

        if ( dir == 0 )
            continue;

        va = aValues[ e->capability ];
        vb = bValues[ e->capability ];
        if ( va == vb )
            continue;

        if ( dir > 0 )
            return ( va > vb ) ? -1 : 1;
        return ( va < vb ) ? -1 : 1;
    }
    return 0;
}
