/*++

Module Name:

    PairedAligner.cpp

Abstract:

    Functions for running the paired end aligner sub-program.


Authors:

    Matei Zaharia, February, 2012

Environment:
`
    User mode service.

Revision History:

    Adapted from cSNAP, which was in turn adapted from the scala prototype

--*/

//
// TODO: This is really similar to the single-end aligner overall. It would be nice
// to avoid code duplication.
//

#include "stdafx.h"
#include "options.h"
#include <time.h>
#include "Compat.h"
#include "RangeSplitter.h"
#include "GenomeIndex.h"
#include "Range.h"
#include "SAM.h"
#include "ChimericPairedEndAligner.h"
#include "Tables.h"
#include "WGsim.h"
#include "AlignerOptions.h"
#include "AlignerContext.h"
#include "AlignerStats.h"
#include "FASTQ.h"
#include "PairedAligner.h"
#include "MultiInputReadSupplier.h"
#include "Util.h"
#include "IntersectingPairedEndAligner.h"
#include "exit.h"
#include "Error.h"
#include "AlignmentFilter.h"

using namespace std;

using util::stringEndsWith;

static const int DEFAULT_MIN_SPACING = 50;
static const int DEFAULT_MAX_SPACING = 1000;

struct PairedAlignerStats : public AlignerStats
{
    // TODO: make these constants configurable
    static const int MAX_DISTANCE = 1000;
    static const int MAX_SCORE = 15;

    _int64 sameComplement;
    _int64* distanceCounts; // histogram of distances
    // TODO: could save a bit of memory & time since this is a triangular matrix
    _int64* scoreCounts; // 2-d histogram of scores for paired ends
    static const unsigned maxMapq = 70;
    static const unsigned nTimeBuckets = 32;
    static const unsigned nHitsBuckets = 32;
    static const unsigned nLVCallsBuckets = 32;

    _int64 alignTogetherByMapqHistogram[maxMapq+1][nTimeBuckets];
    _int64 totalTimeByMapqHistogram[maxMapq+1][nTimeBuckets];
    _int64 nSmallHitsByTimeHistogram[nHitsBuckets][nTimeBuckets];
    _int64 nLVCallsByTimeHistogram[nLVCallsBuckets][nTimeBuckets];
    _int64 mapqByNLVCallsHistogram[maxMapq+1][nLVCallsBuckets];
    _int64 mapqByNSmallHitsHistogram[maxMapq+1][nHitsBuckets];

    PairedAlignerStats(AbstractStats* i_extra = NULL);

    virtual ~PairedAlignerStats();

    inline void incrementDistance(int distance) {
        distanceCounts[max(0, min(MAX_DISTANCE, distance))]++;
    }

    inline void incrementScore(int s0, int s1)
    {
        // ensure s0 <= s1, both within range
        s0 = max(0, min(MAX_SCORE, s0));
        s1 = max(0, min(MAX_SCORE, s1));
        if (s0 > s1) {
            int t = s0; s0 = s1; s1 = t;
        }
        scoreCounts[s0*(MAX_SCORE+1)+s1]++;
    }

    inline void recordAlignTogetherMapqAndTime(unsigned mapq, _int64 timeInNanos, unsigned nSmallHits, unsigned nLVCalls) {
        int timeBucket;
        _int64 dividedTime = timeInNanos;
        for (timeBucket = 0; timeBucket < nTimeBuckets-1; timeBucket++) {
            if (dividedTime == 0) break;
            dividedTime /= 2;
        }

        alignTogetherByMapqHistogram[mapq][timeBucket]++;
        totalTimeByMapqHistogram[mapq][timeBucket] += timeInNanos;

        int nHitsBucket;
        int dividedHits = nSmallHits;
        for (nHitsBucket = 0; nHitsBucket < nHitsBuckets; nHitsBucket++) {
            if (0 == dividedHits) break;
            dividedHits /= 2;
        }
        _ASSERT((char *)&nSmallHitsByTimeHistogram[nHitsBucket][timeBucket] < (char *)(this + 1));
        nSmallHitsByTimeHistogram[nHitsBucket][timeBucket]++;

        int nLVCallsBucket;
        int dividedLVCalls = nLVCalls;
        for (nLVCallsBucket = 0; nLVCallsBucket < nLVCallsBuckets; nLVCallsBucket++) {
            if (dividedLVCalls == 0) break;
            dividedLVCalls /= 2;
        }
        _ASSERT((char *)&nLVCallsByTimeHistogram[nLVCallsBucket][timeBucket] < (char *)(this + 1));
        nLVCallsByTimeHistogram[nLVCallsBucket][timeBucket]++;

        _ASSERT((char *)&mapqByNLVCallsHistogram[mapq][nLVCallsBucket] < (char *)(this + 1));
        mapqByNLVCallsHistogram[mapq][nLVCallsBucket]++;

        _ASSERT((char *)&mapqByNSmallHitsHistogram[mapq][nHitsBucket] < (char *)(this + 1));
        mapqByNSmallHitsHistogram[mapq][nHitsBucket]++;
    }



    virtual void add(const AbstractStats * other);

    virtual void printHistograms(FILE* output);
};

const int PairedAlignerStats::MAX_DISTANCE;
const int PairedAlignerStats::MAX_SCORE;

