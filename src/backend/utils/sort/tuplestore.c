/*-------------------------------------------------------------------------
 *
 * tuplestore.c
 *	  Generalized routines for temporary tuple storage.
 *
 * This module handles temporary storage of tuples for purposes such
 * as Materialize nodes, hashjoin batch files, etc.  It is essentially
 * a dumbed-down version of tuplesort.c; it does no sorting of tuples
 * but can only store and regurgitate a sequence of tuples.  However,
 * because no sort is required, it is allowed to start reading the sequence
 * before it has all been written.	This is particularly useful for cursors,
 * because it allows random access within the already-scanned portion of
 * a query without having to process the underlying scan to completion.
 * Also, it is possible to support multiple independent read pointers.
 *
 * A temporary file is used to handle the data if it exceeds the
 * space limit specified by the caller.
 *
 * The (approximate) amount of memory allowed to the tuplestore is specified
 * in kilobytes by the caller.	We absorb tuples and simply store them in an
 * in-memory array as long as we haven't exceeded maxKBytes.  If we do exceed
 * maxKBytes, we dump all the tuples into a temp file and then read from that
 * when needed.
 *
 * Upon creation, a tuplestore supports a single read pointer, numbered 0.
 * Additional read pointers can be created using tuplestore_alloc_read_pointer.
 * Mark/restore behavior is supported by copying read pointers.
 *
 * When the caller requests backward-scan capability, we write the temp file
 * in a format that allows either forward or backward scan.  Otherwise, only
 * forward scan is allowed.  A request for backward scan must be made before
 * putting any tuples into the tuplestore.	Rewind is normally allowed but
 * can be turned off via tuplestore_set_eflags; turning off rewind for all
 * read pointers enables truncation of the tuplestore at the oldest read point
 * for minimal memory usage.  (The caller must explicitly call tuplestore_trim
 * at appropriate times for truncation to actually happen.)
 *
 * Note: in TSS_WRITEFILE state, the temp file's seek position is the
 * current write position, and the write-position variables in the tuplestore
 * aren't kept up to date.  Similarly, in TSS_READFILE state the temp file's
 * seek position is the active read pointer's position, and that read pointer
 * isn't kept up to date.  We update the appropriate variables using ftell()
 * before switching to the other state or activating a different read pointer.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplestore.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/tablespace.h"
#include "executor/executor.h"
#include "storage/buffile.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"

//add by cywang
#include "executor/instrument.h"
#include "windowapi.h"

/*
 * Possible states of a Tuplestore object.	These denote the states that
 * persist between calls of Tuplestore routines.
 */
typedef enum
{
	TSS_INMEM,					/* Tuples still fit in memory */
	TSS_WRITEFILE,				/* Writing to temp file */
	TSS_READFILE				/* Reading from temp file */
} TupStoreStatus;

/*
 * State for a single read pointer.  If we are in state INMEM then all the
 * read pointers' "current" fields denote the read positions.  In state
 * WRITEFILE, the file/offset fields denote the read positions.  In state
 * READFILE, inactive read pointers have valid file/offset, but the active
 * read pointer implicitly has position equal to the temp file's seek position.
 *
 * Special case: if eof_reached is true, then the pointer's read position is
 * implicitly equal to the write position, and current/file/offset aren't
 * maintained.	This way we need not update all the read pointers each time
 * we write.
 */
typedef struct
{
	int			eflags;			/* capability flags */
	bool		eof_reached;	/* read has reached EOF */
	int			current;		/* next array index to read */
	int			file;			/* temp file# */
	off_t		offset;			/* byte offset in file */
} TSReadPointer;

/*
 * Private state of a Tuplestore operation.
 */
struct Tuplestorestate
{
	TupStoreStatus status;		/* enumerated value as shown above */
	int			eflags;			/* capability flags (OR of pointers' flags) */
	bool		backward;		/* store extra length words in file? */
	bool		interXact;		/* keep open through transactions? */
	bool		truncated;		/* tuplestore_trim has removed tuples? */
	long		availMem;		/* remaining memory available, in bytes */
	BufFile    *myfile;			/* underlying file, or NULL if none */
	MemoryContext context;		/* memory context for holding tuples */
	ResourceOwner resowner;		/* resowner for holding temp files */

	/*
	 * These function pointers decouple the routines that must know what kind
	 * of tuple we are handling from the routines that don't need to know it.
	 * They are set up by the tuplestore_begin_xxx routines.
	 *
	 * (Although tuplestore.c currently only supports heap tuples, I've copied
	 * this part of tuplesort.c so that extension to other kinds of objects
	 * will be easy if it's ever needed.)
	 *
	 * Function to copy a supplied input tuple into palloc'd space. (NB: we
	 * assume that a single pfree() is enough to release the tuple later, so
	 * the representation must be "flat" in one palloc chunk.) state->availMem
	 * must be decreased by the amount of space used.
	 */
	void	   *(*copytup) (Tuplestorestate *state, void *tup);

	/*
	 * Function to write a stored tuple onto tape.	The representation of the
	 * tuple on tape need not be the same as it is in memory; requirements on
	 * the tape representation are given below.  After writing the tuple,
	 * pfree() it, and increase state->availMem by the amount of memory space
	 * thereby released.
	 */
	void		(*writetup) (Tuplestorestate *state, void *tup);

	/*
	 * Function to read a stored tuple from tape back into memory. 'len' is
	 * the already-read length of the stored tuple.  Create and return a
	 * palloc'd copy, and decrease state->availMem by the amount of memory
	 * space consumed.
	 */
	void	   *(*readtup) (Tuplestorestate *state, unsigned int len);

	/*
	 * This array holds pointers to tuples in memory if we are in state INMEM.
	 * In states WRITEFILE and READFILE it's not used.
	 *
	 * When memtupdeleted > 0, the first memtupdeleted pointers are already
	 * released due to a tuplestore_trim() operation, but we haven't expended
	 * the effort to slide the remaining pointers down.  These unused pointers
	 * are set to NULL to catch any invalid accesses.  Note that memtupcount
	 * includes the deleted pointers.
	 */
	void	  **memtuples;		/* array of pointers to palloc'd tuples */
	int			memtupdeleted;	/* the first N slots are currently unused */
	int			memtupcount;	/* number of tuples currently present */
	int			memtupsize;		/* allocated length of memtuples array */

	/*
	 * These variables are used to keep track of the current positions.
	 *
	 * In state WRITEFILE, the current file seek position is the write point;
	 * in state READFILE, the write position is remembered in writepos_xxx.
	 * (The write position is the same as EOF, but since BufFileSeek doesn't
	 * currently implement SEEK_END, we have to remember it explicitly.)
	 */
	TSReadPointer *readptrs;	/* array of read pointers */
	int			activeptr;		/* index of the active read pointer */
	int			readptrcount;	/* number of pointers currently valid */
	int			readptrsize;	/* allocated length of readptrs array */

	int			writepos_file;	/* file# (valid if READFILE state) */
	off_t		writepos_offset;	/* offset (valid if READFILE state) */

	/* add by cywang */
	Instrumentation	*instr_buffer_read;		/* the time cost of buffer read */
	Instrumentation	*instr_buffer_write;	/* the time cost of buffer write */
	Instrumentation	*instr_disk_read;		/* the time cost of disk read */
	Instrumentation	*instr_disk_write;		/* the time cost of disk write */
	Instrumentation	*instr_locateheadpos_io;
	bool			in_locateheadpos;		/* flags whether we are locate the head position */

//#ifdef WIN_FUN_OPT
	/* following is for useful tuple, the backward is identical with the original one */
	//bool		ever_spool_partition;	/* has ever spool a partition to disk, if true, no need to initialize the useful attributes */
	BufFile		*opt_file;	/* unlike myfile, usefulFile only stores attributes for the computing of window functions */
	AttrNumber	*useful2init;	/* the i'th attribute of the useful tuple is the useful2init[i]'th attribute of the origin tuple */
	//ProjectionInfo	*projInfo;	/* the ProjectionInfo to project a initial tuple to a useful tuple */
	TupleDesc	opt_tupdesc;	/* for heap_form_minimal_tuple(desc, values, isnulls) */
	TupleTableSlot	*init_slot;	/* used to extract attributes from a initial MinimalTuple*/
	int			opt_maxPos;
	TSReadPointer	*opt_readptrs;		/* read pointers for opt_file */
	int			opt_activeptr;
	int			opt_readptrcount;
	int			opt_readptrsize;
	int			opt_writepos_file;
	off_t		opt_writepos_offset;

	/* following is for initial tuple, only needed for current row, so only need forward read */
	BufFile		*init_file;
	TSReadPointer	init_readptr;
	int			init_writepos_file;
	off_t		init_writepos_offset;

	/* for reusing buffer */
	/*
	 * When in disk, use a new strategy to truncate the array.
	 * The array no longer start at index 0, the startIndex can be any value in the scope [0, memtupsize].
	 * If aggregateupto in the scope [startPos, startPos+memtupsize),
	 * access directly buffer[(startIndex+(aggregateupto-startPos))%memtupsize].
	 */
	int			startIndex;
	int64		startPos;		/* position of the tuple at startIndex */
	//int			reuse_count;	/* if memtupcount<=7/8*memtupsize, set to memtupcount; or set to 7/8*memtupsize */
	//int			reuse_size;		/* 7/8*memtupsize */
	int			reuse_ptr;
//#endif
};

#define COPYTUP(state,tup)	((*(state)->copytup) (state, tup))
#define WRITETUP(state,tup) ((*(state)->writetup) (state, tup))
#define READTUP(state,len)	((*(state)->readtup) (state, len))
#define LACKMEM(state)		((state)->availMem < 0)
#define USEMEM(state,amt)	((state)->availMem -= (amt))
#define FREEMEM(state,amt)	((state)->availMem += (amt))

/*--------------------
 *
 * NOTES about on-tape representation of tuples:
 *
 * We require the first "unsigned int" of a stored tuple to be the total size
 * on-tape of the tuple, including itself (so it is never zero).
 * The remainder of the stored tuple
 * may or may not match the in-memory representation of the tuple ---
 * any conversion needed is the job of the writetup and readtup routines.
 *
 * If state->backward is true, then the stored representation of
 * the tuple must be followed by another "unsigned int" that is a copy of the
 * length --- so the total tape space used is actually sizeof(unsigned int)
 * more than the stored length value.  This allows read-backwards.	When
 * state->backward is not set, the write/read routines may omit the extra
 * length word.
 *
 * writetup is expected to write both length words as well as the tuple
 * data.  When readtup is called, the tape is positioned just after the
 * front length word; readtup must read the tuple data and advance past
 * the back length word (if present).
 *
 * The write/read routines can make use of the tuple description data
 * stored in the Tuplestorestate record, if needed. They are also expected
 * to adjust state->availMem by the amount of memory space (not tape space!)
 * released or consumed.  There is no error return from either writetup
 * or readtup; they should ereport() on failure.
 *
 *
 * NOTES about memory consumption calculations:
 *
 * We count space allocated for tuples against the maxKBytes limit,
 * plus the space used by the variable-size array memtuples.
 * Fixed-size space (primarily the BufFile I/O buffer) is not counted.
 * We don't worry about the size of the read pointer array, either.
 *
 * Note that we count actual space used (as shown by GetMemoryChunkSpace)
 * rather than the originally-requested size.  This is important since
 * palloc can add substantial overhead.  It's not a complete answer since
 * we won't count any wasted space in palloc allocation blocks, but it's
 * a lot better than what we were doing before 7.3.
 *
 *--------------------
 */


