/*++

Module Name:

    WGSim.h

Abstract:

    Handle FASTQ ID strings like the ones generated by WGSim

Authors:

    Bill Bolosky, August, 2011

Environment:

    User mode service.

Revision History:

    Pulled from cSNAP to make it useful in more than one place

--*/

#pragma once

#include "GenomeIndex.h"
#include "Read.h"

// Is a wgsim-generated read mapped to a given location misaligned, given the source
// location encoded into its ID and a maximum edit distance maxK?
// Also optionally outputs the low and high location encoded in the wgsim read's ID.

bool wgsimReadMisaligned(Read *read, unsigned location, const Genome* genome, int maxK,
                         unsigned *lowOut = NULL, unsigned *highOut = NULL);

bool wgsimReadMisaligned(Read *read, unsigned location, GenomeIndex *index, int maxK,
                         unsigned *lowOut = NULL, unsigned *highOut = NULL);


// Write a wgsim-style id string.
// Convert from 0-based to 1-based coordinates.  If you're curious about the distinction, see:
// <http://www.biostars.org/post/show/6373/what-are-the-advantagesdisadvantages-of-one-based-vs-zero-based-genome-coordinate-systems/>
void wgsimGenerateIDString(const Genome::Contig *contig, unsigned offsetInContig,
                           unsigned readLength, bool firstHalf, char *outputBuffer);
void wgsimGenerateIDString(const Genome *genome, unsigned genomeLocation,
                           unsigned readLength, bool firstHalf, char *outputBuffer);