PairedAlignerStats::PairedAlignerStats(AbstractStats* i_extra)
    : AlignerStats(i_extra),
    sameComplement(0)
{
    int dsize = sizeof(_int64) * (MAX_DISTANCE+1);
    distanceCounts = (_int64*)BigAlloc(dsize);
    memset(distanceCounts, 0, dsize);

    int ssize = sizeof(_int64) * (MAX_SCORE+1)*(MAX_SCORE+1);
    scoreCounts = (_int64*)BigAlloc(ssize);
    memset(scoreCounts, 0, ssize);

    for (unsigned mapq = 0; mapq <= maxMapq; mapq++) {
        for (unsigned timeBucket = 0; timeBucket < nTimeBuckets; timeBucket++) {
            alignTogetherByMapqHistogram[mapq][timeBucket] = 0;
            totalTimeByMapqHistogram[mapq][timeBucket] = 0;
        }
        for (unsigned smallHits = 0; smallHits < nHitsBuckets; smallHits++) {
            mapqByNSmallHitsHistogram[mapq][smallHits] = 0;
        }
        for (unsigned lvCalls = 0; lvCalls < nLVCallsBuckets; lvCalls++) {
            mapqByNLVCallsHistogram[mapq][lvCalls] = 0;
        }
    }

    for (unsigned timeBucket = 0; timeBucket < nTimeBuckets; timeBucket++) {
        for (unsigned smallHits = 0; smallHits < nHitsBuckets; smallHits++) {
            nSmallHitsByTimeHistogram[smallHits][timeBucket] = 0;
        }
        for (unsigned lvCalls = 0; lvCalls < nLVCallsBuckets; lvCalls++) {
            nLVCallsByTimeHistogram[lvCalls][timeBucket] = 0;
        }
    }

}

PairedAlignerStats::~PairedAlignerStats()
{
    BigDealloc(distanceCounts);
    BigDealloc(scoreCounts);
}

void PairedAlignerStats::add(const AbstractStats * i_other)
{
    AlignerStats::add(i_other);
    PairedAlignerStats* other = (PairedAlignerStats*) i_other;
    for (int i = 0; i < MAX_DISTANCE + 1; i++) {
        distanceCounts[i] += other->distanceCounts[i];
    }
    for (int i = 0; i < (MAX_SCORE + 1) * (MAX_SCORE + 1); i++) {
        scoreCounts[i] += other->scoreCounts[i];
    }

    for (unsigned mapq = 0; mapq <= maxMapq; mapq++) {
        for (unsigned timeBucket = 0; timeBucket < nTimeBuckets; timeBucket++) {
            alignTogetherByMapqHistogram[mapq][timeBucket] += other->alignTogetherByMapqHistogram[mapq][timeBucket];
            totalTimeByMapqHistogram[mapq][timeBucket] += other->totalTimeByMapqHistogram[mapq][timeBucket];
        }
        for (unsigned smallHits = 0; smallHits < nHitsBuckets; smallHits++) {
            mapqByNSmallHitsHistogram[mapq][smallHits] += other->mapqByNSmallHitsHistogram[mapq][smallHits];
        }
        for (unsigned lvCalls = 0; lvCalls < nLVCallsBuckets; lvCalls++) {
            mapqByNLVCallsHistogram[mapq][lvCalls] += other->mapqByNLVCallsHistogram[mapq][lvCalls];
        }
    }

    for (unsigned timeBucket = 0; timeBucket < nTimeBuckets; timeBucket++) {
        for (unsigned smallHits = 0; smallHits < nHitsBuckets; smallHits++) {
            nSmallHitsByTimeHistogram[smallHits][timeBucket] += other->nSmallHitsByTimeHistogram[smallHits][timeBucket];
        }
        for (unsigned lvCalls = 0; lvCalls < nLVCallsBuckets; lvCalls++) {
            nLVCallsByTimeHistogram[lvCalls][timeBucket] += other->nLVCallsByTimeHistogram[lvCalls][timeBucket];
        }
    }

}

void PairedAlignerStats::printHistograms(FILE* output)
{
    AlignerStats::printHistograms(output);
}

PairedAlignerOptions::PairedAlignerOptions(const char* i_commandLine)
    : AlignerOptions(i_commandLine, true),
    minSpacing(DEFAULT_MIN_SPACING),
    maxSpacing(DEFAULT_MAX_SPACING),
    forceSpacing(false),
    intersectingAlignerMaxHits(DEFAULT_INTERSECTING_ALIGNER_MAX_HITS),
    maxCandidatePoolSize(DEFAULT_MAX_CANDIDATE_POOL_SIZE),
    quicklyDropUnpairedReads(true)
{
}

void PairedAlignerOptions::usageMessage()
{
    AlignerOptions::usageMessage();
    WriteErrorMessage(
        "  -s   min and max spacing to allow between paired ends (default: %d %d).\n"
        "  -fs  force spacing to lie between min and max.\n"
        "  -H   max hits for intersecting aligner (default: %d).\n"
        "  -mcp specifies the maximum candidate pool size (An internal data structure. \n"
        "       Only increase this if you get an error message saying to do so. If you're running\n"
        "       out of memory, you may want to reduce it.  Default: %d)\n"
        "  -F b additional option to -F to require both mates to satisfy filter (default is just one)\n",
        "       out of memory, you may want to reduce it.  Default: %d).\n"
        "  -ku  Keep unpaired-looking reads in SAM/BAM input.  Ordinarily, if a read doesn't specify\n"
        "       mate information (RNEXT field is * and/or PNEXT is 0) then the code that matches reads will immdeiately\n"
        "       discard it.  Specifying this flag may cause large memory usage for some input files,\n"
        "       but may be necessary for some strangely formatted input files.  You'll also need to specify this\n"
        "       flag for SAM/BAM files that were aligned by a single-end aligner.\n"
        ,
        DEFAULT_MIN_SPACING,
        DEFAULT_MAX_SPACING,
        DEFAULT_INTERSECTING_ALIGNER_MAX_HITS,
        DEFAULT_MAX_CANDIDATE_POOL_SIZE);
}