static Tuplestorestate *tuplestore_begin_common(int eflags,
						bool interXact,
						int maxKBytes);
static void tuplestore_puttuple_common(Tuplestorestate *state, void *tuple);
static void dumptuples(Tuplestorestate *state);
static unsigned int getlen(Tuplestorestate *state, bool eofOK);
static void *copytup_heap(Tuplestorestate *state, void *tup);
static void writetup_heap(Tuplestorestate *state, void *tup);
static void *readtup_heap(Tuplestorestate *state, unsigned int len);

//#ifdef WIN_FUN_OPT
static void opt_tuplestore_puttuple_common(Tuplestorestate *state, void *tuple);
static unsigned int opt_getlen(Tuplestorestate *state, bool eofOK);
static void opt_dumptuples(Tuplestorestate *state);
static void opt_writetup_heap(Tuplestorestate *state, void *tup);
static void *opt_readtup_heap(Tuplestorestate *state, unsigned int len);

static void init_writetup_heap(Tuplestorestate *state, void *tup);
static void init_dumptuples(Tuplestorestate *state);
static void *opt_tuplestore_gettuple(Tuplestorestate *state, bool forward, bool *should_free);
static void *init_tuplestore_gettuple(Tuplestorestate *state, bool forward, bool *should_free);
static unsigned int init_getlen(Tuplestorestate *state, bool eofOK);
static void *init_readtup_heap(Tuplestorestate *state, unsigned int len);
static void reuse_dumptubles(Tuplestorestate *state);
static void reuse_writetup_heap(Tuplestorestate *state, void *tup);
//#endif

/*
 *		tuplestore_begin_xxx
 *
 * Initialize for a tuple store operation.
 */
static Tuplestorestate *
tuplestore_begin_common(int eflags, bool interXact, int maxKBytes)
{
	Tuplestorestate *state;

	state = (Tuplestorestate *) palloc0(sizeof(Tuplestorestate));

	state->status = TSS_INMEM;
	state->eflags = eflags;
	state->interXact = interXact;
	state->truncated = false;
	state->availMem = maxKBytes * 1024L;
	state->myfile = NULL;
	state->context = CurrentMemoryContext;
	state->resowner = CurrentResourceOwner;

	state->memtupdeleted = 0;
	state->memtupcount = 0;
	state->memtupsize = 1024;	/* initial guess */
	state->memtuples = (void **) palloc(state->memtupsize * sizeof(void *));

	USEMEM(state, GetMemoryChunkSpace(state->memtuples));

	state->activeptr = 0;
	state->readptrcount = 1;
	state->readptrsize = 8;		/* arbitrary */
	state->readptrs = (TSReadPointer *)
		palloc(state->readptrsize * sizeof(TSReadPointer));

	state->readptrs[0].eflags = eflags;
	state->readptrs[0].eof_reached = false;
	state->readptrs[0].current = 0;

	return state;
}

/*
 * tuplestore_begin_heap
 *
 * Create a new tuplestore; other types of tuple stores (other than
 * "heap" tuple stores, for heap tuples) are possible, but not presently
 * implemented.
 *
 * randomAccess: if true, both forward and backward accesses to the
 * tuple store are allowed.
 *
 * interXact: if true, the files used for on-disk storage persist beyond the
 * end of the current transaction.	NOTE: It's the caller's responsibility to
 * create such a tuplestore in a memory context and resource owner that will
 * also survive transaction boundaries, and to ensure the tuplestore is closed
 * when it's no longer wanted.
 *
 * maxKBytes: how much data to store in memory (any data beyond this
 * amount is paged to disk).  When in doubt, use work_mem.
 */
Tuplestorestate *
tuplestore_begin_heap(bool randomAccess, bool interXact, int maxKBytes)
{
	Tuplestorestate *state;
	int			eflags;

	/*
	 * This interpretation of the meaning of randomAccess is compatible with
	 * the pre-8.3 behavior of tuplestores.
	 */
	eflags = randomAccess ?
		(EXEC_FLAG_BACKWARD | EXEC_FLAG_REWIND) :
		(EXEC_FLAG_REWIND);

	state = tuplestore_begin_common(eflags, interXact, maxKBytes);

	state->copytup = copytup_heap;
	state->writetup = writetup_heap;
	state->readtup = readtup_heap;

	/* by cywang, to record the time cost */
	state->instr_buffer_read = InstrAlloc(1,1);
	state->instr_buffer_write = InstrAlloc(1,1);
	state->instr_disk_read = InstrAlloc(1,1);
	state->instr_disk_write = InstrAlloc(1,1);
	state->instr_locateheadpos_io = InstrAlloc(1,1);
	state->in_locateheadpos = false;
	/* for reusing buffer */
	state->startPos = 0;

	return state;
}

/*
 * tuplestore_set_eflags
 *
 * Set the capability flags for read pointer 0 at a finer grain than is
 * allowed by tuplestore_begin_xxx.  This must be called before inserting
 * any data into the tuplestore.
 *
 * eflags is a bitmask following the meanings used for executor node
 * startup flags (see executor.h).	tuplestore pays attention to these bits:
 *		EXEC_FLAG_REWIND		need rewind to start
 *		EXEC_FLAG_BACKWARD		need backward fetch
 * If tuplestore_set_eflags is not called, REWIND is allowed, and BACKWARD
 * is set per "randomAccess" in the tuplestore_begin_xxx call.
 *
 * NOTE: setting BACKWARD without REWIND means the pointer can read backwards,
 * but not further than the truncation point (the furthest-back read pointer
 * position at the time of the last tuplestore_trim call).
 */
void
tuplestore_set_eflags(Tuplestorestate *state, int eflags)
{
	int			i;

	if (state->status != TSS_INMEM || state->memtupcount != 0)
		elog(ERROR, "too late to call tuplestore_set_eflags");

	state->readptrs[0].eflags = eflags;
	for (i = 1; i < state->readptrcount; i++)
		eflags |= state->readptrs[i].eflags;
	state->eflags = eflags;
}

/*
 * tuplestore_alloc_read_pointer - allocate another read pointer.
 *
 * Returns the pointer's index.
 *
 * The new pointer initially copies the position of read pointer 0.
 * It can have its own eflags, but if any data has been inserted into
 * the tuplestore, these eflags must not represent an increase in
 * requirements.
 */
int
tuplestore_alloc_read_pointer(Tuplestorestate *state, int eflags)
{
	/* Check for possible increase of requirements */
	if (state->status != TSS_INMEM || state->memtupcount != 0)
	{
		if ((state->eflags | eflags) != state->eflags)
			elog(ERROR, "too late to require new tuplestore eflags");
	}

	/* Make room for another read pointer if needed */
	if (state->readptrcount >= state->readptrsize)
	{
		int			newcnt = state->readptrsize * 2;

		state->readptrs = (TSReadPointer *)
			repalloc(state->readptrs, newcnt * sizeof(TSReadPointer));
		state->readptrsize = newcnt;
	}

	/* And set it up */
	state->readptrs[state->readptrcount] = state->readptrs[0];
	state->readptrs[state->readptrcount].eflags = eflags;

	state->eflags |= eflags;

	return state->readptrcount++;
}

/*
 * tuplestore_clear
 *
 *	Delete all the contents of a tuplestore, and reset its read pointers
 *	to the start.
 */
void
tuplestore_clear(Tuplestorestate *state)
{
	int			i;
	TSReadPointer *readptr;
/*
//#ifdef WIN_FUN_OPT
	TSReadPointer *opt_readptr;
	if(enable_winfunopt){
		if(state->opt_file)
			BufFileClose(state->opt_file);
		state->opt_file = NULL;
		opt_readptr = state->opt_readptrs;
		if(opt_readptr){
			for (i = 0; i < state->opt_readptrcount; opt_readptr++, i++)
			{
				opt_readptr->eof_reached = false;
				opt_readptr->current = 0;
			}
		}
		if(state->init_file)
			BufFileClose(state->opt_file);
	}
//#endif
*/
	if (state->myfile)
		BufFileClose(state->myfile);
	state->myfile = NULL;
	if (state->memtuples)
	{
		for (i = state->memtupdeleted; i < state->memtupcount; i++)
		{
			FREEMEM(state, GetMemoryChunkSpace(state->memtuples[i]));
			pfree(state->memtuples[i]);
		}
	}
	state->status = TSS_INMEM;
	state->truncated = false;
	state->memtupdeleted = 0;
	state->memtupcount = 0;
	readptr = state->readptrs;
	for (i = 0; i < state->readptrcount; readptr++, i++)
	{
		readptr->eof_reached = false;
		readptr->current = 0;
	}
}

/*
 * tuplestore_end
 *
 *	Release resources and clean up.
 */
void
tuplestore_end(Tuplestorestate *state)
{
	int			i;

	if (state->myfile)
		BufFileClose(state->myfile);
	if (state->memtuples)
	{
		for (i = state->memtupdeleted; i < state->memtupcount; i++)
			pfree(state->memtuples[i]);
		pfree(state->memtuples);
	}
	pfree(state->readptrs);
	pfree(state);
}

/*
 * tuplestore_select_read_pointer - make the specified read pointer active
 */
void
tuplestore_select_read_pointer(Tuplestorestate *state, int ptr)
{
	TSReadPointer *readptr;
	TSReadPointer *oldptr;

	Assert(ptr >= 0 && ptr < state->readptrcount);

	/* No work if already active */
	if (ptr == state->activeptr)
		return;

	readptr = &state->readptrs[ptr];
	oldptr = &state->readptrs[state->activeptr];

	switch (state->status)
	{
		case TSS_INMEM:
		case TSS_WRITEFILE:
			/* no work */
			break;
		case TSS_READFILE:

			/*
			 * First, save the current read position in the pointer about to
			 * become inactive.
			 */
			if (!oldptr->eof_reached)
				BufFileTell(state->myfile,
							&oldptr->file,
							&oldptr->offset);

			/*
			 * We have to make the temp file's seek position equal to the
			 * logical position of the new read pointer.  In eof_reached
			 * state, that's the EOF, which we have available from the saved
			 * write position.
			 */
			if (readptr->eof_reached)
			{
				if (BufFileSeek(state->myfile,
								state->writepos_file,
								state->writepos_offset,
								SEEK_SET) != 0)
					elog(ERROR, "tuplestore seek failed");
			}
			else
			{
				if (BufFileSeek(state->myfile,
								readptr->file,
								readptr->offset,
								SEEK_SET) != 0)
					elog(ERROR, "tuplestore seek failed");
			}
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}

	state->activeptr = ptr;
}

/*
 * tuplestore_ateof
 *
 * Returns the active read pointer's eof_reached state.
 */
bool
tuplestore_ateof(Tuplestorestate *state)
{
	return state->readptrs[state->activeptr].eof_reached;
}

/*
 * Accept one tuple and append it to the tuplestore.
 *
 * Note that the input tuple is always copied; the caller need not save it.
 *
 * If the active read pointer is currently "at EOF", it remains so (the read
 * pointer implicitly advances along with the write pointer); otherwise the
 * read pointer is unchanged.  Non-active read pointers do not move, which
 * means they are certain to not be "at EOF" immediately after puttuple.
 * This curious-seeming behavior is for the convenience of nodeMaterial.c and
 * nodeCtescan.c, which would otherwise need to do extra pointer repositioning
 * steps.
 *
 * tuplestore_puttupleslot() is a convenience routine to collect data from
 * a TupleTableSlot without an extra copy operation.
 */