bool PairedAlignerOptions::parse(const char** argv, int argc, int& n, bool *done)
{
    *done = false;

    if (strcmp(argv[n], "-s") == 0) {
        if (n + 2 < argc) {
            minSpacing = atoi(argv[n+1]);
            maxSpacing = atoi(argv[n+2]);
            n += 2;
            return true;
        } 
        return false;
    } else if (strcmp(argv[n], "-H") == 0) {
        if (n + 1 < argc) {
            intersectingAlignerMaxHits = atoi(argv[n+1]);
            n += 1;
            return true;
        } 
        return false;
    } else if (strcmp(argv[n], "-fs") == 0) {
        forceSpacing = true;
        return true;    
    } else if (strcmp(argv[n], "-ku") == 0) {
        quicklyDropUnpairedReads = false;
        return true;
    } else if (strcmp(argv[n], "-mcp") == 0) {
        if (n + 1 < argc) {
            maxCandidatePoolSize = atoi(argv[n+1]);
            n += 1;
            return true;
        } 
        return false;
    } else if (strcmp(argv[n], "-F") == 0 && n + 1 < argc && strcmp(argv[n + 1],"b") == 0) {
        filterFlags |= FilterBothMatesMatch;
        n += 1;
        return true;
    }
    return AlignerOptions::parse(argv, argc, n, done);
}

PairedAlignerContext::PairedAlignerContext(AlignerExtension* i_extension)
    : AlignerContext( 0,  NULL, NULL, i_extension)
{
}

/*
AlignerOptions* PairedAlignerContext::parseOptions(int i_argc, const char **i_argv, const char *i_version, unsigned *argsConsumed)
{
    argc = i_argc;
    argv = i_argv;
    version = i_version;

    PairedAlignerOptions* options = new PairedAlignerOptions(
        "snapr paired <genome-dir> <transcriptome-dir> <annotation> <input file(s)> <read2.fq> [<options>]\n"
        "   where <input file(s)> is a list of files to process.  FASTQ\n"
        "   files must come in pairs, since each read end is in a separate file.");
    options->extra = extension->extraOptions();
    if (argc < 4) {
        fprintf(stderr, "Too few parameters\n");
        options->usage();
    }

    options->indexDir = argv[0];
    options->transcriptomeDir = argv[1];
    options->annotation = argv[2];
    //
    // Figure out how many inputs there are.  All options begin with a '-', so count the
    // args until we hit an option.  FASTQ files come in pairs, and each pair only counts
    // as one input.
    //
    int nInputs = 0;
    bool foundFirstHalfOfFASTQ = false;
    for (int i = 3; i < argc; i++) {
        if (argv[i][0] == '-' || argv[i][0] == ',' && argv[i][1] == '\0') {
                break;
        }
        
        if (stringEndsWith(argv[i],".sam") || stringEndsWith(argv[i],".bam")) {
            if (foundFirstHalfOfFASTQ) {
                fprintf(stderr,"For the paired aligner, FASTQ files must come in pairs.  I found SAM/BAM file '%s' after first half FASTQ file '%s'.\n",
                    argv[i],argv[i-1]);
                soft_exit(1);
            }
            nInputs++;
        } else {
            if (foundFirstHalfOfFASTQ) {
                nInputs++;
            }
            foundFirstHalfOfFASTQ = !foundFirstHalfOfFASTQ;
        }
    }
    if (foundFirstHalfOfFASTQ) {
        fprintf(stderr,"For the paired aligner, FASTQ files must come in pairs.  The last one is unmatched.\n");
        soft_exit(1);
    }
    if (0 == nInputs) {
        fprintf(stderr,"Didn't see any input files\n");
        options->usage();
    }
    //
    // Now build the input array.
    //
    options->nInputs = nInputs;
    options->inputs = new SNAPInput[nInputs];
    int i;
    int whichInput = 0;
    for (i = 3; i < argc; i++) {
        if (argv[i][0] == '-') {
                break;
        }

        if (stringEndsWith(argv[i],".sam") || stringEndsWith(argv[i],".bam")) {
            _ASSERT(!foundFirstHalfOfFASTQ);
            options->inputs[whichInput].fileType = stringEndsWith(argv[i],".sam") ? SAMFile : BAMFile;
            options->inputs[whichInput].fileName = argv[i];
            whichInput++;
        } else {
            if (foundFirstHalfOfFASTQ) {
                options->inputs[whichInput].fileType =
                    stringEndsWith(argv[i],".gzip") || stringEndsWith(argv[i], ".gz") // todo: more suffixes?
                        ? GZipFASTQFile : FASTQFile;
                options->inputs[whichInput].secondFileName = argv[i];
                whichInput++;
            } else {
                options->inputs[whichInput].fileName = argv[i];
            }
            foundFirstHalfOfFASTQ = !foundFirstHalfOfFASTQ;
        }
    }

    for (; i < argc; i++) {
        bool done;
        int oldI = i;
        if (!options->parse(argv, argc, i, &done)) {
            fprintf(stderr, "Didn't understand options starting at %s\n", argv[oldI]);
            options->usage();
        }

        if (done) {
            i++;    // For the ',' arg
            break;
        }
    }

    if (options->maxDist.end + options->extraSearchDepth >= MAX_K) {
        fprintf(stderr,"You specified too large of a maximum edit distance combined with extra search depth.  The must add up to less than %d.\n", MAX_K);
        fprintf(stderr,"Either reduce their sum, or change MAX_K in LandauVishkin.h and recompile.\n");
        soft_exit(1);
    }
        
    *argsConsumed = i;
    return options;
}
*/

void PairedAlignerContext::initialize()
{
    AlignerContext::initialize();
    PairedAlignerOptions* options2 = (PairedAlignerOptions*) options;
    minSpacing = options2->minSpacing;
    maxSpacing = options2->maxSpacing;
    forceSpacing = options2->forceSpacing;
    maxCandidatePoolSize = options2->maxCandidatePoolSize;
    intersectingAlignerMaxHits = options2->intersectingAlignerMaxHits;
    ignoreMismatchedIDs = options2->ignoreMismatchedIDs;
    quicklyDropUnpairedReads = options2->quicklyDropUnpairedReads;
    noUkkonen = options->noUkkonen;
    noOrderedEvaluation = options->noOrderedEvaluation;
}

AlignerStats* PairedAlignerContext::newStats()
{
    return new PairedAlignerStats();
}

void PairedAlignerContext::runTask()
{
    ParallelTask<PairedAlignerContext> task(this);
    task.run();
}

void PairedAlignerContext::runIterationThread()
{
    PairedReadSupplier *supplier = pairedReadSupplierGenerator->generateNewPairedReadSupplier();

    if (NULL == supplier) {
        //
        // No work for this thread to do.
        //
        return;
    }
	if (extension->runIterationThread(supplier, this)) {
        delete supplier;
		return;
	}


 	if (index == NULL) {
        // no alignment, just input/output
        Read *read0;
        Read *read1;
        PairedAlignmentResult result;
        memset(&result, 0, sizeof(result));
        result.location[0] = result.location[1] = InvalidGenomeLocation;
        
        while (supplier->getNextReadPair(&read0,&read1)) {
            // Check that the two IDs form a pair; they will usually be foo/1 and foo/2 for some foo.
            if (!ignoreMismatchedIDs && !readIdsMatch(read0, read1)) {
                unsigned n[2] = {min(read0->getIdLength(), 200u), min(read1->getIdLength(), 200u)};
                char* p[2] = {(char*) alloca(n[0] + 1), (char*) alloca(n[1] + 1)};
                memcpy(p[0], read0->getId(), n[0]); p[0][n[0]] = 0;
                memcpy(p[1], read1->getId(), n[1]); p[1][n[1]] = 0;
                WriteErrorMessage( "Unmatched read IDs '%s' and '%s'.  Use the -I option to ignore this.\n", p[0], p[1]);
                soft_exit(1);
            }
            stats->totalReads += 2;


            writePair(read0, read1, &result, false);
        }
        delete supplier;
        return;
    }

    int maxReadSize = MAX_READ_LENGTH;
    size_t g_memoryPoolSize = IntersectingPairedEndAligner::getBigAllocatorReservation(index, intersectingAlignerMaxHits, maxReadSize, index->getSeedLength(), 
                                                                numSeedsFromCommandLine, seedCoverage, maxDist, extraSearchDepth, maxCandidatePoolSize);

    g_memoryPoolSize += ChimericPairedEndAligner::getBigAllocatorReservation(index, maxReadSize, maxHits, index->getSeedLength(), numSeedsFromCommandLine, seedCoverage, maxDist,
                                                    extraSearchDepth, maxCandidatePoolSize);

    unsigned g_maxPairedSecondaryHits;
    unsigned g_maxSingleSecondaryHits;

    if (maxSecondaryAligmmentAdditionalEditDistance < 0) {
        g_maxPairedSecondaryHits = 0;
        g_maxSingleSecondaryHits = 0;
    } else {
        g_maxPairedSecondaryHits = IntersectingPairedEndAligner::getMaxSecondaryResults(numSeedsFromCommandLine, seedCoverage, maxReadSize, maxHits, index->getSeedLength());
        g_maxSingleSecondaryHits = ChimericPairedEndAligner::getMaxSingleEndSecondaryResults(numSeedsFromCommandLine, seedCoverage, maxReadSize, maxHits, index->getSeedLength());
    }

    g_memoryPoolSize += g_maxPairedSecondaryHits * sizeof(PairedAlignmentResult) + g_maxSingleSecondaryHits * sizeof(SingleAlignmentResult);

    BigAllocator *g_allocator = new BigAllocator(g_memoryPoolSize);
    
    IntersectingPairedEndAligner *g_intersectingAligner = new (g_allocator) IntersectingPairedEndAligner(index, maxReadSize, maxHits, maxDist, numSeedsFromCommandLine, 
                                                                seedCoverage, minSpacing, maxSpacing, intersectingAlignerMaxHits, extraSearchDepth, 
                                                                maxCandidatePoolSize, g_allocator, noUkkonen, noOrderedEvaluation);

    ChimericPairedEndAligner *g_aligner = new (g_allocator) ChimericPairedEndAligner(
        index,
        maxReadSize,
        maxHits,
        maxDist,
        numSeedsFromCommandLine,
        seedCoverage,
        forceSpacing,
        extraSearchDepth,
        noUkkonen,
        noOrderedEvaluation,
        g_intersectingAligner,
        g_allocator);        

    g_allocator->checkCanaries();

    PairedAlignmentResult *g_secondaryResults = (PairedAlignmentResult *)g_allocator->allocate(g_maxPairedSecondaryHits * sizeof(*g_secondaryResults));
    SingleAlignmentResult *g_singleSecondaryResults = (SingleAlignmentResult *)g_allocator->allocate(g_maxSingleSecondaryHits * sizeof(*g_singleSecondaryResults));

    BigAllocator *c_allocator = NULL;
    IntersectingPairedEndAligner *c_intersectingAligner = NULL;
    ChimericPairedEndAligner *c_aligner = NULL;
    PairedAlignmentResult *c_secondaryResults = NULL;
    SingleAlignmentResult *c_singleSecondaryResults = NULL;

    if (contamination != NULL) {

      //Contamination database for paired end reads
      size_t c_memoryPoolSize = IntersectingPairedEndAligner::getBigAllocatorReservation(contamination, intersectingAlignerMaxHits, maxReadSize, contamination->getSeedLength(),
                                                                  numSeedsFromCommandLine, seedCoverage, maxDist, extraSearchDepth, maxCandidatePoolSize);

      c_memoryPoolSize += ChimericPairedEndAligner::getBigAllocatorReservation(contamination, maxReadSize, maxHits, contamination->getSeedLength(), numSeedsFromCommandLine, seedCoverage, maxDist, extraSearchDepth, maxCandidatePoolSize);

      if (maxSecondaryAligmmentAdditionalEditDistance < 0) {
          c_maxPairedSecondaryHits = 0;
          c_maxSingleSecondaryHits = 0;
      } else {
          c_maxPairedSecondaryHits = IntersectingPairedEndAligner::getMaxSecondaryResults(numSeedsFromCommandLine, seedCoverage, maxReadSize, maxHits, contamination->getSeedLength());
          c_maxSingleSecondaryHits = ChimericPairedEndAligner::getMaxSingleEndSecondaryResults(numSeedsFromCommandLine, seedCoverage, maxReadSize, maxHits, contamination->getSeedLength());
      }

      c_memoryPoolSize += c_maxPairedSecondaryHits * sizeof(PairedAlignmentResult) + c_maxSingleSecondaryHits * sizeof(SingleAlignmentResult);

      c_allocator = new BigAllocator(c_memoryPoolSize);
      
      c_intersectingAligner = new (c_allocator) IntersectingPairedEndAligner(contamination, maxReadSize, maxHits, maxDist, numSeedsFromCommandLine,
                                                                  seedCoverage, minSpacing, maxSpacing, intersectingAlignerMaxHits, extraSearchDepth,
                                                                  maxCandidatePoolSize, c_allocator, noUkkonen, noOrderedEvaluation);
      c_aligner = new (c_allocator) ChimericPairedEndAligner(
          contamination,
          maxReadSize,
          maxHits, 
          maxDist,
          numSeedsFromCommandLine,
          seedCoverage,
          forceSpacing,
          extraSearchDepth,
          noUkkonen,
          noOrderedEvaluation,
          c_intersectingAligner,
          c_allocator);

      c_allocator->checkCanaries();
      c_secondaryResults = (PairedAlignmentResult *)c_allocator->allocate(c_maxPairedSecondaryHits * sizeof(*c_secondaryResults));
      c_singleSecondaryResults = (SingleAlignmentResult *)c_allocator->allocate(c_maxSingleSecondaryHits * sizeof(*c_singleSecondaryResults));
    
    }

    unsigned singleAlignerMaxHits = 300;
    SingleAlignmentResult *t_secondaryAlignments = NULL;
    unsigned t_secondaryAlignmentBufferCount;
    if (maxSecondaryAligmmentAdditionalEditDistance < 0) {
        t_secondaryAlignmentBufferCount = 0;
    } else {
        t_secondaryAlignmentBufferCount = BaseAligner::getMaxSecondaryResults(numSeedsFromCommandLine, seedCoverage, maxReadSize, maxHits, transcriptome->getSeedLength());
    }
    size_t t_secondaryAlignmentBufferSize = sizeof(*t_secondaryAlignments) * t_secondaryAlignmentBufferCount;

    BigAllocator *t_allocator = new BigAllocator(BaseAligner::getBigAllocatorReservation(true, singleAlignerMaxHits, maxReadSize, transcriptome->getSeedLength(), numSeedsFromCommandLine, seedCoverage) + t_secondaryAlignmentBufferSize);

    BaseAligner *t_aligner = new (t_allocator) BaseAligner(
            transcriptome,
            singleAlignerMaxHits,
            maxDist,
            maxReadSize,
            numSeedsFromCommandLine,
            seedCoverage,
            extraSearchDepth,
            noUkkonen,
            noOrderedEvaluation,
            NULL,               // LV (no need to cache in the single aligner)
            NULL,               // reverse LV
            stats,
            t_allocator);

    if (maxSecondaryAligmmentAdditionalEditDistance >= 0) {
        t_secondaryAlignments = (SingleAlignmentResult *)t_allocator->allocate(t_secondaryAlignmentBufferSize);
    }

    t_allocator->checkCanaries();

    t_aligner->setExplorePopularSeeds(options->explorePopularSeeds);
    t_aligner->setStopOnFirstHit(options->stopOnFirstHit);

    //p_aligner for partial alignments

    unsigned p_numSeedsFromCommandLine = 0;
    float p_seedCoverage = (float) maxReadSize / (index->getSeedLength()*2);
    SingleAlignmentResult *p_secondaryAlignments = NULL;    
    unsigned p_secondaryAlignmentBufferCount;
    if (maxSecondaryAligmmentAdditionalEditDistance < 0) {
        p_secondaryAlignmentBufferCount = 0;
    } else {
        p_secondaryAlignmentBufferCount = BaseAligner::getMaxSecondaryResults(p_numSeedsFromCommandLine, p_seedCoverage, maxReadSize, maxHits, index->getSeedLength());
    }
    size_t p_secondaryAlignmentBufferSize = sizeof(*p_secondaryAlignments) * p_secondaryAlignmentBufferCount;

    BigAllocator *p_allocator = new BigAllocator(BaseAligner::getBigAllocatorReservation(true, singleAlignerMaxH
its, maxReadSize, index->getSeedLength(), p_numSeedsFromCommandLine, p_seedCoverage) + p_secondaryAlignmentB
ufferSize);

    BaseAligner *p_aligner = new (p_allocator) BaseAligner(
            index,
            singleAlignerMaxHits,
            maxDist,            
            maxReadSize,
            p_numSeedsFromCommandLine,
            p_seedCoverage,
            extraSearchDepth,
            noUkkonen,
            noOrderedEvaluation,
            NULL,               // LV (no need to cache in the single aligner)
            NULL,               // reverse LV
            stats,
            p_allocator);

    if (maxSecondaryAligmmentAdditionalEditDistance >= 0) {
        p_secondaryAlignments = (SingleAlignmentResult *)p_allocator->allocate(p_secondaryAlignmentBufferSize);
    }       
            
    p_allocator->checkCanaries();
            
    p_aligner->setExplorePopularSeeds(options->explorePopularSeeds);
    p_aligner->setStopOnFirstHit(options->stopOnFirstHit);

    //END P_ALIGNER

    ReadWriter *readWriter = this->readWriter;

#ifdef  _MSC_VER
    if (options->useTimingBarrier) {
        if (0 == InterlockedDecrementAndReturnNewValue(nThreadsAllocatingMemory)) {
            AllowEventWaitersToProceed(memoryAllocationCompleteBarrier);
        } else {
            WaitForEvent(memoryAllocationCompleteBarrier);
        }
    }
#endif  // _MSC_VER

    // Align the reads.
    Read *read0;
    Read *read1;

    _uint64 lastReportTime = timeInMillis();
    _uint64 readsWhenLastReported = 0;

    while (supplier->getNextReadPair(&read0,&read1)) {
        // Check that the two IDs form a pair; they will usually be foo/1 and foo/2 for some foo.
        if (!ignoreMismatchedIDs) {
			Read::checkIdMatch(read0, read1);
		}

        stats->totalReads += 2;

        // Skip the pair if there are too many Ns or 2s.
        int maxDist = this->maxDist;
        bool useful0 = read0->getDataLength() >= 50 && (int)read0->countOfNs() <= maxDist;
        bool useful1 = read1->getDataLength() >= 50 && (int)read1->countOfNs() <= maxDist;

        //Quality filtering
        bool quality0 = read0->qualityFilter(options->minPercentAbovePhred, options->minPhred, options->phredOffset);
        bool quality1 = read1->qualityFilter(options->minPercentAbovePhred, options->minPhred, options->phredOffset);
        
        if ((!useful0 && !useful1) || (!quality0 || !quality0)) {
            PairedAlignmentResult result;
            result.isTranscriptome[0] = false;
            result.isTranscriptome[1] = false;
            result.status[0] = NotFound;
            result.status[1] = NotFound;
            result.location[0] = InvalidGenomeLocation;
            result.location[1] = InvalidGenomeLocation;
            writePair(read0, read1, &result, false);
            continue;
        } else {
            // Here one the reads might still be hopeless, but maybe we can align the other.
            stats->usefulReads += (useful0 && useful1) ? 2 : 1;
        }

/*
<<<<<<< HEAD
        PairedAlignmentResult result, contaminantResult;
        result.isTranscriptome[0] = false;
        result.isTranscriptome[1] = false;

        //Make users setting
        AlignmentFilter filter(read0, read1, index->getGenome(), transcriptome->getGenome(), gtf, minSpacing, maxSpacing, options->confDiff, options->maxDist.start, index->getSeedLength(), p_aligner);

        const unsigned maxHitsToGet = 1000;
        unsigned loc0, loc1;
        Direction rc0, rc1;
        int score0, score1;
        int mapq0, mapq1;
        AlignmentResult status0;
        
        int       transcriptome_multiHitsFound0;
        unsigned  transcriptome_multiHitLocations0[maxHitsToGet];
        bool      transcriptome_multiHitRCs0[maxHitsToGet];
        int       transcriptome_multiHitScores0[maxHitsToGet]; 
        int       transcriptome_multiHitsFound1;
        unsigned  transcriptome_multiHitLocations1[maxHitsToGet];
        bool      transcriptome_multiHitRCs1[maxHitsToGet];
        int       transcriptome_multiHitScores1[maxHitsToGet]; 
        
        SingleAlignmentResult *secondaryAlignments = NULL;

        t_aligner->setReadId(0);      
        status0 = t_aligner->AlignRead(read0, &loc0, &rc0, &score0, &mapq0, 0, 0, 0, maxHitsToGet, &transcriptome_multiHitsFound0, (unsigned*)&transcriptome_multiHitLocations0, (bool*)&transcriptome_multiHitRCs0, (int*)&transcriptome_multiHitScores0);            
       
        t_aligner->setReadId(1);      
        status0 = t_aligner->AlignRead(read1, &loc1, &rc1, &score1, &mapq1, 0, 0, 0, maxHitsToGet, &transcriptome_multiHitsFound1, (unsigned*)&transcriptome_multiHitLocations1, (bool*)&transcriptome_multiHitRCs1, (int*)&transcriptome_multiHitScores1);            
 
        //Add reads to filter
        for (int i = 0; i < transcriptome_multiHitsFound0; ++i) {
            filter.AddAlignment(transcriptome_multiHitLocations0[i], transcriptome_multiHitRCs0[i], transcriptome_multiHitScores0[i], 0, true, false);
        }

        for (int i = 0; i < transcriptome_multiHitsFound1; ++i) {
            filter.AddAlignment(transcriptome_multiHitLocations1[i], transcriptome_multiHitRCs1[i], transcriptome_multiHitScores1[i], 0, true, true);
        }    
    
        g_aligner->align(read0, read1, &result);
        filter.AddAlignment(result.location[0], result.direction[0], result.score[0], result.mapq[0], false, false);
        filter.AddAlignment(result.location[1], result.direction[1], result.score[1], result.mapq[1], false, true);
     
        //Perform the primary filtering of all aligned reads
        unsigned status = filter.Filter(&result);

        //If the read is still unaligned
        if ((result.status[0] == NotFound) && (result.status[1] == NotFound)) {
        
          //If the contamination database is present
          if (c_aligner != NULL) {

            c_aligner->align(read0, read1, &contaminantResult);
            if ((contaminantResult.status[0] != NotFound) && (contaminantResult.status[1] != NotFound)) {

              c_filter->AddAlignment(contaminantResult.location[0], string(read0->getId(), read0->getIdLength()), string(read0->getData(), read0->getDataLength()));
              c_filter->AddAlignment(contaminantResult.location[1], string(read1->getId(), read1->getIdLength()), string(read1->getData(), read1->getDataLength()));

            }
          }
        }
=======
*/

        if (AlignerOptions::useHadoopErrorMessages && stats->totalReads % 10000 == 0 && timeInMillis() - lastReportTime > 10000) {
            fprintf(stderr,"reporter:counter:SNAP,readsAligned,%lu\n",stats->totalReads - readsWhenLastReported);
            readsWhenLastReported = stats->totalReads;
            lastReportTime = timeInMillis();
        }


        PairedAlignmentResult pairedResult, contaminantResult;
        pairedResult.isTranscriptome[0] = false;
        pairedResult.isTranscriptome[1] = false;
        pairedResult.tlocation[0] = 0;
        pairedResult.tlocation[1] = 0;
        contaminantResult.isTranscriptome[0] = false;
        contaminantResult.isTranscriptome[1] = false;
        contaminantResult.tlocation[0] = 0;
        contaminantResult.tlocation[1] = 0;

#if     TIME_HISTOGRAM
        _int64 startTime = timeInNanos();
#endif // TIME_HISTOGRAM

        //Add transcriptome alignments
        SingleAlignmentResult singleResult;
        singleResult0.isTranscriptome = false;
        singleResult1.isTranscriptome = false;
        int nSecondaryResults = 0;

        t_aligner->AlignRead(read0, &singleResult, maxSecondaryAligmmentAdditionalEditDistance, secondaryAlignmentBufferCount, &nSecondaryResults, t_secondaryAlignments);

        t_allocator->checkCanaries();

        filter.AddAlignment(singleResult.location, singleResult.direction, singleResult.score, singleResult.mapq, true, false);
        for (int i = 0; i < nSecondaryResults; i++) {
          filter.AddAlignment(t_secondaryAlignments[i].location, t_secondaryAlignments[i].direction, t_secondaryAlignments[i].score, t_secondaryAlignments[i].mapq, true, false);
        }

        t_aligner->AlignRead(read1, &singleResult, maxSecondaryAligmmentAdditionalEditDistance, secondaryAlignmentBufferCount, &nSecondaryResults, t_secondaryAlignments);

        t_allocator->checkCanaries();

        filter.AddAlignment(singleResult.location, singleResult.direction, singleResult.score, singleResult.mapq, true, true);
        for (int i = 0; i < nSecondaryResults; i++) {
          filter.AddAlignment(t_secondaryAlignments[i].location, t_secondaryAlignments[i].direction, t_secondaryAlignments[i].score, t_secondaryAlignments[i].mapq, true, true);
        }

        //Add genomic reads
        int nSecondaryResults;
        int nSingleSecondaryResults[2];

        g_aligner->align(read0, read1, &pairedResult, maxSecondaryAligmmentAdditionalEditDistance, maxPairedSecondaryHits, &nSecondaryResults, g_secondaryResults, maxSingleSecondaryHits, &nSingleSecondaryResults[0], &nSingleSecondaryResults[1],g_singleSecondaryResults);

        //Add primary result
        filter.AddAlignment(pairedResult.location[0], pairedResult.direction[0], pairedResult.score[0], pairedResult.mapq[0], false, false);
        filter.AddAlignment(pairedResult.location[1], pairedResult.direction[1], pairedResult.score[1], pairedResult.mapq[1], false, true);

        //Add all secondary results
        for (int i = 0; i < nSecondaryResults; i++) {
          filter.AddAlignment(g_secondaryResults[i].location[0], g_secondaryResults[i].direction[0], g_secondaryResults[i].score[0], g_secondaryResults[i].mapq[0], false, false);         
          filter.AddAlignment(g_secondaryResults[i].location[1], g_secondaryResults[i].direction[1], g_secondaryResults[i].score[1], g_secondaryResults[i].mapq[1], false, true);
        }
      
        for (int i = 0; i < nSingleSecondaryResults[0] + nSingleSecondaryResults[1]; i++) {
            bool isMate0 = i < nSingleSecondaryResults[0] ? true : false;
            if (readWriter != NULL && (options->passFilter(read, g_singleSecondaryResults[i].status))) {
                filter.AddAlignment(g_singleSecondaryResults[i].location, g_singleSecondaryResults[i].direction, g_singleSecondaryResults[i].score, g_singleSecondaryResults[i].mapq, false, isMate0); 
            }
        }

        //Perform the filtering
        unsigned status = filter.Filter(&result);

#if     TIME_HISTOGRAM
        _int64 runTime = timeInNanos() - startTime;
        int timeBucket = min(30, cheezyLogBase2(runTime));
        stats->countByTimeBucket[timeBucket]++;
        stats->nanosByTimeBucket[timeBucket] += runTime;
#endif // TIME_HISTOGRAM

        if (forceSpacing && isOneLocation(result.status[0]) != isOneLocation(result.status[1])) {
            // either both align or neither do
            result.status[0] = result.status[1] = NotFound;
            result.location[0] = result.location[1] = InvalidGenomeLocation;
        }

        writePair(read0, read1, &result, false);
        
         //No secondary alignments
        /*
        for (int i = 0; i < nSecondaryResults; i++) {
            writePair(read0, read1, secondaryResults + i, true);
        }

        for (int i = 0; i < nSingleSecondaryResults[0] + nSingleSecondaryResults[1]; i++) {
            Read *read = i < nSingleSecondaryResults[0] ? read0 : read1;
            if (readWriter != NULL && (options->passFilter(read, singleSecondaryResults[i].status))) {
                readWriter->writeRead(read, singleSecondaryResults[i].status, singleSecondaryResults[i].mapq, singleSecondaryResults[i].location, singleSecondaryResults[i].direction, true);
            }
        }
        */

        updateStats((PairedAlignerStats*) stats, read0, read1, &result);

    }

    stats->lvCalls = g_aligner->getLocationsScored();

    g_allocator->checkCanaries();

    g_aligner->~ChimericPairedEndAligner();
    delete supplier;

    g_intersectingAligner->~IntersectingPairedEndAligner();
    delete g_allocator;

    t_aligner->~BaseAligner();
    delete t_allocator;

    p_aligner->~BaseAligner();
    delete p_allocator;

    if (c_allocator != NULL) {
        c_allocator->checkCanaries();
        c_aligner->~ChimericPairedEndAligner();
        c_intersectingAligner->~IntersectingPairedEndAligner();
        delete c_allocator;
    }

}