void
tuplestore_puttupleslot(Tuplestorestate *state,
						TupleTableSlot *slot)
{
	MinimalTuple tuple;
	MemoryContext oldcxt = MemoryContextSwitchTo(state->context);

	/*
	 * Form a MinimalTuple in working memory
	 */
	tuple = ExecCopySlotMinimalTuple(slot);
	USEMEM(state, GetMemoryChunkSpace(tuple));

	tuplestore_puttuple_common(state, (void *) tuple);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * "Standard" case to copy from a HeapTuple.  This is actually now somewhat
 * deprecated, but not worth getting rid of in view of the number of callers.
 */
void
tuplestore_puttuple(Tuplestorestate *state, HeapTuple tuple)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(state->context);

	/*
	 * Copy the tuple.  (Must do this even in WRITEFILE case.  Note that
	 * COPYTUP includes USEMEM, so we needn't do that here.)
	 */
	tuple = COPYTUP(state, tuple);

	tuplestore_puttuple_common(state, (void *) tuple);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Similar to tuplestore_puttuple(), but work from values + nulls arrays.
 * This avoids an extra tuple-construction operation.
 */
void
tuplestore_putvalues(Tuplestorestate *state, TupleDesc tdesc,
					 Datum *values, bool *isnull)
{
	MinimalTuple tuple;
	MemoryContext oldcxt = MemoryContextSwitchTo(state->context);

	tuple = heap_form_minimal_tuple(tdesc, values, isnull);
	USEMEM(state, GetMemoryChunkSpace(tuple));

	tuplestore_puttuple_common(state, (void *) tuple);

	MemoryContextSwitchTo(oldcxt);
}

static void
tuplestore_puttuple_common(Tuplestorestate *state, void *tuple)
{
	TSReadPointer *readptr;
	int			i;
	ResourceOwner oldowner;

	/* by cywang, to record the time of dumping the buffer from memory to tape */
	Instrumentation *instr=InstrAlloc(1,1);

	switch (state->status)
	{
		case TSS_INMEM:
			InstrStartNode(state->instr_buffer_write);	/* to record time of buffer write*/

			/*
			 * Update read pointers as needed; see API spec above.
			 */
			readptr = state->readptrs;
			for (i = 0; i < state->readptrcount; readptr++, i++)
			{
				if (readptr->eof_reached && i != state->activeptr)
				{
					readptr->eof_reached = false;
					readptr->current = state->memtupcount;
				}
			}

			/*
			 * Grow the array as needed.  Note that we try to grow the array
			 * when there is still one free slot remaining --- if we fail,
			 * there'll still be room to store the incoming tuple, and then
			 * we'll switch to tape-based operation.
			 */
			if (state->memtupcount >= state->memtupsize - 1)
			{
				/*
				 * See grow_memtuples() in tuplesort.c for the rationale
				 * behind these two tests.
				 */
				if (state->availMem > (long) (state->memtupsize * sizeof(void *)) &&
					(Size) (state->memtupsize * 2) < MaxAllocSize / sizeof(void *))
				{
					FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
					state->memtupsize *= 2;
					state->memtuples = (void **)
						repalloc(state->memtuples,
								 state->memtupsize * sizeof(void *));
					USEMEM(state, GetMemoryChunkSpace(state->memtuples));
				}
			}

			/* Stash the tuple in the in-memory array */
			state->memtuples[state->memtupcount++] = tuple;

			InstrStopNode(state->instr_buffer_write, 0);

			/*
			 * Done if we still fit in available memory and have array slots.
			 */
			if (state->memtupcount < state->memtupsize && !LACKMEM(state))
				return;

			/*
			 * Nope; time to switch to tape-based operation.  Make sure that
			 * the temp file(s) are created in suitable temp tablespaces.
			 */
			PrepareTempTablespaces();

			/* associate the file with the store's resource owner */
			oldowner = CurrentResourceOwner;
			CurrentResourceOwner = state->resowner;

			state->myfile = BufFileCreateTemp(state->interXact);

			CurrentResourceOwner = oldowner;

			/*
			 * Freeze the decision about whether trailing length words will be
			 * used.  We can't change this choice once data is on tape, even
			 * though callers might drop the requirement.
			 */
			state->backward = (state->eflags & EXEC_FLAG_BACKWARD) != 0;
			state->status = TSS_WRITEFILE;

			InstrStartNode(instr);


			if(enable_reusebuffer)
				reuse_dumptubles(state);
			else
				dumptuples(state);

			InstrStopNode(instr, 0);
			printf("dump memory: %f\n", INSTR_TIME_GET_DOUBLE(instr->counter));

			break;
		case TSS_WRITEFILE:

			/*
			 * Update read pointers as needed; see API spec above. Note:
			 * BufFileTell is quite cheap, so not worth trying to avoid
			 * multiple calls.
			 */
			readptr = state->readptrs;
			for (i = 0; i < state->readptrcount; readptr++, i++)
			{
				if (readptr->eof_reached && i != state->activeptr)
				{
					readptr->eof_reached = false;
					BufFileTell(state->myfile,
								&readptr->file,
								&readptr->offset);
				}
			}

			WRITETUP(state, tuple);
			break;
		case TSS_READFILE:

			/*
			 * Switch from reading to writing.
			 */
			if (!state->readptrs[state->activeptr].eof_reached)
				BufFileTell(state->myfile,
							&state->readptrs[state->activeptr].file,
							&state->readptrs[state->activeptr].offset);
			if (BufFileSeek(state->myfile,
							state->writepos_file, state->writepos_offset,
							SEEK_SET) != 0)
				elog(ERROR, "tuplestore seek to EOF failed");
			state->status = TSS_WRITEFILE;

			/*
			 * Update read pointers as needed; see API spec above.
			 */
			readptr = state->readptrs;
			for (i = 0; i < state->readptrcount; readptr++, i++)
			{
				if (readptr->eof_reached && i != state->activeptr)
				{
					readptr->eof_reached = false;
					readptr->file = state->writepos_file;
					readptr->offset = state->writepos_offset;
				}
			}

			WRITETUP(state, tuple);
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.	If should_free is set, the
 * caller must pfree the returned tuple when done with it.
 *
 * Backward scan is only allowed if randomAccess was set true or
 * EXEC_FLAG_BACKWARD was specified to tuplestore_set_eflags().
 */
static void *
tuplestore_gettuple(Tuplestorestate *state, bool forward,
					bool *should_free)
{
	TSReadPointer *readptr = &state->readptrs[state->activeptr];
	unsigned int tuplen;
	void	   *tup;

	Assert(forward || (readptr->eflags & EXEC_FLAG_BACKWARD));

	switch (state->status)
	{
		case TSS_INMEM:

			*should_free = false;
			if (forward)
			{
				if (readptr->eof_reached)
					return NULL;
				if (readptr->current < state->memtupcount)
				{
					/* We have another tuple, so return it */
					return state->memtuples[readptr->current++];
				}
				readptr->eof_reached = true;
				return NULL;
			}
			else
			{
				/*
				 * if all tuples are fetched already then we return last
				 * tuple, else tuple before last returned.
				 */
				if (readptr->eof_reached)
				{
					readptr->current = state->memtupcount;
					readptr->eof_reached = false;
				}
				else
				{
					if (readptr->current <= state->memtupdeleted)
					{
						Assert(!state->truncated);
						return NULL;
					}
					readptr->current--; /* last returned tuple */
				}
				if (readptr->current <= state->memtupdeleted)
				{
					Assert(!state->truncated);
					return NULL;
				}
				return state->memtuples[readptr->current - 1];
			}
			break;

		case TSS_WRITEFILE:

			/* Skip state change if we'll just return NULL */
			if (readptr->eof_reached && forward)
				return NULL;

			/*
			 * Switch from writing to reading.
			 */
			BufFileTell(state->myfile,
						&state->writepos_file, &state->writepos_offset);
			if (!readptr->eof_reached)
				if (BufFileSeek(state->myfile,
								readptr->file, readptr->offset,
								SEEK_SET) != 0)
					elog(ERROR, "tuplestore seek failed");

			state->status = TSS_READFILE;
			/* FALL THRU into READFILE case */

		case TSS_READFILE:
			*should_free = true;
			if (forward)
			{
				if ((tuplen = getlen(state, true)) != 0)
				{
					tup = READTUP(state, tuplen);
					return tup;
				}
				else
				{
					readptr->eof_reached = true;
					return NULL;
				}
			}

			/*
			 * Backward.
			 *
			 * if all tuples are fetched already then we return last tuple,
			 * else tuple before last returned.
			 *
			 * Back up to fetch previously-returned tuple's ending length
			 * word. If seek fails, assume we are at start of file.
			 */
			if (BufFileSeek(state->myfile, 0, -(long) sizeof(unsigned int),
							SEEK_CUR) != 0)
			{
				/* even a failed backwards fetch gets you out of eof state */
				readptr->eof_reached = false;
				Assert(!state->truncated);
				return NULL;
			}
			tuplen = getlen(state, false);

			if (readptr->eof_reached)
			{
				readptr->eof_reached = false;
				/* We will return the tuple returned before returning NULL */
			}
			else
			{
				/*
				 * Back up to get ending length word of tuple before it.
				 */
				if (BufFileSeek(state->myfile, 0,
								-(long) (tuplen + 2 * sizeof(unsigned int)),
								SEEK_CUR) != 0)
				{
					/*
					 * If that fails, presumably the prev tuple is the first
					 * in the file.  Back up so that it becomes next to read
					 * in forward direction (not obviously right, but that is
					 * what in-memory case does).
					 */
					if (BufFileSeek(state->myfile, 0,
									-(long) (tuplen + sizeof(unsigned int)),
									SEEK_CUR) != 0)
						elog(ERROR, "bogus tuple length in backward scan");
					Assert(!state->truncated);
					return NULL;
				}
				tuplen = getlen(state, false);
			}

			/*
			 * Now we have the length of the prior tuple, back up and read it.
			 * Note: READTUP expects we are positioned after the initial
			 * length word of the tuple, so back up to that point.
			 */
			if (BufFileSeek(state->myfile, 0,
							-(long) tuplen,
							SEEK_CUR) != 0)
				elog(ERROR, "bogus tuple length in backward scan");
			tup = READTUP(state, tuplen);
			return tup;

		default:
			elog(ERROR, "invalid tuplestore state");
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * tuplestore_gettupleslot - exported function to fetch a MinimalTuple
 *
 * If successful, put tuple in slot and return TRUE; else, clear the slot
 * and return FALSE.
 *
 * If copy is TRUE, the slot receives a copied tuple (allocated in current
 * memory context) that will stay valid regardless of future manipulations of
 * the tuplestore's state.  If copy is FALSE, the slot may just receive a
 * pointer to a tuple held within the tuplestore.  The latter is more
 * efficient but the slot contents may be corrupted if additional writes to
 * the tuplestore occur.  (If using tuplestore_trim, see comments therein.)
 */
bool
tuplestore_gettupleslot(Tuplestorestate *state, bool forward,
						bool copy, TupleTableSlot *slot)
{
	MinimalTuple tuple;
	bool		should_free;

	/* by cywang */
	if(tuplestore_in_memory(state)){
		InstrStartNode(state->instr_buffer_read);
	}else{
		InstrStartNode(state->instr_disk_read);
		if(state->in_locateheadpos)
			InstrStartNode(state->instr_locateheadpos_io);
	}

	tuple = (MinimalTuple) tuplestore_gettuple(state, forward, &should_free);

	if(tuplestore_in_memory(state)){
		InstrStopNode(state->instr_buffer_read, 0);
	}else{
		InstrStopNode(state->instr_disk_read, 0);
		if(state->in_locateheadpos)
			InstrStopNode(state->instr_locateheadpos_io, 0);
	}

	if (tuple)
	{
		if (copy && !should_free)
		{
			tuple = heap_copy_minimal_tuple(tuple);
			should_free = true;
		}
		ExecStoreMinimalTuple(tuple, slot, should_free);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

/*
 * tuplestore_advance - exported function to adjust position without fetching
 *
 * We could optimize this case to avoid palloc/pfree overhead, but for the
 * moment it doesn't seem worthwhile.  (XXX this probably needs to be
 * reconsidered given the needs of window functions.)
 */
bool
tuplestore_advance(Tuplestorestate *state, bool forward)
{
	void	   *tuple;
	bool		should_free;

	tuple = tuplestore_gettuple(state, forward, &should_free);

	if (tuple)
	{
		if (should_free)
			pfree(tuple);
		return true;
	}
	else
	{
		return false;
	}
}

/*
 * dumptuples - remove tuples from memory and write to tape
 *
 * As a side effect, we must convert each read pointer's position from
 * "current" to file/offset format.  But eof_reached pointers don't
 * need to change state.
 */
static void
dumptuples(Tuplestorestate *state)
{
	int			i;

	for (i = state->memtupdeleted;; i++)
	{
		TSReadPointer *readptr = state->readptrs;
		int			j;

		for (j = 0; j < state->readptrcount; readptr++, j++)
		{
			if (i == readptr->current && !readptr->eof_reached)
				BufFileTell(state->myfile,
							&readptr->file, &readptr->offset);
		}
		if (i >= state->memtupcount)
			break;
		WRITETUP(state, state->memtuples[i]);
	}
	state->memtupdeleted = 0;
	state->memtupcount = 0;
}

/*
 * tuplestore_rescan		- rewind the active read pointer to start
 */
void
tuplestore_rescan(Tuplestorestate *state)
{
	TSReadPointer *readptr = &state->readptrs[state->activeptr];

	Assert(readptr->eflags & EXEC_FLAG_REWIND);
	Assert(!state->truncated);

	switch (state->status)
	{
		case TSS_INMEM:
			readptr->eof_reached = false;
			readptr->current = 0;
			break;
		case TSS_WRITEFILE:
			readptr->eof_reached = false;
			readptr->file = 0;
			readptr->offset = 0L;
			break;
		case TSS_READFILE:
			readptr->eof_reached = false;
			if (BufFileSeek(state->myfile, 0, 0L, SEEK_SET) != 0)
				elog(ERROR, "tuplestore seek to start failed");
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * tuplestore_copy_read_pointer - copy a read pointer's state to another
 */
void
tuplestore_copy_read_pointer(Tuplestorestate *state,
							 int srcptr, int destptr)
{
	TSReadPointer *sptr = &state->readptrs[srcptr];
	TSReadPointer *dptr = &state->readptrs[destptr];

	Assert(srcptr >= 0 && srcptr < state->readptrcount);
	Assert(destptr >= 0 && destptr < state->readptrcount);

	/* Assigning to self is a no-op */
	if (srcptr == destptr)
		return;

	if (dptr->eflags != sptr->eflags)
	{
		/* Possible change of overall eflags, so copy and then recompute */
		int			eflags;
		int			i;

		*dptr = *sptr;
		eflags = state->readptrs[0].eflags;
		for (i = 1; i < state->readptrcount; i++)
			eflags |= state->readptrs[i].eflags;
		state->eflags = eflags;
	}
	else
		*dptr = *sptr;

	switch (state->status)
	{
		case TSS_INMEM:
		case TSS_WRITEFILE:
			/* no work */
			break;
		case TSS_READFILE:

			/*
			 * This case is a bit tricky since the active read pointer's
			 * position corresponds to the seek point, not what is in its
			 * variables.  Assigning to the active requires a seek, and
			 * assigning from the active requires a tell, except when
			 * eof_reached.
			 */
			if (destptr == state->activeptr)
			{
				if (dptr->eof_reached)
				{
					if (BufFileSeek(state->myfile,
									state->writepos_file,
									state->writepos_offset,
									SEEK_SET) != 0)
						elog(ERROR, "tuplestore seek failed");
				}
				else
				{
					if (BufFileSeek(state->myfile,
									dptr->file, dptr->offset,
									SEEK_SET) != 0)
						elog(ERROR, "tuplestore seek failed");
				}
			}
			else if (srcptr == state->activeptr)
			{
				if (!dptr->eof_reached)
					BufFileTell(state->myfile,
								&dptr->file,
								&dptr->offset);
			}
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * tuplestore_trim	- remove all no-longer-needed tuples
 *
 * Calling this function authorizes the tuplestore to delete all tuples
 * before the oldest read pointer, if no read pointer is marked as requiring
 * REWIND capability.
 *
 * Note: this is obviously safe if no pointer has BACKWARD capability either.
 * If a pointer is marked as BACKWARD but not REWIND capable, it means that
 * the pointer can be moved backward but not before the oldest other read
 * pointer.
 */
void
tuplestore_trim(Tuplestorestate *state)
{
	int			oldest;
	int			nremove;
	int			i;

	/*
	 * Truncation is disallowed if any read pointer requires rewind
	 * capability.
	 */
	if (state->eflags & EXEC_FLAG_REWIND)
		return;

	/*
	 * We don't bother trimming temp files since it usually would mean more
	 * work than just letting them sit in kernel buffers until they age out.
	 */
	if (state->status != TSS_INMEM)
		return;

	/* Find the oldest read pointer */
	oldest = state->memtupcount;
	for (i = 0; i < state->readptrcount; i++)
	{
		if (!state->readptrs[i].eof_reached)
			oldest = Min(oldest, state->readptrs[i].current);
	}

	/*
	 * Note: you might think we could remove all the tuples before the oldest
	 * "current", since that one is the next to be returned.  However, since
	 * tuplestore_gettuple returns a direct pointer to our internal copy of
	 * the tuple, it's likely that the caller has still got the tuple just
	 * before "current" referenced in a slot. So we keep one extra tuple
	 * before the oldest "current".  (Strictly speaking, we could require such
	 * callers to use the "copy" flag to tuplestore_gettupleslot, but for
	 * efficiency we allow this one case to not use "copy".)
	 */
	nremove = oldest - 1;
	if (nremove <= 0)
		return;					/* nothing to do */

	Assert(nremove >= state->memtupdeleted);
	Assert(nremove <= state->memtupcount);

	/* Release no-longer-needed tuples */
	for (i = state->memtupdeleted; i < nremove; i++)
	{
		FREEMEM(state, GetMemoryChunkSpace(state->memtuples[i]));
		pfree(state->memtuples[i]);
		/* by cywang, for reusing buffer */
		state->startPos++;
		state->memtuples[i] = NULL;
	}
	state->memtupdeleted = nremove;

	/* mark tuplestore as truncated (used for Assert crosschecks only) */
	state->truncated = true;

	/*
	 * If nremove is less than 1/8th memtupcount, just stop here, leaving the
	 * "deleted" slots as NULL.  This prevents us from expending O(N^2) time
	 * repeatedly memmove-ing a large pointer array.  The worst case space
	 * wastage is pretty small, since it's just pointers and not whole tuples.
	 */
	if (nremove < state->memtupcount / 8)
		return;

	/*
	 * Slide the array down and readjust pointers.
	 *
	 * In mergejoin's current usage, it's demonstrable that there will always
	 * be exactly one non-removed tuple; so optimize that case.
	 */
	if (nremove + 1 == state->memtupcount)
		state->memtuples[0] = state->memtuples[nremove];
	else
		memmove(state->memtuples, state->memtuples + nremove,
				(state->memtupcount - nremove) * sizeof(void *));

	state->memtupdeleted = 0;
	state->memtupcount -= nremove;
	for (i = 0; i < state->readptrcount; i++)
	{
		if (!state->readptrs[i].eof_reached)
			state->readptrs[i].current -= nremove;
	}
}

/*
 * tuplestore_in_memory
 *
 * Returns true if the tuplestore has not spilled to disk.
 *
 * XXX exposing this is a violation of modularity ... should get rid of it.
 */
bool
tuplestore_in_memory(Tuplestorestate *state)
{
	return (state->status == TSS_INMEM);
}


/*
 * Tape interface routines
 */

static unsigned int
getlen(Tuplestorestate *state, bool eofOK)
{
	unsigned int len;
	size_t		nbytes;

	nbytes = BufFileRead(state->myfile, (void *) &len, sizeof(len));
	if (nbytes == sizeof(len))
		return len;
	if (nbytes != 0)
		elog(ERROR, "unexpected end of tape");
	if (!eofOK)
		elog(ERROR, "unexpected end of data");
	return 0;
}


/*
 * Routines specialized for HeapTuple case
 *
 * The stored form is actually a MinimalTuple, but for largely historical
 * reasons we allow COPYTUP to work from a HeapTuple.
 *
 * Since MinimalTuple already has length in its first word, we don't need
 * to write that separately.
 */

static void *
copytup_heap(Tuplestorestate *state, void *tup)
{
	MinimalTuple tuple;

	tuple = minimal_tuple_from_heap_tuple((HeapTuple) tup);
	USEMEM(state, GetMemoryChunkSpace(tuple));
	return (void *) tuple;
}

static void
writetup_heap(Tuplestorestate *state, void *tup)
{
	MinimalTuple tuple = (MinimalTuple) tup;

	/* the part of the MinimalTuple we'll write: */
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

	/* total on-disk footprint: */
	unsigned int tuplen = tupbodylen + sizeof(int);

	InstrStartNode(state->instr_disk_write);

	if (BufFileWrite(state->myfile, (void *) &tuplen,
					 sizeof(tuplen)) != sizeof(tuplen))
		elog(ERROR, "write failed");
	if (BufFileWrite(state->myfile, (void *) tupbody,
					 tupbodylen) != (size_t) tupbodylen)
		elog(ERROR, "write failed");
	if (state->backward)		/* need trailing length word? */
		if (BufFileWrite(state->myfile, (void *) &tuplen,
						 sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "write failed");

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	heap_free_minimal_tuple(tuple);

	InstrStopNode(state->instr_disk_write, 0);
}

static void *
readtup_heap(Tuplestorestate *state, unsigned int len)
{
	unsigned int tupbodylen = len - sizeof(int);
	unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
	MinimalTuple tuple = (MinimalTuple) palloc(tuplen);
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;

	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* read in the tuple proper */
	tuple->t_len = tuplen;
	if (BufFileRead(state->myfile, (void *) tupbody,
					tupbodylen) != (size_t) tupbodylen)
		elog(ERROR, "unexpected end of data");
	if (state->backward)		/* need trailing length word? */
		if (BufFileRead(state->myfile, (void *) &tuplen,
						sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "unexpected end of data");
	return (void *) tuple;
}

/* add by cywang */
//#ifdef WIN_FUN_OPT
static void
opt_writetup_heap(Tuplestorestate *state, void *tup)
{
	MinimalTuple tuple = (MinimalTuple) tup;
	TupleTableSlot *slot = state->init_slot;

	/* the part of the MinimalTuple we'll write: */
	char	*tupbody;
	unsigned int tupbodylen;
	/* total on-disk footprint: */
	unsigned int tuplen;

	Datum		values[MAX_USEFUL_ATT_NUM];
	bool		isnull[MAX_USEFUL_ATT_NUM];
	MinimalTuple	opt_tuple;
	int			i;

	InstrStartNode(state->instr_disk_write);

	ExecStoreMinimalTuple(tuple, slot, false);

	slot_getsomeattrs(slot, state->opt_maxPos);

	memset(values, 0, sizeof(values));
	memset(isnull, 0, sizeof(isnull));
	for(i=1; i<=state->useful2init[0]; i++){
		values[i-1] = slot->tts_values[state->useful2init[i]-1];	/* note: elements of useful2init starts 1, while that of values starts 0 */
		isnull[i-1] = slot->tts_isnull[state->useful2init[i]-1];
	}
	opt_tuple = heap_form_minimal_tuple(state->opt_tupdesc, values, isnull);
	tupbody = (char*)opt_tuple+MINIMAL_TUPLE_DATA_OFFSET;
	tupbodylen = opt_tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;
	tuplen = tupbodylen + sizeof(int);

	if(BufFileWrite(state->opt_file, (void *)&tuplen, sizeof(tuplen)) != sizeof(tuplen))
		elog(ERROR, "write failed");
	if(BufFileWrite(state->opt_file, (void *)tupbody, tupbodylen) != (size_t)tupbodylen)
		elog(ERROR, "write failed");
	if(state->backward)
		if(BufFileWrite(state->opt_file, (void *)&tuplen, sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "write failed");

	/*
	 * for some reasons, we set here
	 *
	 * NOTE: move to opt_tuplestore_updatewritepos()
	 */
	//BufFileTell(state->opt_file, &state->opt_writepos_file, &state->opt_writepos_offset);

	//FREEMEM(state, GetMemoryChunkSpace(tuple));
	heap_free_minimal_tuple(opt_tuple);

	InstrStopNode(state->instr_disk_write, 0);
}

/*
 * opt_dumptuples - the opt version of dumptuples(), remove tuples from memory and write to tape
 *
 * As a side effect, we must convert each read pointer's position from
 * "current" to file/offset format.  But eof_reached pointers don't
 * need to change state.
 */
static void
opt_dumptuples(Tuplestorestate *state)
{
	int			i;

	//first need to initialize the read pointers for opt_file
	state->opt_activeptr = state->activeptr;
	state->opt_readptrcount = state->readptrcount;
	state->opt_readptrsize = state->readptrcount;	/* the readptrcount will no change any more, we set is as the opt size to save memory */
	if(state->opt_readptrs)
		pfree(state->opt_readptrs);
	state->opt_readptrs = (TSReadPointer *)
		palloc(state->opt_readptrsize * sizeof(TSReadPointer));

	/* set the init_readptr before the current_ptr is converted */
	state->init_readptr.current = state->readptrs[0].current;
	state->init_readptr.eflags = 0;	/* only need forward operation */
	state->init_readptr.eof_reached = state->readptrs[0].eof_reached;

	//can we use memcpy here?
	//memcpy(state->opt_readptrs, state->readptrs, state->opt_readptrsize * sizeof(TSReadPointer));
	for (i = 0; i < state->opt_readptrcount; i++){
		state->opt_readptrs[i].eflags = state->readptrs[i].eflags;
		state->opt_readptrs[i].eof_reached = state->readptrs[i].eof_reached;
		state->opt_readptrs[i].current = state->readptrs[i].current;
	}

	for (i = state->memtupdeleted;; i++)
	{
		TSReadPointer *opt_readptr = state->opt_readptrs;
		int			j;

		for (j = 0; j < state->opt_readptrcount; opt_readptr++, j++)
		{
			if (i == opt_readptr->current && !opt_readptr->eof_reached)
				BufFileTell(state->opt_file,
							&opt_readptr->file, &opt_readptr->offset);
		}
		if (i >= state->memtupcount)
			break;

		opt_writetup_heap(state, state->memtuples[i]);
		//WRITETUP(state, state->memtuples[i]);
	}
}

/*
 * set the required attributes of the buffer
 */
void tuplestore_init_opt(Tuplestorestate *state, TupleDesc opt_tupdesc, AttrNumber *useful2init, int opt_maxPos, TupleTableSlot *slot){
	state->opt_tupdesc = opt_tupdesc;
	state->useful2init = useful2init;
	state->opt_maxPos = opt_maxPos;
	state->init_slot = slot;
}

/*
 * dump the initial tuples to tape
 *
 * the difference with dumptuples is that init_dumptuples only support forward read
 */
void init_dumptuples(Tuplestorestate *state){
	int			i;

	for (i = state->memtupdeleted;; i++)
	{
		/*
		 * before this, the init_readptr need to be set to current_ptr before it is converted in opt_dumples
		 */
		if(i == state->init_readptr.current && !state->init_readptr.eof_reached)
			BufFileTell(state->init_file, &state->init_readptr.file, &state->init_readptr.offset);

		if (i >= state->memtupcount)
			break;
		init_writetup_heap(state, state->memtuples[i]);
	}
	/* NOTE: the following comment may need to remove when dumptuples() is in comment */
	state->memtupdeleted = 0;
	state->memtupcount = 0;
}

/*
 * store a initial tuple to tape
 *
 * the difference with writetup_heap is that init_writeup_heap only support forward read,
 * which means only stores one length attribute.
 *
 * NOTE: if we finally replace the dumptuples with opt_dumptuples and init_dumptuples, we need to free the input tuple
 */
void init_writetup_heap(Tuplestorestate *state, void *tup){
	MinimalTuple tuple = (MinimalTuple) tup;

	/* the part of the MinimalTuple we'll write: */
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

	/* total on-disk footprint: */
	unsigned int tuplen = tupbodylen + sizeof(int);

	InstrStartNode(state->instr_disk_write);

	if (BufFileWrite(state->init_file, (void *) &tuplen,
					 sizeof(tuplen)) != sizeof(tuplen))
		elog(ERROR, "write failed");
	if (BufFileWrite(state->init_file, (void *) tupbody,
					 tupbodylen) != (size_t) tupbodylen)
		elog(ERROR, "write failed");

	/*
	 * When get init tuple, we don't call tuplestore_select_read_pointer,
	 * we need to update the init_writepos_file and init_writepos_offset.
	 *
	 * NOTE: move to opt_tuplestore_updatewritepos()
	 */
	//BufFileTell(state->init_file, &state->init_writepos_file, &state->init_writepos_offset);

	/* NOTE: the following comment may need to remove when dumptuples() is in comment */
	FREEMEM(state, GetMemoryChunkSpace(tuple));
	heap_free_minimal_tuple(tuple);

	InstrStopNode(state->instr_disk_write, 0);
}
/*
 * When in TSS_READFILE, switch to the alternative method
 */
void opt_tuplestore_select_read_pointer(Tuplestorestate *state, int ptr){
	TSReadPointer *opt_readptr;
	TSReadPointer *opt_oldptr;

	if(tuplestore_in_memory(state))
		return tuplestore_select_read_pointer(state, ptr);

	Assert(ptr >= 0 && ptr < state->opt_readptrcount);

	/* No work if already active */
	if (ptr == state->opt_activeptr)
		return;

	opt_readptr = &state->opt_readptrs[ptr];
	opt_oldptr = &state->opt_readptrs[state->opt_activeptr];

	switch (state->status)
	{
		case TSS_INMEM:
		case TSS_WRITEFILE:
			/* no work */
			break;
		case TSS_READFILE:

			/*
			 * First, save the current read position in the pointer about to
			 * become inactive.
			 */
			if (!opt_oldptr->eof_reached)
				BufFileTell(state->opt_file,
							&opt_oldptr->file,
							&opt_oldptr->offset);

			/*
			 * We have to make the temp file's seek position equal to the
			 * logical position of the new read pointer.  In eof_reached
			 * state, that's the EOF, which we have available from the saved
			 * write position.
			 */
			if (opt_readptr->eof_reached)
			{
				if (BufFileSeek(state->opt_file,
								state->opt_writepos_file,
								state->opt_writepos_offset,
								SEEK_SET) != 0)
					elog(ERROR, "tuplestore seek failed");
			}
			else
			{
				if (BufFileSeek(state->opt_file,
								opt_readptr->file,
								opt_readptr->offset,
								SEEK_SET) != 0)
					elog(ERROR, "tuplestore seek failed");
			}
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}

	state->opt_activeptr = ptr;
}
/*
 * Convert an initial tuple slot to an opt tuple slot
 *
 * NOTE: the opt_slot can't be null for some reasons
 */
void tuplestore_convert_to_opt(Tuplestorestate *state, TupleTableSlot *slot, TupleTableSlot *opt_slot){
	Datum			values[MAX_USEFUL_ATT_NUM];
	bool			isnull[MAX_USEFUL_ATT_NUM];
	MinimalTuple	opt_tuple;
	int				i;

	slot_getsomeattrs(slot, state->opt_maxPos);

	memset(values, 0, sizeof(values));
	memset(isnull, 0, sizeof(isnull));
	for(i=0; i<state->useful2init[0]; i++){
		values[i] = slot->tts_values[state->useful2init[i]-1];	/* note: elements of useful2init starts 1, while that of values starts 0 */
		isnull[i] = slot->tts_isnull[state->useful2init[i]-1];
	}
	opt_tuple = heap_form_minimal_tuple(state->opt_tupdesc, values, isnull);
	ExecStoreMinimalTuple(opt_tuple, opt_slot, false);
}

/*
 * to make no effect to other component, we define a new set of interface function for window function
 * but is much similar with the original ones.
 */
bool opt_tuplestore_gettupleslot(Tuplestorestate *state, bool forward, bool copy, TupleTableSlot *slot){
	MinimalTuple tuple;
	bool		should_free;

	/* by cywang */
	if(tuplestore_in_memory(state)){
		InstrStartNode(state->instr_buffer_read);
	}else{
		InstrStartNode(state->instr_disk_read);
		if(state->in_locateheadpos)
			InstrStartNode(state->instr_locateheadpos_io);
	}

	/*
	 * NOTE: the type of the parameter slot need to be set outside
	 *
	 * actually, when opt_tuplestore_gettupleslot is called, the buffer is not in memory,
	 * so the type of the parameter slot will be opt.
	 */
	tuple = (MinimalTuple) opt_tuplestore_gettuple(state, forward, &should_free);
	/*
	 * the following is to test the tranverse of the whole partition
	 */
	/*
	while(true){
		tuple = (MinimalTuple) opt_tuplestore_gettuple(state, forward, &should_free);
		if(!tuple) break;
		if(should_free)
			heap_free_minimal_tuple(tuple);
	}
	*/

	if(tuplestore_in_memory(state)){
		InstrStopNode(state->instr_buffer_read, 0);
	}else{
		if(state->in_locateheadpos){
			InstrStopNode(state->instr_locateheadpos_io, 0);
		}
		InstrStopNode(state->instr_disk_read, 0);
	}

	if (tuple)
	{
		if (copy && !should_free)
		{
			tuple = heap_copy_minimal_tuple(tuple);
			should_free = true;
		}
		ExecStoreMinimalTuple(tuple, slot, should_free);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}
/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.	If should_free is set, the
 * caller must pfree the returned tuple when done with it.
 *
 * Backward scan is only allowed if randomAccess was set true or
 * EXEC_FLAG_BACKWARD was specified to tuplestore_set_eflags().
 */
static void *
opt_tuplestore_gettuple(Tuplestorestate *state, bool forward, bool *should_free)
{
	unsigned int tuplen;
	void	   *tup;

	TSReadPointer *opt_readptr = &state->opt_readptrs[state->opt_activeptr];

	if(tuplestore_in_memory(state))
		return tuplestore_gettuple(state, forward, should_free);

	Assert(forward || (opt_readptr->eflags & EXEC_FLAG_BACKWARD));

	/*
	 * no need to deal with the status in memory
	 */
	switch (state->status)
	{
		case TSS_WRITEFILE:

			/* Skip state change if we'll just return NULL */
			if (opt_readptr->eof_reached && forward)
				return NULL;
			/*
			 * first switch from writing to reading
			 */
			/*
			 * moved to opt_writeup_heap()
			 *
			 * NOTE: keep as close as to the original one
			 */
			BufFileTell(state->opt_file, &state->opt_writepos_file, &state->opt_writepos_offset);
			if(!opt_readptr->eof_reached)
				if(BufFileSeek(state->opt_file, opt_readptr->file, opt_readptr->offset, SEEK_SET) != 0)
					elog(ERROR, "tuplestore seek failed");

			state->status = TSS_READFILE;
			/* FALL THRU into READFILE case */

		case TSS_READFILE:
			*should_free = true;

			if (forward)
			{
				if ((tuplen = opt_getlen(state, true)) != 0)
				{
					tup = opt_readtup_heap(state, tuplen);
					return tup;
				}
				else
				{
					opt_readptr->eof_reached = true;
					return NULL;
				}
			}

			/*
			 * Backward.
			 *
			 * if all tuples are fetched already then we return last tuple,
			 * else tuple before last returned.
			 *
			 * Back up to fetch previously-returned tuple's ending length
			 * word. If seek fails, assume we are at start of file.
			 */
			if (BufFileSeek(state->opt_file, 0, -(long) sizeof(unsigned int),
							SEEK_CUR) != 0)
			{
				/* even a failed backwards fetch gets you out of eof state */
				opt_readptr->eof_reached = false;
				Assert(!state->truncated);
				return NULL;
			}
			tuplen = opt_getlen(state, false);

			if (opt_readptr->eof_reached)
			{
				opt_readptr->eof_reached = false;
				/* We will return the tuple returned before returning NULL */
			}
			else
			{
				/*
				 * Back up to get ending length word of tuple before it.
				 */
				if (BufFileSeek(state->opt_file, 0,
								-(long) (tuplen + 2 * sizeof(unsigned int)),
								SEEK_CUR) != 0)
				{
					/*
					 * If that fails, presumably the prev tuple is the first
					 * in the file.  Back up so that it becomes next to read
					 * in forward direction (not obviously right, but that is
					 * what in-memory case does).
					 */
					if (BufFileSeek(state->opt_file, 0,
									-(long) (tuplen + sizeof(unsigned int)),
									SEEK_CUR) != 0)
						elog(ERROR, "bogus tuple length in backward scan");
					Assert(!state->truncated);
					return NULL;
				}
				tuplen = opt_getlen(state, false);
			}

			/*
			 * Now we have the length of the prior tuple, back up and read it.
			 * Note: READTUP expects we are positioned after the initial
			 * length word of the tuple, so back up to that point.
			 */
			if (BufFileSeek(state->opt_file, 0,
							-(long) tuplen,
							SEEK_CUR) != 0)
				elog(ERROR, "bogus tuple length in backward scan");
			tup = opt_readtup_heap(state, tuplen);
			return tup;

		default:
			elog(ERROR, "invalid tuplestore state");
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * Tape interface routines
 */
static unsigned int
opt_getlen(Tuplestorestate *state, bool eofOK)
{
	unsigned int len;
	size_t		nbytes;

	nbytes = BufFileRead(state->opt_file, (void *) &len, sizeof(len));

	if (nbytes == sizeof(len))
		return len;
	if (nbytes != 0)
		elog(ERROR, "unexpected end of tape");
	if (!eofOK)
		elog(ERROR, "unexpected end of data");
	return 0;
}

static void *
opt_readtup_heap(Tuplestorestate *state, unsigned int len)
{
	unsigned int tupbodylen = len - sizeof(int);
	unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
	MinimalTuple tuple = (MinimalTuple) palloc(tuplen);
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;

	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* read in the tuple proper */
	tuple->t_len = tuplen;
	if (BufFileRead(state->opt_file, (void *) tupbody,
					tupbodylen) != (size_t) tupbodylen)
		elog(ERROR, "unexpected end of data");
	if (state->backward)		/* need trailing length word? */
		if (BufFileRead(state->opt_file, (void *) &tuplen,
						sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "unexpected end of data");
	return (void *) tuple;
}

bool
opt_tuplestore_advance(Tuplestorestate *state, bool forward)
{
	void	   *tuple;
	bool		should_free;

	if(tuplestore_in_memory(state))
		return tuplestore_advance(state, forward);
	else
		tuple = opt_tuplestore_gettuple(state, forward, &should_free);

	if (tuple)
	{
		if (should_free)
			pfree(tuple);
		return true;
	}
	else
	{
		return false;
	}
}

/*
 * When buffer is in memory, we can't just return tuplestore_puttuple_common(),
 * bacause the status may change during this function.
 *
 * NOTE: important function
 */
static void
opt_tuplestore_puttuple_common(Tuplestorestate *state, void *tuple)
{
	TSReadPointer *readptr;
	int			i;
	ResourceOwner oldowner;

//#ifdef WIN_FUN_OPT
	TSReadPointer *opt_readptr;
	Instrumentation *instr=InstrAlloc(1,1);
//#endif

	switch (state->status)
	{
		case TSS_INMEM:

			InstrStartNode(state->instr_buffer_write);

			/*
			 * Update read pointers as needed; see API spec above.
			 */
			readptr = state->readptrs;
			for (i = 0; i < state->readptrcount; readptr++, i++)
			{
				if (readptr->eof_reached && i != state->activeptr)
				{
					readptr->eof_reached = false;
					readptr->current = state->memtupcount;
				}
			}

			/*
			 * Grow the array as needed.  Note that we try to grow the array
			 * when there is still one free slot remaining --- if we fail,
			 * there'll still be room to store the incoming tuple, and then
			 * we'll switch to tape-based operation.
			 */
			if (state->memtupcount >= state->memtupsize - 1)
			{
				/*
				 * See grow_memtuples() in tuplesort.c for the rationale
				 * behind these two tests.
				 */
				if (state->availMem > (long) (state->memtupsize * sizeof(void *)) &&
					(Size) (state->memtupsize * 2) < MaxAllocSize / sizeof(void *))
				{
					FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
					state->memtupsize *= 2;
					state->memtuples = (void **)
						repalloc(state->memtuples,
								 state->memtupsize * sizeof(void *));
					USEMEM(state, GetMemoryChunkSpace(state->memtuples));
				}
			}

			/* Stash the tuple in the in-memory array */
			state->memtuples[state->memtupcount++] = tuple;

			InstrStopNode(state->instr_buffer_write, 0);

			/*
			 * Done if we still fit in available memory and have array slots.
			 */
			if (state->memtupcount < state->memtupsize && !LACKMEM(state))
				return;

			/*
			 * Nope; time to switch to tape-based operation.  Make sure that
			 * the temp file(s) are created in suitable temp tablespaces.
			 */
			PrepareTempTablespaces();

			/* associate the file with the store's resource owner */
			oldowner = CurrentResourceOwner;
			CurrentResourceOwner = state->resowner;

//#ifdef WIN_FUN_OPT
			if(enable_winfunopt){
				state->opt_file = BufFileCreateTemp(state->interXact);
				state->init_file = BufFileCreateTemp(state->interXact);
			}else{
//#else
				state->myfile = BufFileCreateTemp(state->interXact);
			}
//#endif

			CurrentResourceOwner = oldowner;

			/*
			 * Freeze the decision about whether trailing length words will be
			 * used.  We can't change this choice once data is on tape, even
			 * though callers might drop the requirement.
			 */
			state->backward = (state->eflags & EXEC_FLAG_BACKWARD) != 0;
			state->status = TSS_WRITEFILE;

			/* add by cywang */
			InstrStartNode(instr);
//#ifdef WIN_FUN_OPT
			/* we need to dump tuples to two files.
			 * one for tuple fetch, which means we only need to keep on read pointer, and no need to fetch backward.
			 * the other for function computing, to reduce IO cost, it only contains useful attributes.
			 */
			if(enable_winfunopt){
				opt_dumptuples(state);
				init_dumptuples(state);
			}else{
//#else
				dumptuples(state);
			}
//#endif
			InstrStopNode(instr, 0);
			printf("dump memory: %f\n", INSTR_TIME_GET_DOUBLE(instr->counter));

			break;
		case TSS_WRITEFILE:

//#ifdef WIN_FUN_OPT
			if(enable_winfunopt){
				/* for opt tuple */
				opt_readptr = state->opt_readptrs;
				for(i=0; i<state->opt_readptrcount; opt_readptr++, i++){
					if(opt_readptr->eof_reached && i!=state->opt_activeptr){
						opt_readptr->eof_reached = false;
						BufFileTell(state->opt_file, &opt_readptr->file, &opt_readptr->offset);
					}
				}
				opt_writetup_heap(state, tuple);

				/*
				 * for initial tuple
				 * no need to set the read pointers, because init_file only support forward read,
				 * and it's only used by init_readptr, which is advanced when the current row is read.
				 * in ExecWindowAgg()?
				 */
				if(state->init_readptr.eof_reached){
					state->init_readptr.eof_reached = false;
					BufFileTell(state->init_file, &state->init_readptr.file, &state->init_readptr.offset);
				}
				/*
				 * as there is no switch of status for init_readptr,
				 * we need to check or set before and after every read and write operation.
				 *
				 * commented, currently, don't cause bugs.
				 */
				//if(BufFileSeek(state->init_file, state->init_writepos_file, state->init_writepos_offset, SEEK_SET) != 0)
				//	elog(ERROR, "tuplestore seek to EOF failed");

				init_writetup_heap(state, tuple);
			}else{
//#else
				/*
				 * Update read pointers as needed; see API spec above. Note:
				 * BufFileTell is quite cheap, so not worth trying to avoid
				 * multiple calls.
				 */
				readptr = state->readptrs;
				for (i = 0; i < state->readptrcount; readptr++, i++)
				{
					if (readptr->eof_reached && i != state->activeptr)
					{
						readptr->eof_reached = false;
						BufFileTell(state->myfile,
									&readptr->file,
									&readptr->offset);
					}
				}

				WRITETUP(state, tuple);
			}
//#endif
			break;
		case TSS_READFILE:

//#ifdef WIN_FUN_OPT
			if(enable_winfunopt){
				/*
				 * to switch from read status to write statue,
				 * first we need to save the active pointer that is reading,
				 * then we need to change the BufFile's pos to the EOF,
				 * so the appending write can happen.
				 */
				//switch from reading to writing of opt_file
				if(!state->opt_readptrs[state->opt_activeptr].eof_reached)
					BufFileTell(state->opt_file, &state->opt_readptrs[state->opt_activeptr].file, &state->opt_readptrs[state->opt_activeptr].offset);
				if(BufFileSeek(state->opt_file, state->opt_writepos_file, state->opt_writepos_offset, SEEK_SET) != 0)
					elog(ERROR, "OPT: tuplestore seek to EOF failed");
				opt_readptr = state->opt_readptrs;
				//update opt read pointers
				for(i=0; i<state->opt_readptrcount; opt_readptr++, i++){
					if(opt_readptr->eof_reached && i!=state->opt_activeptr){
						opt_readptr->eof_reached = false;
						opt_readptr->file = state->opt_writepos_file;
						opt_readptr->offset = state->opt_writepos_offset;
					}
				}
				//opt_writetup_heap(state, tuple);

				if(state->init_readptr.eof_reached){
					state->init_readptr.eof_reached = false;
					BufFileTell(state->init_file, &state->init_readptr.file, &state->init_readptr.offset);
				}
				//TODO: to check
				if(BufFileSeek(state->init_file, state->init_writepos_file, state->init_writepos_offset, SEEK_SET) != 0)
					elog(ERROR, "tuplestore seek to EOF failed");
				//init_writetup_heap(state, tuple);

				state->status = TSS_WRITEFILE;

				/*
				 * take the state->status into consideration,
				 * we do the real write a little later
				 */
				opt_writetup_heap(state, tuple);
				init_writetup_heap(state, tuple);
			}else{
//#else
				/*
				 * Switch from reading to writing.
				 */
				if (!state->readptrs[state->activeptr].eof_reached)
					BufFileTell(state->myfile,
								&state->readptrs[state->activeptr].file,
								&state->readptrs[state->activeptr].offset);
				if (BufFileSeek(state->myfile,
								state->writepos_file, state->writepos_offset,
								SEEK_SET) != 0)
					elog(ERROR, "tuplestore seek to EOF failed");

				/*
				 * Update read pointers as needed; see API spec above.
				 */
				readptr = state->readptrs;
				for (i = 0; i < state->readptrcount; readptr++, i++)
				{
					if (readptr->eof_reached && i != state->activeptr)
					{
						readptr->eof_reached = false;
						readptr->file = state->writepos_file;
						readptr->offset = state->writepos_offset;
					}
				}
				state->status = TSS_WRITEFILE;

				WRITETUP(state, tuple);
			}
//#endif
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * here we can't simply return tuplestore_puttupleslot() when buffer is in memory,
 * because during this function, the status may switch to tape.
 *
 * NOTE: the parameter slot is an initial tuple slot
 */
void opt_tuplestore_puttupleslot(Tuplestorestate *state, TupleTableSlot *slot){
	MinimalTuple tuple;
	MemoryContext oldcxt = MemoryContextSwitchTo(state->context);

	/*
	 * Form a MinimalTuple in working memory
	 */
	tuple = ExecCopySlotMinimalTuple(slot);
	USEMEM(state, GetMemoryChunkSpace(tuple));
//#ifdef WIN_FUN_OPT
	if(enable_winfunopt)
		opt_tuplestore_puttuple_common(state, (void *) tuple);
//#else
	else
		tuplestore_puttuple_common(state, (void *) tuple);
//#endif

	MemoryContextSwitchTo(oldcxt);
}

/*
 * opt_tuplestore_end
 *
 *	Release resources and clean up.
 */
void
opt_tuplestore_end(Tuplestorestate *state)
{
	int			i;

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		if(state->opt_file)
			BufFileClose(state->opt_file);
		if(state->init_file)
			BufFileClose(state->init_file);
		if(state->opt_readptrs)
			pfree(state->opt_readptrs);
	}
//#endif

	if (state->myfile)
		BufFileClose(state->myfile);
	if (state->memtuples)
	{
		for (i = state->memtupdeleted; i < state->memtupcount; i++)
			pfree(state->memtuples[i]);
		pfree(state->memtuples);
	}
	pfree(state->readptrs);
	pfree(state);
}

/*
 * When on tape, to get the current row to process,
 */
bool init_tuplestore_gettupleslot(Tuplestorestate *state, bool forward, bool copy, TupleTableSlot *slot){
	MinimalTuple tuple;
	bool		should_free;

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt && !tuplestore_in_memory(state)){
		tuple = (MinimalTuple) init_tuplestore_gettuple(state, forward, &should_free);

		/*
		 * we use the following code to test if we can tranverse through the init tuples correctly
		 */
		/*
		while(true){
			tuple = (MinimalTuple) init_tuplestore_gettuple(state, forward, &should_free);
			if(!tuple) break;
			if(should_free){
				heap_free_minimal_tuple(tuple);
			}
		}
		*/
	}else{
//#endif
		tuple = (MinimalTuple) tuplestore_gettuple(state, forward, &should_free);
//#ifdef WIN_FUN_OPT
	}
//#endif

	if (tuple)
	{
		if (copy && !should_free)
		{
			tuple = heap_copy_minimal_tuple(tuple);
			should_free = true;
		}
		ExecStoreMinimalTuple(tuple, slot, should_free);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

/*
 * Fetch the next tuple in init_file to get the current row when buffer is on tape
 *
 */
static void *
init_tuplestore_gettuple(Tuplestorestate *state, bool forward,
					bool *should_free)
{
	TSReadPointer *readptr = &state->init_readptr;
	unsigned int tuplen;
	void	   *tup;

	/* just to make sure not in memory */
	if(tuplestore_in_memory(state))
		return tuplestore_gettuple(state, forward, should_free);

	Assert(forward); /* init_file only support forward access */

	/*
	 * init not make any influence for the statue
	 */
	if (readptr->eof_reached && forward)
		return NULL;

	/* eof */
	if(readptr->file == state->writepos_file && readptr->offset==state->writepos_offset){
		readptr->eof_reached = true;
		return NULL;
	}

	/*
	 * the init_file's current postion is not equal to readptr's position,
	 * need BufFileSeek()
	 */
	if(readptr->file != opt_BufFileGetFile(state->init_file) || readptr->offset!=opt_BufFileGetOffset(state->init_file))
		if (BufFileSeek(state->init_file,
						readptr->file, readptr->offset,
						SEEK_SET) != 0)
			elog(ERROR, "tuplestore seek failed");

	*should_free = true;

	if ((tuplen = init_getlen(state, true)) != 0)
	{
		//tup = READTUP(state, tuplen);
		tup = init_readtup_heap(state, tuplen);

		/*
		 * is this is why the bug occurs?
		 */
		BufFileTell(state->init_file, &readptr->file, &readptr->offset);

		return tup;
	}
	else
	{
		readptr->eof_reached = true;
		return NULL;
	}
}

static unsigned int init_getlen(Tuplestorestate *state, bool eofOK){
	unsigned int len;
	size_t		nbytes;

	nbytes = BufFileRead(state->init_file, (void *) &len, sizeof(len));
	if (nbytes == sizeof(len))
		return len;
	if (nbytes != 0)
		elog(ERROR, "unexpected end of tape");
	if (!eofOK)
		elog(ERROR, "unexpected end of data");
	return 0;
}

/*
 * Then format of init_file is {[len, body], [len, body]...}
 */
static void *init_readtup_heap(Tuplestorestate *state, unsigned int len){
	unsigned int tupbodylen = len - sizeof(int);
	unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
	MinimalTuple tuple = (MinimalTuple) palloc(tuplen);
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;

	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* read in the tuple proper */
	tuple->t_len = tuplen;
	if (BufFileRead(state->init_file, (void *) tupbody,
					tupbodylen) != (size_t) tupbodylen)
		elog(ERROR, "unexpected end of data");
	return (void *) tuple;
}

/*
 * The read pointer for init_file is fixed.
 *
 * When on tape, the WindowAggState.current_ptr will not be used to select tuples any more,
 * But it have to be used to switch the active pointer.
 *
 * NOTE: ptr==current_ptr
 */
void init_tuplestore_select_read_pointer(Tuplestorestate *state, int ptr){
	TSReadPointer *opt_readptr;
	TSReadPointer *opt_oldptr;

	Assert(ptr >= 0 && ptr < state->readptrcount);

	/* No work if already active */
	if (ptr == state->opt_activeptr)
		return;

	opt_readptr = &state->opt_readptrs[ptr];
	opt_oldptr = &state->opt_readptrs[state->opt_activeptr];

	switch (state->status)
	{
		case TSS_INMEM:
		case TSS_WRITEFILE:
			/* no work */
			break;
		case TSS_READFILE:

			/*
			 * First, save the current read position in the pointer about to
			 * become inactive.
			 */
			if (!opt_oldptr->eof_reached)
				BufFileTell(state->opt_file,
							&opt_oldptr->file,
							&opt_oldptr->offset);

			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}

	state->opt_activeptr = ptr;
}

/*
 * output the time of loading file blocks
 *
 * WindowAggState can't access the attributes of Tuplestorestate directly,
 * and Tuplestorestate can't access the attributes of BufFile directly,
 * we have to give the interface here.
 */
void opt_tuplestore_instr_BufFile(Tuplestorestate *state){
	if(state->myfile){
		printf("for myfile: \n");
		opt_BufFileInstr(state->myfile);
	}

	if(state->init_file){
		printf("for init_file: \n");
		opt_BufFileInstr(state->init_file);
	}

	if(state->opt_file){
		printf("for opt_file: \n");
		opt_BufFileInstr(state->opt_file);
	}
}

/*
 * to avoid update write position so frequently
 */
void opt_tuplestore_updatewritepos(Tuplestorestate *state){
	//BufFileTell(state->opt_file, &state->opt_writepos_file, &state->opt_writepos_offset);
	BufFileTell(state->init_file, &state->init_writepos_file, &state->init_writepos_offset);
}

void opt_tuplestore_instr_TupleStore(Tuplestorestate *state){
	/*
	 * by cywang
	 *
	 * output the time cost, and reset the time's counter
	 */
	printf("TupleStore, buffer read: %f seconds \n", INSTR_TIME_GET_DOUBLE(state->instr_buffer_read->counter));
	INSTR_TIME_SET_ZERO(state->instr_buffer_read->counter);

	printf("TupleStore, buffer write: %f seconds \n", INSTR_TIME_GET_DOUBLE(state->instr_buffer_write->counter));
	INSTR_TIME_SET_ZERO(state->instr_buffer_write->counter);

	printf("TupleStore, disk read: %f seconds \n", INSTR_TIME_GET_DOUBLE(state->instr_disk_read->counter));
	INSTR_TIME_SET_ZERO(state->instr_disk_read->counter);

	printf("TupleStore, disk write: %f seconds \n\n", INSTR_TIME_GET_DOUBLE(state->instr_disk_write->counter));
	INSTR_TIME_SET_ZERO(state->instr_disk_write->counter);

	printf("TupleStore, IO cost in locating the frame head position: %f seconds \n", INSTR_TIME_GET_DOUBLE(state->instr_locateheadpos_io->counter));
	INSTR_TIME_SET_ZERO(state->instr_locateheadpos_io->counter);
}

/*
 * can't access in_locateheadpos from outside
 *
 * we have to give the api here.
 */
void opt_tuplestore_set_locateheadpos(Tuplestorestate *state, bool flag){
	state->in_locateheadpos = flag;
}

/*
 *
 */
void opt_tuplestore_copy_frameheadptr(Tuplestorestate *state){

}

void opt_tuplestore_update_frameheadptr(Tuplestorestate *state, int64 distance){

}

void opt_tuplestore_init_frameheadptr(Tuplestorestate *state){

}

void opt_tuplestore_copy_ptr(Tuplestorestate *state, int dst, int src){
	/*
	TSReadPointer dst_ptr = state->readptrs[dst];
	TSReadPointer src_ptr = state->readptrs[src];
	dst_ptr.current = src_ptr.current;
	dst_ptr.eflags = src_ptr.eflags;
	dst_ptr.eof_reached = src_ptr.eof_reached;
	dst_ptr.file = src_ptr.file;
	dst_ptr.offset = src_ptr.offset;
	*/

	/*
	 * Must use pointer, or the value will not be changed!
	 */
	TSReadPointer *dst_ptr;
	TSReadPointer *src_ptr;

	if(enable_winfunopt && !tuplestore_in_memory(state)){
		dst_ptr = &state->opt_readptrs[dst];
		src_ptr = &state->opt_readptrs[src];
	}else{
		dst_ptr = &state->readptrs[dst];
		src_ptr = &state->readptrs[src];
	}

	dst_ptr->current = src_ptr->current;
	//dst_ptr->eflags = src_ptr->eflags;	/* we can't change the eflags */
	dst_ptr->eof_reached = src_ptr->eof_reached;
	dst_ptr->file = src_ptr->file;
	dst_ptr->offset = src_ptr->offset;
}

/*
 * set the opt_file or myfile's current position, FOR locating the frame head position.
 *
 * we can't access opt_file or myfile from the outside,
 * we have to give an interface here.
 */
void opt_tuplestore_seek_ptr(Tuplestorestate *state, int ptr){
	TSReadPointer *readptr;

	if(tuplestore_in_memory(state))
		return;

	if(enable_winfunopt){
		Assert(ptr >= 0 && ptr < state->opt_readptrcount);
		readptr = &state->opt_readptrs[ptr];
	}else{
		Assert(ptr >= 0 && ptr < state->readptrcount);
		readptr = &state->readptrs[ptr];
	}

	switch (state->status)
	{
		case TSS_INMEM:
		case TSS_WRITEFILE:
			/* no work */
			break;
		case TSS_READFILE:
			/*
			 * We have to make the temp file's seek position equal to the
			 * logical position of the new read pointer.  In eof_reached
			 * state, that's the EOF, which we have available from the saved
			 * write position.
			 */
			if(enable_winfunopt){
				if (readptr->eof_reached)
				{
					if (BufFileSeek(state->opt_file,
									state->opt_writepos_file,
									state->opt_writepos_offset,
									SEEK_SET) != 0)
						elog(ERROR, "tuplestore seek failed");
				}
				else
				{
					if (BufFileSeek(state->opt_file,
									readptr->file,
									readptr->offset,
									SEEK_SET) != 0)
						elog(ERROR, "tuplestore seek failed");
				}
			}else{
				if (readptr->eof_reached)
				{
					if (BufFileSeek(state->myfile,
									state->writepos_file,
									state->writepos_offset,
									SEEK_SET) != 0)
						elog(ERROR, "tuplestore seek failed");
				}
				else
				{
					if (BufFileSeek(state->myfile,
									readptr->file,
									readptr->offset,
									SEEK_SET) != 0)
						elog(ERROR, "tuplestore seek failed");
				}
			}
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * if not in memory, we need to set the bufFile's offset to opt_tempTransEndPtr.
 *
 * NOTE: not just for on tape, but also for memory, or the answer will contains error
 */
void opt_tuplestore_tell_ptr(Tuplestorestate *state, int ptr){
	TSReadPointer *readptr;

	if(tuplestore_in_memory(state)){
		state->readptrs[ptr].current = state->readptrs[state->activeptr].current;
		return;
	}

	if(enable_winfunopt){
		Assert(ptr >= 0 && ptr < state->opt_readptrcount);
		readptr = &state->opt_readptrs[ptr];

		BufFileTell(state->opt_file, &readptr->file, &readptr->offset);
	}else{
		Assert(ptr >= 0 && ptr < state->readptrcount);
		readptr = &state->readptrs[ptr];
		BufFileTell(state->myfile, &readptr->file, &readptr->offset);
	}
}

static void
reuse_dumptubles(Tuplestorestate *state)
{
	int			i;

	for (i = state->memtupdeleted;; i++)
	{
		TSReadPointer *readptr = state->readptrs;
		int			j;

		for (j = 0; j < state->readptrcount; readptr++, j++)
		{
			if (i == readptr->current && !readptr->eof_reached)
				BufFileTell(state->myfile,
							&readptr->file, &readptr->offset);
		}

		/* by cywang, for reusing buffer */
		if(i == state->memtupcount && enable_reusebuffer){
			TSReadPointer *temp_ptr = state->readptrs;
			state->reuse_ptr = tuplestore_alloc_read_pointer(state, 0);
			temp_ptr += state->reuse_ptr;
			temp_ptr->eof_reached = false;
			BufFileTell(state->myfile, &temp_ptr->file, &temp_ptr->offset);
		}

		if (i >= state->memtupcount)
			break;
		//WRITETUP(state, state->memtuples[i]);
		/* by cywang, for reusing buffer */
		reuse_writetup_heap(state, state->memtuples[i]);
	}

	/* by cywang, for reusing buffer */
	state->startIndex = state->memtupdeleted;
	state->memtupcount -= state->memtupdeleted;

	state->memtupdeleted = 0;
	//state->memtupcount = 0;
}


static void
reuse_writetup_heap(Tuplestorestate *state, void *tup)
{
	MinimalTuple tuple = (MinimalTuple) tup;

	/* the part of the MinimalTuple we'll write: */
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

	/* total on-disk footprint: */
	unsigned int tuplen = tupbodylen + sizeof(int);

	InstrStartNode(state->instr_disk_write);

	if (BufFileWrite(state->myfile, (void *) &tuplen,
					 sizeof(tuplen)) != sizeof(tuplen))
		elog(ERROR, "write failed");
	if (BufFileWrite(state->myfile, (void *) tupbody,
					 tupbodylen) != (size_t) tupbodylen)
		elog(ERROR, "write failed");
	if (state->backward)		/* need trailing length word? */
		if (BufFileWrite(state->myfile, (void *) &tuplen,
						 sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "write failed");

	//FREEMEM(state, GetMemoryChunkSpace(tuple));
	//heap_free_minimal_tuple(tuple);

	InstrStopNode(state->instr_disk_write, 0);
}
/*
 * concat a tuple to the buffer for reusing
 *
 * TODO: update the pointer that point to the tail of the buffer
 */
void
reuse_tuplestore_puttuple(Tuplestorestate *state, void *tuple, int64 pos){

	MemoryContext oldcxt;

	/* the buffer must be consecutive */
	if(state->startPos+state->memtupcount != pos || state->memtupcount >= state->memtupsize || state->status == TSS_INMEM){
		return;
	}

	oldcxt = MemoryContextSwitchTo(state->context);

	/*
	 * Copy the tuple.  (Must do this even in WRITEFILE case.  Note that
	 * COPYTUP includes USEMEM, so we needn't do that here.)
	 */
	//tuple = COPYTUP(state, tuple);
	tuple = heap_copy_minimal_tuple(tuple);

	/* Stash the tuple in the in-memory array */
	//state->memtuples[state->memtupcount++] = tuple;
	state->memtuples[(state->startIndex+state->memtupcount)%state->memtupsize] = tuple;
	state->memtupcount++;

	opt_tuplestore_copy_ptr(state, state->reuse_ptr, state->activeptr);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * by cywang, for reusing buffer
 *
 * use a different trim strategy by treating the buffer array as a circle
 */
void reuse_tuplestore_trim(Tuplestorestate *state, int64 frameheadpos){
	int		i;
	int64	j;
	MinimalTuple tuple;

	if (state->status == TSS_INMEM || state->memtupcount<=0)
		return;

	for(i=state->startIndex,j=state->startPos; j<frameheadpos; i++,j++){
		tuple = state->memtuples[i%state->memtupsize];
		FREEMEM(state, GetMemoryChunkSpace(tuple));
		heap_free_minimal_tuple(tuple);
		state->memtupcount--;
		state->startIndex++;
		state->startPos++;
	}
	state->startIndex %= state->memtupsize;
}

bool reuse_tuplestore_gettupleslot_direct(Tuplestorestate *state, int64 pos, TupleTableSlot *slot){
	MinimalTuple	tuple;
	int				dis;

	if(pos >= state->startPos && pos < state->startPos+state->memtupcount){
		dis = pos - state->startPos;
		tuple = state->memtuples[(state->startIndex+dis)%state->memtupsize];
		ExecStoreMinimalTuple(tuple, slot, false); /* in buffer, should not be free */
		return true;
	}

	return false;
}

bool
reuse_tuplestore_gettupleslot(Tuplestorestate *state, bool forward,
						bool copy, TupleTableSlot *slot, int64 pos)
{
	MinimalTuple tuple;
	bool		should_free;

	/* by cywang */
	if(tuplestore_in_memory(state)){
		InstrStartNode(state->instr_buffer_read);
	}else{
		InstrStartNode(state->instr_disk_read);
		if(state->in_locateheadpos)
			InstrStartNode(state->instr_locateheadpos_io);
	}

	tuple = (MinimalTuple) tuplestore_gettuple(state, forward, &should_free);

	if(tuplestore_in_memory(state)){
		InstrStopNode(state->instr_buffer_read, 0);
	}else{
		InstrStopNode(state->instr_disk_read, 0);
		if(state->in_locateheadpos)
			InstrStopNode(state->instr_locateheadpos_io, 0);
	}

	if (tuple)
	{
		if (copy && !should_free)
		{
			tuple = heap_copy_minimal_tuple(tuple);
			should_free = true;
		}
		/*
		 * by cywang, for reusing buffer
		 */
		reuse_tuplestore_puttuple(state, tuple, pos);

		ExecStoreMinimalTuple(tuple, slot, should_free);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

void reuse_tuplestore_clear(Tuplestorestate *state){
	int			i;
	MinimalTuple tuple;

	for (i = 0; i<state->memtupcount; i++){
		tuple = state->memtuples[(state->startIndex+i)%state->memtupsize];
		FREEMEM(state, GetMemoryChunkSpace(tuple));
		heap_free_minimal_tuple(tuple);
	}
	state->memtupdeleted = 0;
	state->memtupcount = 0;
}

bool reuse_tuplestore_has_reuseptr(Tuplestorestate *state){
	if(state->reuse_ptr >= 0)
		return true;
	return false;
}
int64 reuse_tuplestore_copy_reuseptr(Tuplestorestate *state){
	opt_tuplestore_copy_ptr(state, state->reuse_ptr, state->activeptr);
	return state->startPos+state->memtupcount;
}
//#endif