void PairedAlignerContext::writePair(Read* read0, Read* read1, PairedAlignmentResult* result, bool secondary)
{
    bool pass0 = options->passFilter(read0, result->status[0]);
    bool pass1 = options->passFilter(read1, result->status[1]);
    bool pass = (options->filterFlags & AlignerOptions::FilterBothMatesMatch)
        ? (pass0 && pass1) : (pass0 || pass1);
    if (readWriter != NULL && pass) {
        readWriter->writePair(read0, read1, result, secondary);
    }
}

void PairedAlignerContext::updateStats(PairedAlignerStats* stats, Read* read0, Read* read1, PairedAlignmentResult* result)
{
    // Update stats
    for (int r = 0; r < 2; r++) {
        bool wasError = false;
        if (computeError && result->status[r] != NotFound) {
            wasError = wgsimReadMisaligned((r == 0 ? read0 : read1), result->location[r], index, options->misalignThreshold);
        }
        if (isOneLocation(result->status[r])) {
            stats->singleHits++;
            stats->errors += wasError ? 1 : 0;
        } else if (result->status[r] == MultipleHits) {
            stats->multiHits++;
        } else {
            _ASSERT(result->status[r] == NotFound);
            stats->notFound++;
        }
        // Add in MAPQ stats
        if (result->status[r] != NotFound) {
            int mapq = result->mapq[r];
            _ASSERT(mapq >= 0 && mapq <= AlignerStats::maxMapq);
            stats->mapqHistogram[mapq]++;
            stats->mapqErrors[mapq] += wasError ? 1 : 0;
        }
    }

    if (result->direction[0] == result->direction[1]) {
        stats->sameComplement++;
    }

    if (isOneLocation(result->status[0]) && isOneLocation(result->status[1])) {
        stats->incrementDistance(abs((int) (result->location[0] - result->location[1])));
        stats->incrementScore(result->score[0], result->score[1]);
    }

    if (result->fromAlignTogether) {
        stats->recordAlignTogetherMapqAndTime(__max(result->mapq[0], result->mapq[1]), result->nanosInAlignTogether, result->nSmallHits, result->nLVCalls);
    }

    if (result->alignedAsPair) {
        stats->alignedAsPairs += 2; // They are a pair, after all.  Hence, +2.
    }
}

    void 
PairedAlignerContext::typeSpecificBeginIteration()
{
    if (1 == options->nInputs) {
        //
        // We've only got one input, so just connect it directly to the consumer.
        //
        pairedReadSupplierGenerator = options->inputs[0].createPairedReadSupplierGenerator(options->numThreads, quicklyDropUnpairedReads, readerContext);
    } else {
        //
        // We've got multiple inputs, so use a MultiInputReadSupplier to combine the individual inputs.
        //
        PairedReadSupplierGenerator **generators = new PairedReadSupplierGenerator *[options->nInputs];
        // use separate context for each supplier, initialized from common
        for (int i = 0; i < options->nInputs; i++) {
            ReaderContext context(readerContext);
            generators[i] = options->inputs[i].createPairedReadSupplierGenerator(options->numThreads, quicklyDropUnpairedReads, context);
        }
        pairedReadSupplierGenerator = new MultiInputPairedReadSupplierGenerator(options->nInputs,generators);
    }
    ReaderContext* context = pairedReadSupplierGenerator->getContext();
    readerContext.header = context->header;
    readerContext.headerBytes = context->headerBytes;
    readerContext.headerLength = context->headerLength;
    readerContext.headerMatchesIndex = context->headerMatchesIndex;
}
    void 
PairedAlignerContext::typeSpecificNextIteration()
{
    if (readerContext.header != NULL) {
        delete [] readerContext.header;
        readerContext.header = NULL;
        readerContext.headerLength = readerContext.headerBytes = 0;
        readerContext.headerMatchesIndex = false;
    }
    delete pairedReadSupplierGenerator;
    pairedReadSupplierGenerator = NULL;
}
