/*-------------------------------------------------------------------------
 *
 * nodeWindowAgg.c
 *	  routines to handle WindowAgg nodes.
 *
 * A WindowAgg node evaluates "window functions" across suitable partitions
 * of the input tuple set.	Any one WindowAgg works for just a single window
 * specification, though it can evaluate multiple window functions sharing
 * identical window specifications.  The input tuples are required to be
 * delivered in sorted order, with the PARTITION BY columns (if any) as
 * major sort keys and the ORDER BY columns (if any) as minor sort keys.
 * (The planner generates a stack of WindowAggs with intervening Sort nodes
 * as needed, if a query involves more than one window specification.)
 *
 * Since window functions can require access to any or all of the rows in
 * the current partition, we accumulate rows of the partition into a
 * tuplestore.	The window functions are called using the WindowObject API
 * so that they can access those rows as needed.
 *
 * We also support using plain aggregate functions as window functions.
 * For these, the regular Agg-node environment is emulated for each partition.
 * As required by the SQL spec, the output represents the value of the
 * aggregate function over all rows in the current row's window frame.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeWindowAgg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeWindowAgg.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "windowapi.h"

#include "executor/instrument.h"
//#ifdef WIN_FUN_OPT
#include "utils/tuplestore.h"
//#endif

/* by cywang */
bool enable_winfunopt = false;
bool enable_locate = true;
bool enable_recompute = true;
bool enable_reusebuffer = false;
int recompute_k = 100;
int recompute_num = 1;

/*
 * All the window function APIs are called with this object, which is passed
 * to window functions as fcinfo->context.
 */
typedef struct WindowObjectData
{
	NodeTag		type;
	WindowAggState *winstate;	/* parent WindowAggState */
	List	   *argstates;		/* ExprState trees for fn's arguments */
	void	   *localmem;		/* WinGetPartitionLocalMemory's chunk */
	int			markptr;		/* tuplestore mark pointer for this fn */
	int			readptr;		/* tuplestore read pointer for this fn */
	int64		markpos;		/* row that markptr is positioned on */
	int64		seekpos;		/* row that readptr is positioned on */

	/* add by cywang */
	List		*opt_argstates;	/* the backup arguments for a window function */
	/*
	 * to locate frame head efficiently, currently only for window aggregation,
	 * may be active during update.
	 */
	int			opt_frameheadptr;
	int64		opt_frameheadpos;

	/*
	 * to locate the temporary transition value's end position efficiently,
	 * only for window aggregation currently.
	 * will never be active!
	 */
	int			opt_tempTransEndPtr[16];
	int64		opt_tempTransEndPos[16];

	int64		opt_tempStartPos[16];	/* the start position of the temporary transition value, for checking where to jump */

	//bool		opt_needTempTransValue;
	//bool		opt_frameheadeverchanged;
} WindowObjectData;

/*
 * We have one WindowStatePerFunc struct for each window function and
 * window aggregate handled by this node.
 */
typedef struct WindowStatePerFuncData
{
	/* Links to WindowFunc expr and state nodes this working state is for */
	WindowFuncExprState *wfuncstate;
	WindowFunc *wfunc;

	int			numArguments;	/* number of arguments */

	FmgrInfo	flinfo;			/* fmgr lookup data for window function */

	Oid			winCollation;	/* collation derived for window function */

	/*
	 * We need the len and byval info for the result of each function in order
	 * to know how to copy/delete values.
	 */
	int16		resulttypeLen;
	bool		resulttypeByVal;

	bool		plain_agg;		/* is it just a plain aggregate function? */
	int			aggno;			/* if so, index of its PerAggData */

	WindowObject winobj;		/* object used in window function API */

	/* add by cywang */
	//WindowFuncExprState *opt_wfuncstate;	/* seems no need. if TSS_INMEM, use wfuncstate; or use this one when partition is spooled to tape */

}	WindowStatePerFuncData;

/*
 * For plain aggregate window functions, we also have one of these.
 */
typedef struct WindowStatePerAggData
{
	/* Oids of transfer functions */
	Oid			transfn_oid;
	Oid			finalfn_oid;	/* may be InvalidOid */

	/*
	 * fmgr lookup data for transfer functions --- only valid when
	 * corresponding oid is not InvalidOid.  Note in particular that fn_strict
	 * flags are kept here.
	 */
	FmgrInfo	transfn;
	FmgrInfo	finalfn;

	/*
	 * initial value from pg_aggregate entry
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * cached value for current frame boundaries
	 */
	Datum		resultValue;
	bool		resultValueIsNull;

	/*
	 * We need the len and byval info for the agg's input, result, and
	 * transition data types in order to know how to copy/delete values.
	 */
	int16		inputtypeLen,
				resulttypeLen,
				transtypeLen;
	bool		inputtypeByVal,
				resulttypeByVal,
				transtypeByVal;

	int			wfuncno;		/* index of associated PerFuncData */

	/* Current transition value */
	Datum		transValue;		/* current transition value */
	bool		transValueIsNull;

	bool		noTransValue;	/* true if transValue not set yet */


	/* by cywang, for reducing recompute*/
	Datum		opt_temp_transValue[16];	/* the temporary value to reduce recompute*/
	bool		opt_temp_transValueIsNull[16];
	bool		opt_temp_noTransValue[16];
} WindowStatePerAggData;

static void initialize_windowaggregate(WindowAggState *winstate,
						   WindowStatePerFunc perfuncstate,
						   WindowStatePerAgg peraggstate);
static void advance_windowaggregate(WindowAggState *winstate,
						WindowStatePerFunc perfuncstate,
						WindowStatePerAgg peraggstate);
static void finalize_windowaggregate(WindowAggState *winstate,
						 WindowStatePerFunc perfuncstate,
						 WindowStatePerAgg peraggstate,
						 Datum *result, bool *isnull);

static void eval_windowaggregates(WindowAggState *winstate);
static void eval_windowfunction(WindowAggState *winstate,
					WindowStatePerFunc perfuncstate,
					Datum *result, bool *isnull);

static void begin_partition(WindowAggState *winstate);
static void spool_tuples(WindowAggState *winstate, int64 pos);
static void release_partition(WindowAggState *winstate);

static bool row_is_in_frame(WindowAggState *winstate, int64 pos,
				TupleTableSlot *slot);
static void update_frameheadpos(WindowObject winobj, TupleTableSlot *slot);
static void update_frametailpos(WindowObject winobj, TupleTableSlot *slot);

static WindowStatePerAggData *initialize_peragg(WindowAggState *winstate,
				  WindowFunc *wfunc,
				  WindowStatePerAgg peraggstate);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);

static bool are_peers(WindowAggState *winstate, TupleTableSlot *slot1,
		  TupleTableSlot *slot2);
static bool window_gettupleslot(WindowObject winobj, int64 pos,
					TupleTableSlot *slot);

//#ifdef WIN_FUN_OPT
static AttrNumber addNumber(AttrNumber *array, AttrNumber num);
static bool opt_window_gettupleslot(WindowObject winobj, int64 pos, TupleTableSlot *slot, TupleTableSlot *opt_slot);
static void opt_update_frameheadpos(WindowObject winobj, TupleTableSlot *slot, TupleTableSlot *opt_slot);
static void opt_update_frametailpos(WindowObject winobj, TupleTableSlot *slot, TupleTableSlot *opt_slot);
static void opt_temp_advance_windowaggregate(WindowAggState *winstate,
						WindowStatePerFunc perfuncstate,
						WindowStatePerAgg peraggstate,
						int target);
static void opt_combine_tranValues(WindowAggState *winstate,
		WindowStatePerFunc perfuncstate,
		WindowStatePerAgg peraggstate);
static void opt_temp_initialize_windowaggregate(WindowAggState *winstate,
						   WindowStatePerFunc perfuncstate,
						   WindowStatePerAgg peraggstate,
						   int target);
static void opt_eval_tempTransEnd(WindowAggState *winstate);
static void opt_copy_transValue_from_tempTransValue(WindowAggState *winstate,
		   WindowStatePerFunc perfuncstate,
		   WindowStatePerAgg peraggstate,
		   int target);
static bool reuse_window_gettupleslot(WindowObject winobj, int64 pos, TupleTableSlot *slot);
static void reuse_update_frameheadpos(WindowObject winobj, TupleTableSlot *slot);
//#endif

/*
 * initialize_windowaggregate
 * parallel to initialize_aggregates in nodeAgg.c
 */
static void
initialize_windowaggregate(WindowAggState *winstate,
						   WindowStatePerFunc perfuncstate,
						   WindowStatePerAgg peraggstate)
{
	MemoryContext oldContext;

	if (peraggstate->initValueIsNull)
		peraggstate->transValue = peraggstate->initValue;
	else
	{
		oldContext = MemoryContextSwitchTo(winstate->aggcontext);
		peraggstate->transValue = datumCopy(peraggstate->initValue,
											peraggstate->transtypeByVal,
											peraggstate->transtypeLen);
		MemoryContextSwitchTo(oldContext);
	}
	peraggstate->transValueIsNull = peraggstate->initValueIsNull;
	peraggstate->noTransValue = peraggstate->initValueIsNull;
	peraggstate->resultValueIsNull = true;
}

/*
 * advance_windowaggregate
 * parallel to advance_aggregates in nodeAgg.c
 */
static void
advance_windowaggregate(WindowAggState *winstate,
						WindowStatePerFunc perfuncstate,
						WindowStatePerAgg peraggstate)
{
	WindowFuncExprState *wfuncstate = perfuncstate->wfuncstate;
	int			numArguments = perfuncstate->numArguments;
	FunctionCallInfoData fcinfodata;
	FunctionCallInfo fcinfo = &fcinfodata;
	Datum		newVal;
	ListCell   *arg;
	int			i;
	MemoryContext oldContext;
	ExprContext *econtext = winstate->tmpcontext;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* We start from 1, since the 0th arg will be the transition value */
	i = 1;
//#ifdef WIN_FUN_OPT
	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
		foreach(arg, wfuncstate->opt_args){
			ExprState  *argstate = (ExprState *) lfirst(arg);

			fcinfo->arg[i] = ExecEvalExpr(argstate, econtext,
										  &fcinfo->argnull[i], NULL);
			i++;
		}
	}else{
//#endif
		foreach(arg, wfuncstate->args)
		{
			ExprState  *argstate = (ExprState *) lfirst(arg);

			fcinfo->arg[i] = ExecEvalExpr(argstate, econtext,
										  &fcinfo->argnull[i], NULL);
			i++;
		}
//#ifdef WIN_FUN_OPT
	}
//#endif

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->argnull[i])
			{
				MemoryContextSwitchTo(oldContext);
				return;
			}
		}
		if (peraggstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into aggcontext if it is pass-by-ref. We
			 * do not need to pfree the old transValue, since it's NULL.
			 */
			MemoryContextSwitchTo(winstate->aggcontext);
			peraggstate->transValue = datumCopy(fcinfo->arg[1],
												peraggstate->transtypeByVal,
												peraggstate->transtypeLen);
			peraggstate->transValueIsNull = false;
			peraggstate->noTransValue = false;
			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (peraggstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			MemoryContextSwitchTo(oldContext);
			return;
		}
	}

	/*
	 * OK to call the transition function
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn),
							 numArguments + 1,
							 perfuncstate->winCollation,
							 (void *) winstate, NULL);
	fcinfo->arg[0] = peraggstate->transValue;
	fcinfo->argnull[0] = peraggstate->transValueIsNull;
	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.	But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(peraggstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(winstate->aggcontext);
			newVal = datumCopy(newVal,
							   peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		if (!peraggstate->transValueIsNull)
			pfree(DatumGetPointer(peraggstate->transValue));
	}

	MemoryContextSwitchTo(oldContext);
	peraggstate->transValue = newVal;
	peraggstate->transValueIsNull = fcinfo->isnull;
}

/*
 * finalize_windowaggregate
 * parallel to finalize_aggregate in nodeAgg.c
 */
static void
finalize_windowaggregate(WindowAggState *winstate,
						 WindowStatePerFunc perfuncstate,
						 WindowStatePerAgg peraggstate,
						 Datum *result, bool *isnull)
{
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		FunctionCallInfoData fcinfo;

		InitFunctionCallInfoData(fcinfo, &(peraggstate->finalfn), 1,
								 perfuncstate->winCollation,
								 (void *) winstate, NULL);
		fcinfo.arg[0] = peraggstate->transValue;
		fcinfo.argnull[0] = peraggstate->transValueIsNull;
		if (fcinfo.flinfo->fn_strict && peraggstate->transValueIsNull)
		{
			/* don't call a strict function with NULL inputs */
			*result = (Datum) 0;
			*isnull = true;
		}
		else
		{
			*result = FunctionCallInvoke(&fcinfo);
			*isnull = fcinfo.isnull;
		}
	}
	else
	{
		*result = peraggstate->transValue;
		*isnull = peraggstate->transValueIsNull;
	}

	/*
	 * If result is pass-by-ref, make sure it is in the right context.
	 */
	if (!peraggstate->resulttypeByVal && !*isnull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*result)))
		*result = datumCopy(*result,
							peraggstate->resulttypeByVal,
							peraggstate->resulttypeLen);
	MemoryContextSwitchTo(oldContext);
}

/*
 * eval_windowaggregates
 * evaluate plain aggregates being used as window functions
 *
 * Much of this is duplicated from nodeAgg.c.  But NOTE that we expect to be
 * able to call aggregate final functions repeatedly after aggregating more
 * data onto the same transition value.  This is not a behavior required by
 * nodeAgg.c.
 */
static void
eval_windowaggregates(WindowAggState *winstate)
{
	WindowStatePerAgg peraggstate;
	int			wfuncno,
				numaggs;
	int			i;
	MemoryContext oldContext;
	ExprContext *econtext;
	WindowObject agg_winobj;
	TupleTableSlot *agg_row_slot;

//#ifdef WIN_FUN_OPT
	TupleTableSlot	*opt_agg_row_slot = winstate->opt_agg_row_slot;

	/* the following is some flags for computing the time cost */
	bool			recompute_cpu_started = false;	/* a flag for instr_recompute_part */
	bool			recompute_part_stoped = true;
	bool			framehead_updated = false;
	bool			stillinframe = true;
	bool			cpu_afterspooled_started = false;
	int64			pre_upto = winstate->aggregatedupto;
	bool			io_afterspooled_started = false;

	/* the following is for reducing recompute */
	bool			need_compute_temp_trans = false;
	int64 			previous_frame_size = 0;
	//bool			need_combine = false;
//#endif

	numaggs = winstate->numaggs;
	if (numaggs == 0)
		return;					/* nothing to do */

	/* final output execution is in ps_ExprContext */
	econtext = winstate->ss.ps.ps_ExprContext;
	agg_winobj = winstate->agg_winobj;
	agg_row_slot = winstate->agg_row_slot;

	/*
	 * Currently, we support only a subset of the SQL-standard window framing
	 * rules.
	 *
	 * If the frame start is UNBOUNDED_PRECEDING, the window frame consists of
	 * a contiguous group of rows extending forward from the start of the
	 * partition, and rows only enter the frame, never exit it, as the current
	 * row advances forward.  This makes it possible to use an incremental
	 * strategy for evaluating aggregates: we run the transition function for
	 * each row added to the frame, and run the final function whenever we
	 * need the current aggregate value.  This is considerably more efficient
	 * than the naive approach of re-running the entire aggregate calculation
	 * for each current row.  It does assume that the final function doesn't
	 * damage the running transition value, but we have the same assumption in
	 * nodeAgg.c too (when it rescans an existing hash table).
	 *
	 * For other frame start rules, we discard the aggregate state and re-run
	 * the aggregates whenever the frame head row moves.  We can still
	 * optimize as above whenever successive rows share the same frame head.
	 *
	 * In many common cases, multiple rows share the same frame and hence the
	 * same aggregate value. (In particular, if there's no ORDER BY in a RANGE
	 * window, then all rows are peers and so they all have window frame equal
	 * to the whole partition.)  We optimize such cases by calculating the
	 * aggregate value once when we reach the first row of a peer group, and
	 * then returning the saved value for all subsequent rows.
	 *
	 * 'aggregatedupto' keeps track of the first row that has not yet been
	 * accumulated into the aggregate transition values.  Whenever we start a
	 * new peer group, we accumulate forward to the end of the peer group.
	 *
	 * TODO: Rerunning aggregates from the frame start can be pretty slow. For
	 * some aggregates like SUM and COUNT we could avoid that by implementing
	 * a "negative transition function" that would be called for each row as
	 * it exits the frame.	We'd have to think about avoiding recalculation of
	 * volatile arguments of aggregate functions, too.
	 */


	/*
	 * First, update the frame head position.
	 */
//#ifdef WIN_FUN_OPT
	if(enable_winfunopt)
		opt_update_frameheadpos(agg_winobj, winstate->temp_slot_1, winstate->opt_temp_slot_1);
//#else
	else if(enable_reusebuffer)
		reuse_update_frameheadpos(agg_winobj, winstate->temp_slot_1);
	else
		update_frameheadpos(agg_winobj, winstate->temp_slot_1);
//#endif


	/*
	 * Initialize aggregates on first call for partition, or if the frame head
	 * position moved since last time.
	 */
	if (winstate->currentpos == 0 ||
		winstate->frameheadpos != winstate->aggregatedbase)
	{


		/*
		 * Discard transient aggregate values
		 */
		/*
		 * moved by cywang
		 *
		 * because this discard process will discard the temporary transition value too.
		 */
		//MemoryContextResetAndDeleteChildren(winstate->aggcontext);

		/*
		 * by cywang, moved backward
		 */
		/*
		for (i = 0; i < numaggs; i++)
		{
			peraggstate = &winstate->peragg[i];
			wfuncno = peraggstate->wfuncno;
			initialize_windowaggregate(winstate,
									   &winstate->perfunc[wfuncno],
									   peraggstate);
		}
		*/

		/*
		 * If we created a mark pointer for aggregates, keep it pushed up to
		 * frame head, so that tuplestore can discard unnecessary rows.
		 */
		if (agg_winobj->markptr >= 0)
			WinSetMarkPosition(agg_winobj, winstate->frameheadpos);

		/* by cywang, for locate*/
		if(enable_locate){
			if(agg_winobj->opt_frameheadptr >= 0 && winstate->currentpos!=0){
				/* first update opt_frameheadptr */
				opt_update_frameheadptr(agg_winobj, winstate->frameheadpos);
				/* then copy opt_frameheadptr to readptr */
				opt_copy_frameheadptr(agg_winobj);
			}

			if(enable_reusebuffer && !tuplestore_in_memory(winstate->buffer)){
				copy_reuseptr(agg_winobj);
			}
		}

		if(enable_reusebuffer){
			reuse_tuplestore_trim(winstate->buffer, winstate->frameheadpos);
		}
//#ifdef WIN_FUN_OPT
		/*
		 * Initialize for loop below
		 */
		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			ExecClearTuple(opt_agg_row_slot);
		}else{
//#endif
			ExecClearTuple(agg_row_slot);
//#ifdef WIN_FUN_OPT
		}
//#endif

		/* by cywang */
		framehead_updated = true;

		if(winstate->currentpos == 0){
			for(i=0; i<winstate->opt_tempTransValue_num; i++)
				agg_winobj->opt_tempStartPos[i] = 0;
		}

		previous_frame_size = winstate->aggregatedupto - winstate->aggregatedbase +1;

		/*
		 * In the last step when need_compute_temp_trans is true, no temporary transition value is produced,
		 * which means recompute_k is bigger than the frame size
		 */
		//if(agg_winobj->opt_needTempTransValue && agg_winobj->opt_frameheadeverchanged && agg_winobj->opt_tempTransEndPos==-1)
		//	agg_winobj->opt_needTempTransValue = false;

		//if(enable_recompute && agg_winobj->opt_needTempTransValue){
		if(enable_recompute && previous_frame_size>4){
			winstate->opt_active_tempTransValue = -1;
			for(i=0; i<winstate->opt_tempTransValue_num; i++){
				if(winstate->frameheadpos <= agg_winobj->opt_tempStartPos[i] && agg_winobj->opt_tempTransEndPos[i]>=0){
					winstate->opt_active_tempTransValue = i;
					break;
				}
			}

			/* no temporary transition value is proper */
			if(winstate->opt_active_tempTransValue  < 0){
				int target;
				MemoryContextResetAndDeleteChildren(winstate->aggcontext);
				for(target=0; target<winstate->opt_tempTransValue_num; target++){
					for (i = 0; i < numaggs; i++)
					{
						peraggstate = &winstate->peragg[i];
						wfuncno = peraggstate->wfuncno;
						/* the pointer of temporary transition value is automatically freed */
						opt_temp_initialize_windowaggregate(winstate,
												   &winstate->perfunc[wfuncno],
												   peraggstate,
												   target);
						/*
						 * We don't use a fixed size for the tempTransValue,
						 * because the frame size may changes.
						 *
						 * However, we have an assumption that the size of two adjacent frames doesn't vary much.
						 * So currently, the size of tempTransValue comes from the previous frame.
						 *
						 * In addition, only when frame size is bigger than 4, we import recompute.
						 * Which means the size of the tempTransValue is bigger than 2.
						 */
						if(winstate->currentpos != 0)
							agg_winobj->opt_tempStartPos[target] = winstate->frameheadpos + (target+1)*(int64)sqrt(previous_frame_size);
						else
							agg_winobj->opt_tempStartPos[target] = winstate->frameheadpos + (target+1)*recompute_k;

						agg_winobj->opt_tempTransEndPos[target] = -1;
					}
				}
				need_compute_temp_trans = true;

				for (i = 0; i < numaggs; i++)
				{
					peraggstate = &winstate->peragg[i];
					wfuncno = peraggstate->wfuncno;
					initialize_windowaggregate(winstate,
											   &winstate->perfunc[wfuncno],
											   peraggstate);
				}
			}else{
				/* a temporary transition value can still be used */
				for (i = 0; i < numaggs; i++)
				{
					peraggstate = &winstate->peragg[i];
					wfuncno = peraggstate->wfuncno;
					opt_copy_transValue_from_tempTransValue(winstate,
												&winstate->perfunc[wfuncno],
												peraggstate,
												winstate->opt_active_tempTransValue);
				}
			}
		}else{
			MemoryContextResetAndDeleteChildren(winstate->aggcontext);
			for (i = 0; i < numaggs; i++)
			{
				peraggstate = &winstate->peragg[i];
				wfuncno = peraggstate->wfuncno;
				initialize_windowaggregate(winstate,
										   &winstate->perfunc[wfuncno],
										   peraggstate);
			}
		}
		//if(winstate->currentpos != 0)
			//agg_winobj->opt_frameheadeverchanged = true;

		winstate->aggregatedbase = winstate->frameheadpos;
		winstate->aggregatedupto = winstate->frameheadpos;
	}

	/*
	 * In UNBOUNDED_FOLLOWING mode, we don't have to recalculate aggregates
	 * except when the frame head moves.  In END_CURRENT_ROW mode, we only
	 * have to recalculate when the frame head moves or currentpos has
	 * advanced past the place we'd aggregated up to.  Check for these cases
	 * and if so, reuse the saved result values.
	 */
	if ((winstate->frameOptions & (FRAMEOPTION_END_UNBOUNDED_FOLLOWING |
								   FRAMEOPTION_END_CURRENT_ROW)) &&
		winstate->aggregatedbase <= winstate->currentpos &&
		winstate->aggregatedupto > winstate->currentpos)
	{
		for (i = 0; i < numaggs; i++)
		{
			peraggstate = &winstate->peragg[i];
			wfuncno = peraggstate->wfuncno;
			econtext->ecxt_aggvalues[wfuncno] = peraggstate->resultValue;
			econtext->ecxt_aggnulls[wfuncno] = peraggstate->resultValueIsNull;
		}
		return;
	}

	/*
	 * by cywang
	 *
	 * only when the frame head is changed and
	 */
	if(framehead_updated && pre_upto > winstate->aggregatedupto){
		//recompute_started = true;
		recompute_part_stoped = false;
		InstrStartNode(winstate->instr_recompute_part);
	}


	/*
	 * Advance until we reach a row not in frame (or end of partition).
	 *
	 * Note the loop invariant: agg_row_slot is either empty or holds the row
	 * at position aggregatedupto.	We advance aggregatedupto after processing
	 * a row.
	 */
	for (;;)
	{
		if(!recompute_part_stoped && winstate->aggregatedupto>=pre_upto){
			//recompute_started = false;
			recompute_part_stoped = true;
			InstrStopNode(winstate->instr_recompute_part, 0);
		}

		if(framehead_updated){
			opt_tuplestore_set_locateheadpos(winstate->buffer, true);	/* for locate IO */
			InstrStartNode(winstate->instr_locateheadpos_part);	/* for locate the head position */
		}

		if(winstate->partition_spooled && !framehead_updated){
			InstrStartNode(winstate->instr_io_afterspooled);
			io_afterspooled_started = true;
		}

//#ifdef WIN_FUN_OPT
		if(enable_winfunopt){
			/*
			 * opt_window_gettupleslot may change the status
			 */
			if(!tuplestore_in_memory(winstate->buffer)){
				if (TupIsNull(opt_agg_row_slot)){
					if (!opt_window_gettupleslot(agg_winobj, winstate->aggregatedupto, agg_row_slot, opt_agg_row_slot))
						break;			/* must be end of partition */
				}
			}else{
				if (TupIsNull(agg_row_slot))
				{
					if (!opt_window_gettupleslot(agg_winobj, winstate->aggregatedupto, agg_row_slot, opt_agg_row_slot))
						break;			/* must be end of partition */
				}
				/*
				 * this is the situation that the buffer status switch from memory to tape,
				 * as the follow-up computing will for opt, we need to converted it to an opt tuple.
				 */
				if(!tuplestore_in_memory(winstate->buffer)){
					//tuplestore_convert_to_opt(winstate->buffer, agg_row_slot, opt_agg_row_slot);
					/* the initial tuple slot is will not be used in the computation of current row */
					if(agg_row_slot)
						ExecClearTuple(agg_row_slot);
				}
			}
		}else{
//#else
			/* Fetch next row if we didn't already */
			if (TupIsNull(agg_row_slot))
			{
				if(enable_reusebuffer){
					if (!reuse_window_gettupleslot(agg_winobj, winstate->aggregatedupto,
											 agg_row_slot))
						break;			/* must be end of partition */
				}else{
					if (!window_gettupleslot(agg_winobj, winstate->aggregatedupto,
											 agg_row_slot))
						break;			/* must be end of partition */
				}
			}
		}
//#endif

		if(io_afterspooled_started){
			InstrStopNode(winstate->instr_io_afterspooled, 0);	/* must before framehead_updated is changed */
			io_afterspooled_started = false;
		}

		if(framehead_updated){
			framehead_updated = false;
			opt_tuplestore_set_locateheadpos(winstate->buffer, false);
			InstrStopNode(winstate->instr_locateheadpos_part, 0);
		}

		if(winstate->partition_spooled){
			InstrStartNode(winstate->instr_cpu_afterspooled);
			cpu_afterspooled_started = true;
		}

		/* Exit loop (for now) if not in frame */
		//InstrStartNode(winstate->instr_checkinframe_part);
//#ifdef WIN_FUN_OPT
		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			if (!row_is_in_frame(winstate, winstate->aggregatedupto, opt_agg_row_slot)){
				stillinframe = false;	/* for instr_checkinframe_part*/
				break;
			}
			/* Set tuple context for evaluation of aggregate arguments */
			winstate->tmpcontext->ecxt_outertuple = opt_agg_row_slot;
		}else{
//#endif
			if (!row_is_in_frame(winstate, winstate->aggregatedupto, agg_row_slot)){
				stillinframe = false;
				break;
			}
			/* Set tuple context for evaluation of aggregate arguments */
			winstate->tmpcontext->ecxt_outertuple = agg_row_slot;
//#ifdef WIN_FUN_OPT
		}
//#endif
		//InstrStopNode(winstate->instr_checkinframe_part,0);

		if(!recompute_part_stoped){
			recompute_cpu_started = true;
			InstrStartNode(winstate->instr_recompute_cpu);
		}

		/*
		 * For single temporary transition value.
		 */
		if(enable_recompute && previous_frame_size>4 && winstate->frameheadpos != 0){
			if(winstate->opt_active_tempTransValue>=0
					&& winstate->aggregatedupto < agg_winobj->opt_tempStartPos[winstate->opt_active_tempTransValue])
			{
				/* Accumulate row into the aggregates */
				for (i = 0; i < numaggs; i++)
				{
					peraggstate = &winstate->peragg[i];
					wfuncno = peraggstate->wfuncno;
					advance_windowaggregate(winstate,
											&winstate->perfunc[wfuncno],
											peraggstate);
				}
			}else if(winstate->opt_active_tempTransValue<0){
				int target;
				/* for currentpos's use */
				for (i = 0; i < numaggs; i++)
				{
					peraggstate = &winstate->peragg[i];
					wfuncno = peraggstate->wfuncno;
					advance_windowaggregate(winstate,
											&winstate->perfunc[wfuncno],
											peraggstate);
				}
				/* for future use */
				for(target=0; target<winstate->opt_tempTransValue_num; target++){
					if(winstate->aggregatedupto >= agg_winobj->opt_tempStartPos[target]){
						for (i = 0; i < numaggs; i++)
						{
							peraggstate = &winstate->peragg[i];
							wfuncno = peraggstate->wfuncno;
							opt_temp_advance_windowaggregate(winstate,
													&winstate->perfunc[wfuncno],
													peraggstate,
													target);
						}
						/*
						 * save opt_tempTransEndPtr
						 *
						 * we can't do this out this loop,
						 * because at that time the position is not the end position for temporary transition value.
						 */
						opt_tuplestore_copy_ptr(winstate->buffer, agg_winobj->opt_tempTransEndPtr[target], agg_winobj->readptr);
						agg_winobj->opt_tempTransEndPos[target] = agg_winobj->seekpos; /* we must copy the seek position */
						/*
						 * if not in memory, we need to set the bufFile's offset to opt_tempTransEndPtr.
						 *
						 * NOTE: not just for on tape, but also for memory, or the answer will contains error
						 */
						opt_tuplestore_tell_ptr(winstate->buffer, agg_winobj->opt_tempTransEndPtr[target]);
					}else{
						break;	/* opt_tempStartPos[i+1] > opt_tempStartPos[i] */
					}
				}
			}else{
				ResetExprContext(winstate->tmpcontext);
				if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
					ExecClearTuple(opt_agg_row_slot);
				}else{
					ExecClearTuple(agg_row_slot);
				}
				break;
			}
		}else{
			for (i = 0; i < numaggs; i++)
			{
				peraggstate = &winstate->peragg[i];
				wfuncno = peraggstate->wfuncno;
				advance_windowaggregate(winstate,
										&winstate->perfunc[wfuncno],
										peraggstate);
			}
		}

		/* Reset per-input-tuple context after each tuple */
		ResetExprContext(winstate->tmpcontext);

		/* And advance the aggregated-row state */
		winstate->aggregatedupto++;

//#ifdef WIN_FUN_OPT
		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			ExecClearTuple(opt_agg_row_slot);
		}else{
//#endif
			ExecClearTuple(agg_row_slot);
//#ifdef WIN_FUN_OPT
		}
//#endif
		if(recompute_cpu_started){
			InstrStopNode(winstate->instr_recompute_cpu, 0);
			recompute_cpu_started = false;
		}

		if(cpu_afterspooled_started){
			InstrStopNode(winstate->instr_cpu_afterspooled, 0);
			cpu_afterspooled_started = false;
		}
	}

	//if(enable_recompute && agg_winobj->opt_needTempTransValue){
	if(enable_recompute && previous_frame_size>4){
		/*
		 * NOTE:
		 * if winstate->opt_active_tempTransValue<0, means aggregatedupto already up to date.
		 * if opt_tempTransEndPos==-1, means no temporary transition value to use.
		 */
		if(winstate->opt_active_tempTransValue>=0
				&& winstate->agg_winobj->opt_tempTransEndPos[winstate->opt_active_tempTransValue] != -1)
		{
			/* copy the readptr from opt_tempTransEndPtr */
			opt_copy_tempTransEndPtr(agg_winobj, winstate->opt_active_tempTransValue);
			/*
			 * After combine the transition values, we need to set the aggregatedupto value.
			 *
			 * NOTE: here must +1, aggregatedupto means the position of the next tuple to aggregate,
			 * while opt_tempTransEndPos comes from seekpos, that is the position of the last tuple.
			 */
			//winstate->aggregatedupto = agg_winobj->opt_tempTransEndPos;
			winstate->aggregatedupto = agg_winobj->opt_tempTransEndPos[winstate->opt_active_tempTransValue]+1;
			/* Compute the tuples after opt_tempTransEndPos */
			opt_eval_tempTransEnd(winstate);
		}
	}

	/*
	 * by cywang
	 *
	 * the instruments may no stop in the upper for loop,
	 * need to check here.
	 */
	if(io_afterspooled_started){
		InstrStopNode(winstate->instr_io_afterspooled, 0);
		io_afterspooled_started = false;
	}
	if(framehead_updated){
		framehead_updated = false;
		opt_tuplestore_set_locateheadpos(winstate->buffer, false);
		InstrStopNode(winstate->instr_locateheadpos_part, 0);
	}
	if(!recompute_part_stoped){
		recompute_part_stoped = true;
		InstrStopNode(winstate->instr_recompute_part, 0);
	}
	if(!stillinframe){
		//InstrStopNode(winstate->instr_checkinframe_part, 0);
	}
	if(cpu_afterspooled_started){
		InstrStopNode(winstate->instr_cpu_afterspooled, 0);
		cpu_afterspooled_started = false;
	}

	/*
	 * finalize aggregates and fill result/isnull fields.
	 */
	for (i = 0; i < numaggs; i++)
	{
		Datum	   *result;
		bool	   *isnull;

		peraggstate = &winstate->peragg[i];
		wfuncno = peraggstate->wfuncno;
		result = &econtext->ecxt_aggvalues[wfuncno];
		isnull = &econtext->ecxt_aggnulls[wfuncno];
		finalize_windowaggregate(winstate,
								 &winstate->perfunc[wfuncno],
								 peraggstate,
								 result, isnull);

		/*
		 * save the result in case next row shares the same frame.
		 *
		 * XXX in some framing modes, eg ROWS/END_CURRENT_ROW, we can know in
		 * advance that the next row can't possibly share the same frame. Is
		 * it worth detecting that and skipping this code?
		 */
		if (!peraggstate->resulttypeByVal)
		{
			/*
			 * clear old resultValue in order not to leak memory.  (Note: the
			 * new result can't possibly be the same datum as old resultValue,
			 * because we never passed it to the trans function.)
			 */
			if (!peraggstate->resultValueIsNull)
				pfree(DatumGetPointer(peraggstate->resultValue));

			/*
			 * If pass-by-ref, copy it into our aggregate context.
			 */
			if (!*isnull)
			{
				oldContext = MemoryContextSwitchTo(winstate->aggcontext);
				peraggstate->resultValue =
					datumCopy(*result,
							  peraggstate->resulttypeByVal,
							  peraggstate->resulttypeLen);
				MemoryContextSwitchTo(oldContext);
			}
		}
		else
		{
			peraggstate->resultValue = *result;
		}
		peraggstate->resultValueIsNull = *isnull;
	}

}

/*
 * eval_windowfunction
 *
 * Arguments of window functions are not evaluated here, because a window
 * function can need random access to arbitrary rows in the partition.
 * The window function uses the special WinGetFuncArgInPartition and
 * WinGetFuncArgInFrame functions to evaluate the arguments for the rows
 * it wants.
 */
static void
eval_windowfunction(WindowAggState *winstate, WindowStatePerFunc perfuncstate,
					Datum *result, bool *isnull)
{
	FunctionCallInfoData fcinfo;
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * We don't pass any normal arguments to a window function, but we do pass
	 * it the number of arguments, in order to permit window function
	 * implementations to support varying numbers of arguments.  The real info
	 * goes through the WindowObject, which is passed via fcinfo->context.
	 */
	InitFunctionCallInfoData(fcinfo, &(perfuncstate->flinfo),
							 perfuncstate->numArguments,
							 perfuncstate->winCollation,
							 (void *) perfuncstate->winobj, NULL);
	/* Just in case, make all the regular argument slots be null */
	memset(fcinfo.argnull, true, perfuncstate->numArguments);

	*result = FunctionCallInvoke(&fcinfo);
	*isnull = fcinfo.isnull;

	/*
	 * Make sure pass-by-ref data is allocated in the appropriate context. (We
	 * need this in case the function returns a pointer into some short-lived
	 * tuple, as is entirely possible.)
	 */
	if (!perfuncstate->resulttypeByVal && !fcinfo.isnull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*result)))
		*result = datumCopy(*result,
							perfuncstate->resulttypeByVal,
							perfuncstate->resulttypeLen);

	MemoryContextSwitchTo(oldContext);
}

/*
 * begin_partition
 * Start buffering rows of the next partition.
 */
static void
begin_partition(WindowAggState *winstate)
{
	PlanState  *outerPlan = outerPlanState(winstate);
	int			numfuncs = winstate->numfuncs;
	int			i;


	winstate->partition_spooled = false;
	winstate->framehead_valid = false;
	winstate->frametail_valid = false;
	winstate->spooled_rows = 0;
	winstate->currentpos = 0;
	winstate->frameheadpos = 0;
	winstate->frametailpos = -1;
	ExecClearTuple(winstate->agg_row_slot);

	/*
	 * If this is the very first partition, we need to fetch the first input
	 * row to store in first_part_slot.
	 */
	if (TupIsNull(winstate->first_part_slot))
	{
		TupleTableSlot *outerslot;

		InstrStartNode(winstate->instr_total_part);

		outerslot = ExecProcNode(outerPlan);

		if (!TupIsNull(outerslot))
			ExecCopySlot(winstate->first_part_slot, outerslot);
		else
		{
			/* outer plan is empty, so we have nothing to do */
			winstate->partition_spooled = true;
			winstate->more_partitions = false;
			return;
		}

		InstrStopNode(winstate->instr_total_part, 0);
		printf("sort: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_total_part->counter));
		INSTR_TIME_SET_ZERO(winstate->instr_total_part->counter);
	}

	printf("\n#####begin partition:\n");
	InstrStartNode(winstate->instr_total_part);

	/* Create new tuplestore for this partition */
	winstate->buffer = tuplestore_begin_heap(false, false, work_mem);

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		/*
		 * set the attribute of the buffer that its functions can only access in its own scope
		 */
		//tuplestore_set_opt(winstate->buffer, winstate);
		tuplestore_init_opt(winstate->buffer, winstate->opt_tupdesc, winstate->useful2init, winstate->opt_maxPos, winstate->buffer_slot);

		tuplestore_convert_to_opt(winstate->buffer, winstate->first_part_slot, winstate->opt_first_part_slot);
	}
//#endif
	/*
	 * Set up read pointers for the tuplestore.  The current pointer doesn't
	 * need BACKWARD capability, but the per-window-function read pointers do,
	 * and the aggregate pointer does if frame start is movable.
	 */
	winstate->current_ptr = 0;	/* read pointer 0 is pre-allocated */

	/* reset default REWIND capability bit for current ptr */
	tuplestore_set_eflags(winstate->buffer, 0);

	/* create read pointers for aggregates, if needed */
	if (winstate->numaggs > 0)
	{
		WindowObject agg_winobj = winstate->agg_winobj;
		int			readptr_flags = 0;

		/* If the frame head is potentially movable ... */
		if (!(winstate->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING))
		{
			/* ... create a mark pointer to track the frame head */
			agg_winobj->markptr = tuplestore_alloc_read_pointer(winstate->buffer, 0);
			/* and the read pointer will need BACKWARD capability */
			readptr_flags |= EXEC_FLAG_BACKWARD;
		}

		agg_winobj->readptr = tuplestore_alloc_read_pointer(winstate->buffer,
															readptr_flags);
		agg_winobj->markpos = -1;
		agg_winobj->seekpos = -1;

		/* Also reset the row counters for aggregates */
		winstate->aggregatedbase = 0;
		winstate->aggregatedupto = 0;

		/*
		 * by cywang
		 * the frame head read pointer is pro-allocated
		 *
		 * NOTE: currently, only one for aggregates,
		 * if window function needs, we can allocate one for each window object
		 */
		if(enable_locate){
			agg_winobj->opt_frameheadptr = tuplestore_alloc_read_pointer(winstate->buffer, 0);	/* only need forward */
			agg_winobj->opt_frameheadpos = -1;
		}
		/*
		 * by cywang
		 *
		 * for temporary transition value's end position
		 */
		if(enable_recompute){
			for(i=0; i<winstate->opt_tempTransValue_num; i++){
				agg_winobj->opt_tempTransEndPtr[i] = tuplestore_alloc_read_pointer(winstate->buffer, 0);
				agg_winobj->opt_tempTransEndPos[i] = -1;
			}
		}
	}

	/* create mark and read pointers for each real window function */
	for (i = 0; i < numfuncs; i++)
	{
		WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

		if (!perfuncstate->plain_agg)
		{
			WindowObject winobj = perfuncstate->winobj;

			winobj->markptr = tuplestore_alloc_read_pointer(winstate->buffer,
															0);
			winobj->readptr = tuplestore_alloc_read_pointer(winstate->buffer,
														 EXEC_FLAG_BACKWARD);
			winobj->markpos = -1;
			winobj->seekpos = -1;
		}
	}

	/*
	 * Store the first tuple into the tuplestore (it's always available now;
	 * we either read it above, or saved it at the end of previous partition)
	 */
//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		opt_tuplestore_puttupleslot(winstate->buffer, winstate->first_part_slot);
		/* just in case */
		if(!tuplestore_in_memory(winstate->buffer))
			opt_tuplestore_updatewritepos(winstate->buffer);
	}else{
//#else
		tuplestore_puttupleslot(winstate->buffer, winstate->first_part_slot);
	}
//#endif

	winstate->spooled_rows++;
}

/*
 * Read tuples from the outer node, up to and including position 'pos', and
 * store them into the tuplestore. If pos is -1, reads the whole partition.
 */
static void
spool_tuples(WindowAggState *winstate, int64 pos)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	PlanState  *outerPlan;
	TupleTableSlot *outerslot;
	MemoryContext oldcontext;

	if (!winstate->buffer)
		return;					/* just a safety check */
	if (winstate->partition_spooled)
		return;					/* whole partition done already */

	/*
	 * If the tuplestore has spilled to disk, alternate reading and writing
	 * becomes quite expensive due to frequent buffer flushes.	It's cheaper
	 * to force the entire partition to get spooled in one go.
	 *
	 * XXX this is a horrid kluge --- it'd be better to fix the performance
	 * problem inside tuplestore.  FIXME
	 */
	if (!tuplestore_in_memory(winstate->buffer))
		pos = -1;

	outerPlan = outerPlanState(winstate);

	/* Must be in query context to call outerplan */
	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	while (winstate->spooled_rows <= pos || pos == -1)
	{
		outerslot = ExecProcNode(outerPlan);
		if (TupIsNull(outerslot))
		{
			/* reached the end of the last partition */
			winstate->partition_spooled = true;
			winstate->more_partitions = false;
			break;
		}

		if (node->partNumCols > 0)
		{
			/* Check if this tuple still belongs to the current partition */
			if (!execTuplesMatch(winstate->first_part_slot,
								 outerslot,
								 node->partNumCols, node->partColIdx,
								 winstate->partEqfunctions,
								 winstate->tmpcontext->ecxt_per_tuple_memory))
			{
				/*
				 * end of partition; copy the tuple for the next cycle.
				 */
				ExecCopySlot(winstate->first_part_slot, outerslot);
				winstate->partition_spooled = true;
				winstate->more_partitions = true;
				break;
			}
		}

		/* Still in partition, so save it into the tuplestore */
//#ifdef WIN_FUN_OPT
		if(enable_winfunopt)
			opt_tuplestore_puttupleslot(winstate->buffer, outerslot);
//#else
		else
			tuplestore_puttupleslot(winstate->buffer, outerslot);
//#endif
		winstate->spooled_rows++;
	}

	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
		opt_tuplestore_updatewritepos(winstate->buffer);
	}
	MemoryContextSwitchTo(oldcontext);
}

/*
 * release_partition
 * clear information kept within a partition, including
 * tuplestore and aggregate results.
 */
static void
release_partition(WindowAggState *winstate)
{
	int			i;

	for (i = 0; i < winstate->numfuncs; i++)
	{
		WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

		/* Release any partition-local state of this window function */
		if (perfuncstate->winobj)
			perfuncstate->winobj->localmem = NULL;
	}

	/*
	 * Release all partition-local memory (in particular, any partition-local
	 * state that we might have trashed our pointers to in the above loop, and
	 * any aggregate temp data).  We don't rely on retail pfree because some
	 * aggregates might have allocated data we don't have direct pointers to.
	 */
	MemoryContextResetAndDeleteChildren(winstate->partcontext);
	MemoryContextResetAndDeleteChildren(winstate->aggcontext);

	if (winstate->buffer){
		/* by cywang */
		opt_tuplestore_instr_BufFile(winstate->buffer);
		opt_tuplestore_instr_TupleStore(winstate->buffer);

		if(enable_reusebuffer)
			reuse_tuplestore_clear(winstate->buffer);

//#ifdef WIN_FUN_OPT
		if(enable_winfunopt)
			opt_tuplestore_end(winstate->buffer);
//#else
		else
			tuplestore_end(winstate->buffer);
//#endif
	}

	/*
	 * output the time cost, and reset the time's counter
	 */
	//printf("WindowAgg, checking in frame: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_checkinframe_part->counter));
	//INSTR_TIME_SET_ZERO(winstate->instr_checkinframe_part->counter);

	printf("WindowAgg, locating the head position: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_locateheadpos_part->counter));
	INSTR_TIME_SET_ZERO(winstate->instr_locateheadpos_part->counter);

	printf("WindowAgg, CPU cost of computing after spooled: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_cpu_afterspooled->counter));
	INSTR_TIME_SET_ZERO(winstate->instr_cpu_afterspooled->counter);

	printf("WindowAgg, IO cost of computing after spooled: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_io_afterspooled->counter));
	INSTR_TIME_SET_ZERO(winstate->instr_io_afterspooled->counter);

	printf("WindowAgg, recomputing during a partition: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_recompute_part->counter));
	INSTR_TIME_SET_ZERO(winstate->instr_recompute_part->counter);

	//printf("WindowAgg, setting mark position: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_setmarkpos_part->counter));
	//INSTR_TIME_SET_ZERO(winstate->instr_setmarkpos_part->counter);

	printf("WindowAgg, CPU cost of recomputing: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_recompute_cpu->counter));
	INSTR_TIME_SET_ZERO(winstate->instr_recompute_cpu->counter);

	InstrStopNode(winstate->instr_total_part, 0);
	printf("WindowAgg, execution time of partition: %f seconds \n", INSTR_TIME_GET_DOUBLE(winstate->instr_total_part->counter));
	INSTR_TIME_SET_ZERO(winstate->instr_total_part->counter);

	printf("#####release partition\n");

	winstate->buffer = NULL;
	winstate->partition_spooled = false;
}

/*
 * row_is_in_frame
		TupleTableSlot *outerslot = ExecProcNode(outerPlan);
 *
 * Determine whether a row is in the current row's window frame according
 * to our window framing rule
 *
 * The caller must have already determined that the row is in the partition
 * and fetched it into a slot.	This function just encapsulates the framing
 * rules.
 */
static bool
row_is_in_frame(WindowAggState *winstate, int64 pos, TupleTableSlot *slot)
{
	int			frameOptions = winstate->frameOptions;

	Assert(pos >= 0);			/* else caller error */

	/* First, check frame starting conditions */
	if (frameOptions & FRAMEOPTION_START_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* rows before current row are out of frame */
			if (pos < winstate->currentpos)
				return false;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* preceding row that is not peer is out of frame */
//#ifdef WIN_FUN_OPT
			if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
				if (pos < winstate->currentpos &&
					!are_peers(winstate, slot, winstate->opt_scantupslot))
					return false;
			}else{
//#endif
				if (pos < winstate->currentpos &&
					!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
					return false;
//#ifdef WIN_FUN_OPT
			}
//#endif
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_START_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			int64		offset = DatumGetInt64(winstate->startOffsetValue);

			/* rows before current row + offset are out of frame */
			if (frameOptions & FRAMEOPTION_START_VALUE_PRECEDING)
				offset = -offset;

			if (pos < winstate->currentpos + offset)
				return false;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}

	/* Okay so far, now check frame ending conditions */
	if (frameOptions & FRAMEOPTION_END_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* rows after current row are out of frame */
			if (pos > winstate->currentpos)
				return false;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* following row that is not peer is out of frame */
//#ifdef WIN_FUN_OPT
			if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
				if (pos > winstate->currentpos &&
					!are_peers(winstate, slot, winstate->opt_scantupslot))
					return false;
			}else{
//#endif
				if (pos > winstate->currentpos &&
					!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
					return false;
//#ifdef WIN_FUN_OPT
			}
//#endif
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_END_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			int64		offset = DatumGetInt64(winstate->endOffsetValue);

			/* rows after current row + offset are out of frame */
			if (frameOptions & FRAMEOPTION_END_VALUE_PRECEDING)
				offset = -offset;

			if (pos > winstate->currentpos + offset)
				return false;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}

	/* If we get here, it's in frame */
	return true;
}

/*
 * update_frameheadpos
 * make frameheadpos valid for the current row
 *
 * Uses the winobj's read pointer for any required fetches; hence, if the
 * frame mode is one that requires row comparisons, the winobj's mark must
 * not be past the currently known frame head.	Also uses the specified slot
 * for any required fetches.
 */
static void
update_frameheadpos(WindowObject winobj, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;

	if (winstate->framehead_valid)
		return;					/* already known for current row */

	if (frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
	{
		/* In UNBOUNDED PRECEDING mode, frame head is always row 0 */
		winstate->frameheadpos = 0;
		winstate->framehead_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_START_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, frame head is the same as current */
			winstate->frameheadpos = winstate->currentpos;
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			int64		fhprev;

			/* If no ORDER BY, all rows are peers with each other */
			if (node->ordNumCols == 0)
			{
				winstate->frameheadpos = 0;
				winstate->framehead_valid = true;
				return;
			}

			/*
			 * In RANGE START_CURRENT mode, frame head is the first row that
			 * is a peer of current row.  We search backwards from current,
			 * which could be a bit inefficient if peer sets are large. Might
			 * be better to have a separate read pointer that moves forward
			 * tracking the frame head.
			 */
			fhprev = winstate->currentpos - 1;
			for (;;)
			{
				/* assume the frame head can't go backwards */
				if (fhprev < winstate->frameheadpos)
					break;
				if (!window_gettupleslot(winobj, fhprev, slot))
					break;		/* start of partition */
				if (!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
					break;		/* not peer of current row */
				fhprev--;
			}
			winstate->frameheadpos = fhprev + 1;
			winstate->framehead_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_START_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, bound is physically n before/after current */
			int64		offset = DatumGetInt64(winstate->startOffsetValue);

			if (frameOptions & FRAMEOPTION_START_VALUE_PRECEDING)
				offset = -offset;

			winstate->frameheadpos = winstate->currentpos + offset;
			/* frame head can't go before first row */
			if (winstate->frameheadpos < 0)
				winstate->frameheadpos = 0;
			else if (winstate->frameheadpos > winstate->currentpos)
			{
				/* make sure frameheadpos is not past end of partition */
				spool_tuples(winstate, winstate->frameheadpos - 1);
				if (winstate->frameheadpos > winstate->spooled_rows)
					winstate->frameheadpos = winstate->spooled_rows;
			}
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}
	else
		Assert(false);
}

/*
 * update_frametailpos
 * make frametailpos valid for the current row
 *
 * Uses the winobj's read pointer for any required fetches; hence, if the
 * frame mode is one that requires row comparisons, the winobj's mark must
 * not be past the currently known frame tail.	Also uses the specified slot
 * for any required fetches.
 */
static void
update_frametailpos(WindowObject winobj, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;

	if (winstate->frametail_valid)
		return;					/* already known for current row */

	if (frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING)
	{
		/* In UNBOUNDED FOLLOWING mode, all partition rows are in frame */
		spool_tuples(winstate, -1);
		winstate->frametailpos = winstate->spooled_rows - 1;
		winstate->frametail_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_END_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, exactly the rows up to current are in frame */
			winstate->frametailpos = winstate->currentpos;
			winstate->frametail_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			int64		ftnext;

			/* If no ORDER BY, all rows are peers with each other */
			if (node->ordNumCols == 0)
			{
				spool_tuples(winstate, -1);
				winstate->frametailpos = winstate->spooled_rows - 1;
				winstate->frametail_valid = true;
				return;
			}

			/*
			 * Else we have to search for the first non-peer of the current
			 * row.  We assume the current value of frametailpos is a lower
			 * bound on the possible frame tail location, ie, frame tail never
			 * goes backward, and that currentpos is also a lower bound, ie,
			 * frame end always >= current row.
			 */
			ftnext = Max(winstate->frametailpos, winstate->currentpos) + 1;
			for (;;)
			{
				if (!window_gettupleslot(winobj, ftnext, slot))
					break;		/* end of partition */
				if (!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
					break;		/* not peer of current row */
				ftnext++;
			}
			winstate->frametailpos = ftnext - 1;
			winstate->frametail_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_END_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, bound is physically n before/after current */
			int64		offset = DatumGetInt64(winstate->endOffsetValue);

			if (frameOptions & FRAMEOPTION_END_VALUE_PRECEDING)
				offset = -offset;

			winstate->frametailpos = winstate->currentpos + offset;
			/* smallest allowable value of frametailpos is -1 */
			if (winstate->frametailpos < 0)
				winstate->frametailpos = -1;
			else if (winstate->frametailpos > winstate->currentpos)
			{
				/* make sure frametailpos is not past last row of partition */
				spool_tuples(winstate, winstate->frametailpos);
				if (winstate->frametailpos >= winstate->spooled_rows)
					winstate->frametailpos = winstate->spooled_rows - 1;
			}
			winstate->frametail_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}
	else
		Assert(false);
}


/* -----------------
 * ExecWindowAgg
 *
 *	ExecWindowAgg receives tuples from its outer subplan and
 *	stores them into a tuplestore, then processes window functions.
 *	This node doesn't reduce nor qualify any row so the number of
 *	returned rows is exactly the same as its outer subplan's result
 *	(ignoring the case of SRFs in the targetlist, that is).
 * -----------------
 */
TupleTableSlot *
ExecWindowAgg(WindowAggState *winstate)
{
	TupleTableSlot *result;
	ExprDoneCond isDone;
	ExprContext *econtext;
	int			i;
	int			numfuncs;

	if (winstate->all_done)
		return NULL;

	/*
	 * Check to see if we're still projecting out tuples from a previous
	 * output tuple (because there is a function-returning-set in the
	 * projection expressions).  If so, try to project another one.
	 */
	if (winstate->ss.ps.ps_TupFromTlist)
	{
		TupleTableSlot *result;
		ExprDoneCond isDone;

		result = ExecProject(winstate->ss.ps.ps_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return result;
		/* Done with that source tuple... */
		winstate->ss.ps.ps_TupFromTlist = false;
	}

	/*
	 * Compute frame offset values, if any, during first call.
	 */
	if (winstate->all_first)
	{
		int			frameOptions = winstate->frameOptions;
		ExprContext *econtext = winstate->ss.ps.ps_ExprContext;
		Datum		value;
		bool		isnull;
		int16		len;
		bool		byval;

		if (frameOptions & FRAMEOPTION_START_VALUE)
		{
			Assert(winstate->startOffset != NULL);
			value = ExecEvalExprSwitchContext(winstate->startOffset,
											  econtext,
											  &isnull,
											  NULL);
			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("frame starting offset must not be null")));
			/* copy value into query-lifespan context */
			get_typlenbyval(exprType((Node *) winstate->startOffset->expr),
							&len, &byval);
			winstate->startOffsetValue = datumCopy(value, byval, len);
			if (frameOptions & FRAMEOPTION_ROWS)
			{
				/* value is known to be int8 */
				int64		offset = DatumGetInt64(value);

				if (offset < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					  errmsg("frame starting offset must not be negative")));
			}
		}
		if (frameOptions & FRAMEOPTION_END_VALUE)
		{
			Assert(winstate->endOffset != NULL);
			value = ExecEvalExprSwitchContext(winstate->endOffset,
											  econtext,
											  &isnull,
											  NULL);
			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("frame ending offset must not be null")));
			/* copy value into query-lifespan context */
			get_typlenbyval(exprType((Node *) winstate->endOffset->expr),
							&len, &byval);
			winstate->endOffsetValue = datumCopy(value, byval, len);
			if (frameOptions & FRAMEOPTION_ROWS)
			{
				/* value is known to be int8 */
				int64		offset = DatumGetInt64(value);

				if (offset < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("frame ending offset must not be negative")));
			}
		}
		winstate->all_first = false;
	}

restart:
	if (winstate->buffer == NULL)
	{
		/* Initialize for first partition and set current row = 0 */
		begin_partition(winstate);
		/* If there are no input rows, we'll detect that and exit below */
	}
	else
	{
		/* Advance current row within partition */
		winstate->currentpos++;
		/* This might mean that the frame moves, too */
		winstate->framehead_valid = false;
		winstate->frametail_valid = false;
	}

	/*
	 * Spool all tuples up to and including the current row, if we haven't
	 * already
	 */
	spool_tuples(winstate, winstate->currentpos);

	/* Move to the next partition if we reached the end of this partition */
	if (winstate->partition_spooled &&
		winstate->currentpos >= winstate->spooled_rows)
	{
		release_partition(winstate);

		if (winstate->more_partitions)
		{
			begin_partition(winstate);
			Assert(winstate->spooled_rows > 0);
		}
		else
		{
			winstate->all_done = true;
			return NULL;
		}
	}

	//if(TupIsNull(winstate->ss.ss_ScanTupleSlot))
	//	ExecClearTuple(winstate->ss.ss_ScanTupleSlot);

	/* final output execution is in ps_ExprContext */
	econtext = winstate->ss.ps.ps_ExprContext;

	/* Clear the per-output-tuple context for current row */
	ResetExprContext(econtext);

	/*
	 * Read the current row from the tuplestore, and save in ScanTupleSlot.
	 * (We can't rely on the outerplan's output slot because we may have to
	 * read beyond the current row.  Also, we have to actually copy the row
	 * out of the tuplestore, since window function evaluation might cause the
	 * tuplestore to dump its state to disk.)
	 *
	 * Current row must be in the tuplestore, since we spooled it above.
	 */
//#ifdef WIN_FUN_OPT
	/*
	 * When in memory, same as before
	 * When on tape, scan forward on buffer.init_file to get the ScanTupleSlot
	 */
	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
		init_tuplestore_select_read_pointer(winstate->buffer, winstate->current_ptr);
		//opt_tuplestore_gettupleslot(winstate->buffer, true, true, winstate->opt_scantupslot);
		if (!init_tuplestore_gettupleslot(winstate->buffer, true, true,
									 winstate->ss.ss_ScanTupleSlot))
			elog(ERROR, "unexpected end of tuplestore");
	}else{
//#endif
		tuplestore_select_read_pointer(winstate->buffer, winstate->current_ptr);
		if (!tuplestore_gettupleslot(winstate->buffer, true, true,
									 winstate->ss.ss_ScanTupleSlot))
			elog(ERROR, "unexpected end of tuplestore");
//#ifdef WIN_FUN_OPT
	}
//#endif

//#ifdef WIN_FUN_OPT
	/*
	 * get the opt tuple slot of the scan tuple slot, for the operations follow-up
	 */
	if(enable_winfunopt)
		tuplestore_convert_to_opt(winstate->buffer, winstate->ss.ss_ScanTupleSlot, winstate->opt_scantupslot);
//#endif

	/*
	 * Evaluate true window functions
	 */
	numfuncs = winstate->numfuncs;
	for (i = 0; i < numfuncs; i++)
	{
		WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

		if (perfuncstate->plain_agg)
			continue;
		eval_windowfunction(winstate, perfuncstate,
			  &(econtext->ecxt_aggvalues[perfuncstate->wfuncstate->wfuncno]),
			  &(econtext->ecxt_aggnulls[perfuncstate->wfuncstate->wfuncno]));
	}

	/*
	 * Evaluate aggregates
	 */
	if (winstate->numaggs > 0)
		eval_windowaggregates(winstate);

	/*
	 * Truncate any no-longer-needed rows from the tuplestore.
	 */
	tuplestore_trim(winstate->buffer);

	/*
	 * Form and return a projection tuple using the windowfunc results and the
	 * current row.  Setting ecxt_outertuple arranges that any Vars will be
	 * evaluated with respect to that row.
	 */
	econtext->ecxt_outertuple = winstate->ss.ss_ScanTupleSlot;
	result = ExecProject(winstate->ss.ps.ps_ProjInfo, &isDone);

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt)
		ExecClearTuple(winstate->opt_scantupslot);
//#endif

	if (isDone == ExprEndResult)
	{
		/* SRF in tlist returned no rows, so advance to next input tuple */
		goto restart;
	}

	winstate->ss.ps.ps_TupFromTlist =
		(isDone == ExprMultipleResult);
	return result;
}

/* -----------------
 * ExecInitWindowAgg
 *
 *	Creates the run-time information for the WindowAgg node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
WindowAggState *
ExecInitWindowAgg(WindowAgg *node, EState *estate, int eflags)
{
	WindowAggState *winstate;
	Plan	   *outerPlan;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	WindowStatePerFunc perfunc;
	WindowStatePerAgg peragg;
	int			numfuncs,
				wfuncno,
				numaggs,
				aggno;
	ListCell   *l;

//#ifdef WIN_FUN_OPT
	int			i;
	List		*opt_targetList = NIL;	/* NOTE: to initial a List, just set to NIL, do not use makeNode() */
	TupleDesc	opt_tupdesc;
	//int			usefulNum;
	int			opt_maxPos;
//#endif

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	winstate = makeNode(WindowAggState);
	winstate->ss.ps.plan = (Plan *) node;
	winstate->ss.ps.state = estate;

	/*
	 * Create expression contexts.	We need two, one for per-input-tuple
	 * processing and one for per-output-tuple processing.	We cheat a little
	 * by using ExecAssignExprContext() to build both.
	 */
	ExecAssignExprContext(estate, &winstate->ss.ps);
	tmpcontext = winstate->ss.ps.ps_ExprContext;
	winstate->tmpcontext = tmpcontext;
	ExecAssignExprContext(estate, &winstate->ss.ps);

	/* Create long-lived context for storage of partition-local memory etc */
	winstate->partcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "WindowAgg_Partition",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/* Create mid-lived context for aggregate trans values etc */
	winstate->aggcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "WindowAgg_Aggregates",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &winstate->ss);
	ExecInitResultTupleSlot(estate, &winstate->ss.ps);
	winstate->first_part_slot = ExecInitExtraTupleSlot(estate);
	winstate->agg_row_slot = ExecInitExtraTupleSlot(estate);
	winstate->temp_slot_1 = ExecInitExtraTupleSlot(estate);
	winstate->temp_slot_2 = ExecInitExtraTupleSlot(estate);

	winstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) winstate);



	/*
	 * WindowAgg nodes never have quals, since they can only occur at the
	 * logical top level of a query (ie, after any WHERE or HAVING filters)
	 */
	Assert(node->plan.qual == NIL);
	winstate->ss.ps.qual = NIL;

	/*
	 * initialize child nodes
	 */
	outerPlan = outerPlan(node);
	outerPlanState(winstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * initialize source tuple type (which is also the tuple type that we'll
	 * store in the tuplestore and use in all our working slots).
	 */
	ExecAssignScanTypeFromOuterPlan(&winstate->ss);

	ExecSetSlotDescriptor(winstate->first_part_slot,
						  winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	ExecSetSlotDescriptor(winstate->agg_row_slot,
						  winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	ExecSetSlotDescriptor(winstate->temp_slot_1,
						  winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	ExecSetSlotDescriptor(winstate->temp_slot_2,
						  winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&winstate->ss.ps);
	ExecAssignProjectionInfo(&winstate->ss.ps, NULL);

	winstate->ss.ps.ps_TupFromTlist = false;

	/* Set up data for comparing tuples */
	if (node->partNumCols > 0)
		winstate->partEqfunctions = execTuplesMatchPrepare(node->partNumCols,
														node->partOperators);
	if (node->ordNumCols > 0)
		winstate->ordEqfunctions = execTuplesMatchPrepare(node->ordNumCols,
														  node->ordOperators);

	/* add by cywang */
	/*
	 * for recomputing
	 *
	 * NOTE: now is fixed to 2, need to be more automatical
	 */
	if(enable_recompute)
		winstate->opt_tempTransValue_num = recompute_num;

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		/* first collect all the AttrNumber that are useful */
		winstate->useful2init = (AttrNumber*)palloc(sizeof(AttrNumber)*MAX_USEFUL_ATT_NUM); /* start from 1, index 0 stores the number */
		winstate->init2useful = (AttrNumber*)palloc(sizeof(AttrNumber)*(winstate->agg_row_slot->tts_tupleDescriptor->natts+1)); /* start from 1*/
		memset(winstate->useful2init, -1, sizeof(AttrNumber)*MAX_USEFUL_ATT_NUM);
		memset(winstate->init2useful, -1, sizeof(AttrNumber)*(winstate->agg_row_slot->tts_tupleDescriptor->natts+1));

		winstate->useful2init[0] = 0;

		/*
		 * add order by clause first
		 *
		 * ordColIdx is also used to check if two tuple slot are peers,
		 * so we need an alternative opt_ordColIdx
		 */
		if(node->ordNumCols >0)
			node->opt_ordColIdx = (AttrNumber*)palloc(node->ordNumCols*sizeof(AttrNumber));
		for(i=0; i<node->ordNumCols; i++){
			//winstate->useful2init[++usefulNum] = node->ordColIdx[i];
			//winstate->init2useful[node->ordColIdx[i]] = usefulNum;
			AttrNumber pos = addNumber(winstate->useful2init, node->ordColIdx[i]);
			winstate->init2useful[node->ordColIdx[i]] = pos;
			node->opt_ordColIdx[i] = pos;
		}

		//add function parameters then TODO
	}
//#endif

	/*
	 * WindowAgg nodes use aggvalues and aggnulls as well as Agg nodes.
	 */
	numfuncs = winstate->numfuncs;
	numaggs = winstate->numaggs;
	econtext = winstate->ss.ps.ps_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc0(sizeof(Datum) * numfuncs);
	econtext->ecxt_aggnulls = (bool *) palloc0(sizeof(bool) * numfuncs);

	/*
	 * allocate per-wfunc/per-agg state information.
	 */
	perfunc = (WindowStatePerFunc) palloc0(sizeof(WindowStatePerFuncData) * numfuncs);
	peragg = (WindowStatePerAgg) palloc0(sizeof(WindowStatePerAggData) * numaggs);
	winstate->perfunc = perfunc;
	winstate->peragg = peragg;

	wfuncno = -1;
	aggno = -1;
	foreach(l, winstate->funcs)
	{
		WindowFuncExprState *wfuncstate = (WindowFuncExprState *) lfirst(l);
		WindowFunc *wfunc = (WindowFunc *) wfuncstate->xprstate.expr;
		WindowStatePerFunc perfuncstate;
		AclResult	aclresult;
		int			i;

//#ifdef WIN_FUN_OPT
		ListCell *arg;
//#endif

		if (wfunc->winref != node->winref)		/* planner screwed up? */
			elog(ERROR, "WindowFunc with winref %u assigned to WindowAgg with winref %u",
				 wfunc->winref, node->winref);

		/* Look for a previous duplicate window function */
		for (i = 0; i <= wfuncno; i++)
		{
			if (equal(wfunc, perfunc[i].wfunc) &&
				!contain_volatile_functions((Node *) wfunc))
				break;
		}
		if (i <= wfuncno)
		{
			/* Found a match to an existing entry, so just mark it */
			wfuncstate->wfuncno = i;
			continue;
		}

		/* Nope, so assign a new PerAgg record */
		perfuncstate = &perfunc[++wfuncno];

		/* Mark WindowFunc state node with assigned index in the result array */
		wfuncstate->wfuncno = wfuncno;

		/* Check permission to call window function */
		aclresult = pg_proc_aclcheck(wfunc->winfnoid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(wfunc->winfnoid));

		/* Fill in the perfuncstate data */
		perfuncstate->wfuncstate = wfuncstate;
		perfuncstate->wfunc = wfunc;
		perfuncstate->numArguments = list_length(wfuncstate->args);

		fmgr_info_cxt(wfunc->winfnoid, &perfuncstate->flinfo,
					  econtext->ecxt_per_query_memory);
		fmgr_info_set_expr((Node *) wfunc, &perfuncstate->flinfo);

		perfuncstate->winCollation = wfunc->inputcollid;

		get_typlenbyval(wfunc->wintype,
						&perfuncstate->resulttypeLen,
						&perfuncstate->resulttypeByVal);

		/*
		 * If it's really just a plain aggregate function, we'll emulate the
		 * Agg environment for it.
		 */
		perfuncstate->plain_agg = wfunc->winagg;
		if (wfunc->winagg)
		{
			WindowStatePerAgg peraggstate;

			perfuncstate->aggno = ++aggno;
			peraggstate = &winstate->peragg[aggno];
			initialize_peragg(winstate, wfunc, peraggstate);
			peraggstate->wfuncno = wfuncno;
		}
		else
		{
			WindowObject winobj = makeNode(WindowObjectData);

			winobj->winstate = winstate;
			winobj->argstates = wfuncstate->args;
			winobj->localmem = NULL;
			perfuncstate->winobj = winobj;
		}

		/* add by cywang */
//#ifdef WIN_FUN_OPT
		if(enable_winfunopt){
			/*
			 * initial the alternative arguments of a window function, WindowFuncExprState.opt_args
			 * and add the attribute number to useful2init
			 */
			wfuncstate->opt_args = NIL;
			foreach(arg, wfuncstate->args){
				ExprState  *argstate = (ExprState *) lfirst(arg);
				//currently, we only treat the T_Var type arguments as useful

				if(argstate->expr->type == T_Var){
					Var *var = (Var*)argstate->expr;
					//need to make a node first, or the child pointer attribute will share the common pointer with the source struct
					ExprState *opt_argstate = (ExprState*)makeNode(ExprState);
					Var *opt_var = (Var*)makeNode(Var);
					memcpy(opt_argstate, argstate, sizeof(ExprState));
					opt_argstate->expr = (Expr*)opt_var;
					memcpy((Var*)opt_argstate->expr, (Var*)var, sizeof(Var));
					((Var*)opt_argstate->expr)->varattno = addNumber(winstate->useful2init, ((Var*)opt_argstate->expr)->varattno);
					((Var*)opt_argstate->expr)->varoattno = ((Var*)opt_argstate->expr)->varattno;
					wfuncstate->opt_args = lappend(wfuncstate->opt_args, opt_argstate);
				}


			}

			/* for window function */
			if(!wfunc->winagg){
				perfuncstate->winobj->opt_argstates = wfuncstate->opt_args;
			}
		}
//#endif

	}

	/* Update numfuncs, numaggs to match number of unique functions found */
	winstate->numfuncs = wfuncno + 1;
	winstate->numaggs = aggno + 1;

	/* Set up WindowObject for aggregates, if needed */
	if (winstate->numaggs > 0)
	{
		WindowObject agg_winobj = makeNode(WindowObjectData);

		agg_winobj->winstate = winstate;
		agg_winobj->argstates = NIL;
		agg_winobj->localmem = NULL;
		/* make sure markptr = -1 to invalidate. It may not get used */
		agg_winobj->markptr = -1;
		agg_winobj->readptr = -1;
		winstate->agg_winobj = agg_winobj;

		/* by cywang */
		if(enable_winfunopt)
			agg_winobj->opt_argstates = NIL;
		agg_winobj->opt_frameheadptr = -1;
		agg_winobj->opt_tempTransEndPtr[0] = -1;
	}

	/* copy frame options to state node for easy access */
	winstate->frameOptions = node->frameOptions;

	/* initialize frame bound offset expressions */
	winstate->startOffset = ExecInitExpr((Expr *) node->startOffset,
										 (PlanState *) winstate);
	winstate->endOffset = ExecInitExpr((Expr *) node->endOffset,
									   (PlanState *) winstate);

	winstate->all_first = true;
	winstate->partition_spooled = false;
	winstate->more_partitions = false;

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		opt_maxPos = 0;
		for(i=1; i<=winstate->useful2init[0]; i++){
			if(winstate->useful2init[i]>opt_maxPos)
				opt_maxPos = winstate->useful2init[i];
		}
		winstate->opt_maxPos = opt_maxPos;

		//opt_targetList = (List*)makeNode(List); /* NOTE: to initial a List, just set to NIL */

		// make TargetEntry
		for(i=1; i<=winstate->useful2init[0]; i++){
			AttrNumber 	varattno = winstate->useful2init[i];
			int temp = varattno-1;	/* attributes of a TupleDesc starts from 0, while varattno starts from 1 */
			Index 	varno = winstate->agg_row_slot->tts_tupleDescriptor->attrs[temp]->attrelid; /* to check */
			Oid 	vartype = (Oid)winstate->agg_row_slot->tts_tupleDescriptor->attrs[temp]->atttypid;
			int32 	vartypmod = (int32)winstate->agg_row_slot->tts_tupleDescriptor->attrs[temp]->atttypmod;
			Oid 	varcollid = (Oid)winstate->agg_row_slot->tts_tupleDescriptor->attrs[temp]->attcollation;
			Index 	varlevelsup = 0;
			Var *var = makeVar(varno, varattno, vartype, vartypmod, varcollid, varlevelsup); /* All the useful attributes will be Var */
			TargetEntry *tle =
					makeTargetEntry((Expr*) var, varattno,
							winstate->agg_row_slot->tts_tupleDescriptor->attrs[temp]->attname, false ); /* to check */
			opt_targetList = lappend(opt_targetList, tle);
		}
		//winstate->opt_targetList = opt_targetList; /* no need */
		// make TupleDesc by targetList
		opt_tupdesc = ExecTypeFromTL(opt_targetList, false); /* can't use ExecCleanTypeFromTL() */
		winstate->opt_tupdesc = opt_tupdesc;

		// set tuple description
		winstate->opt_agg_row_slot = ExecInitExtraTupleSlot(estate);
		ExecSetSlotDescriptor(winstate->opt_agg_row_slot, winstate->opt_tupdesc);
		winstate->buffer_slot = ExecInitExtraTupleSlot(estate); /* used to extract attributes from a initial MinimalTuple*/
		ExecSetSlotDescriptor(winstate->buffer_slot, winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
		winstate->opt_first_part_slot = ExecInitExtraTupleSlot(estate);
		ExecSetSlotDescriptor(winstate->opt_first_part_slot, winstate->opt_tupdesc);
		winstate->opt_temp_slot_1 = ExecInitExtraTupleSlot(estate);
		ExecSetSlotDescriptor(winstate->opt_temp_slot_1, winstate->opt_tupdesc);
		winstate->opt_temp_slot_2 = ExecInitExtraTupleSlot(estate);
		ExecSetSlotDescriptor(winstate->opt_temp_slot_2, winstate->opt_tupdesc);
		winstate->opt_scantupslot = ExecInitExtraTupleSlot(estate);
		ExecSetSlotDescriptor(winstate->opt_scantupslot, winstate->opt_tupdesc);
		// make ProjectionInfo by TupleTableSlot, TupleDesc and targetList
		//ProjectionInfo *projInfo = ExecBuildProjectionInfo(winstate->opt_targetList, winstate->ss.ps->ps_ExprContext, winstate->opt_agg_row_slot, NULL); /* no need */
		//winstate->opt_projInfo = projInfo; /* no need */
	}
//#endif

/*
	if(enable_recompute){
		winstate->opt_tempStartSlot = ExecInitExtraTupleSlot(estate);
		winstate->opt_tempEndSlot = ExecInitExtraTupleSlot(estate);
		if(enable_winfunopt){
			ExecSetSlotDescriptor(winstate->opt_tempStartSlot, winstate->opt_tupdesc);
			ExecSetSlotDescriptor(winstate->opt_tempEndSlot, winstate->opt_tupdesc);
		}else{
			ExecSetSlotDescriptor(winstate->opt_tempStartSlot, winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
			ExecSetSlotDescriptor(winstate->opt_tempEndSlot, winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
		}
	}
*/

	/* init the instruments */
	winstate->instr_total_part = InstrAlloc(1,1);
	//winstate->instr_checkinframe_part = InstrAlloc(1,1);
	winstate->instr_locateheadpos_part = InstrAlloc(1,1);
	winstate->instr_recompute_part = InstrAlloc(1,1);
	//winstate->instr_setmarkpos_part = InstrAlloc(1,1);
	winstate->instr_cpu_afterspooled = InstrAlloc(1,1);
	winstate->instr_recompute_cpu = InstrAlloc(1,1);
	winstate->instr_io_afterspooled = InstrAlloc(1,1);

	if(enable_winfunopt)
		printf("\n--------------------- with opt ------------------------\n");
	else
		printf("\n--------------------- without opt ---------------------\n");
	if(enable_locate)
		printf("--------------------- with locate ------------------------\n");
	else
		printf("--------------------- without locate ---------------------\n");
	if(enable_recompute)
		printf("--------------------- with recompute ------------------------\n");
	else
		printf("--------------------- without recompute ---------------------\n");
	if(enable_reusebuffer)
		printf("--------------------- with reusebuffer ------------------------\n");
	else
		printf("--------------------- without reusebuffer ------------------------\n");

	return winstate;
}

/* -----------------
 * ExecEndWindowAgg
 * -----------------
 */
void
ExecEndWindowAgg(WindowAggState *node)
{
	PlanState  *outerPlan;


	if(enable_reusebuffer)
		printf("--------------------- with reusebuffer end ------------------------\n");
	else
		printf("--------------------- without reusebuffer end ------------------------\n");
	if(enable_recompute)
		printf("--------------------- with recompute end ------------------------\n");
	else
		printf("--------------------- without recompute end ---------------------\n");
	if(enable_locate)
		printf("--------------------- with locate end ------------------------\n");
	else
		printf("--------------------- without locate end ---------------------\n");
	if(enable_winfunopt)
		printf("--------------------- with opt end --------------------\n\n\n");
	else
		printf("--------------------- without opt end -----------------\n\n\n");



	/* add by cywang */
//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		if(node->useful2init){
			pfree(node->useful2init);
			pfree(node->init2useful);
		}
		/* add some other operations */
		ExecClearTuple(node->opt_scantupslot);
		ExecClearTuple(node->opt_first_part_slot);
		ExecClearTuple(node->opt_agg_row_slot);
		ExecClearTuple(node->opt_temp_slot_1);
		ExecClearTuple(node->opt_temp_slot_2);
		ExecClearTuple(node->buffer_slot);
	}
//#endif

	release_partition(node);

	pfree(node->perfunc);
	pfree(node->peragg);

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->first_part_slot);
	ExecClearTuple(node->agg_row_slot);
	ExecClearTuple(node->temp_slot_1);
	ExecClearTuple(node->temp_slot_2);

	/*
	 * Free both the expr contexts.
	 */
	ExecFreeExprContext(&node->ss.ps);
	node->ss.ps.ps_ExprContext = node->tmpcontext;
	ExecFreeExprContext(&node->ss.ps);

	MemoryContextDelete(node->partcontext);
	MemoryContextDelete(node->aggcontext);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

/* -----------------
 * ExecRescanWindowAgg
 * -----------------
 */
void
ExecReScanWindowAgg(WindowAggState *node)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	node->all_done = false;

	node->ss.ps.ps_TupFromTlist = false;
	node->all_first = true;

	/* release tuplestore et al */
	release_partition(node);

	/* release all temp tuples, but especially first_part_slot */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->first_part_slot);
	ExecClearTuple(node->agg_row_slot);
	ExecClearTuple(node->temp_slot_1);
	ExecClearTuple(node->temp_slot_2);

	/* Forget current wfunc values */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * node->numfuncs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * node->numfuncs);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->ss.ps.lefttree->chgParam == NULL)
		ExecReScan(node->ss.ps.lefttree);
}

/*
 * initialize_peragg
 *
 * Almost same as in nodeAgg.c, except we don't support DISTINCT currently.
 */
static WindowStatePerAggData *
initialize_peragg(WindowAggState *winstate, WindowFunc *wfunc,
				  WindowStatePerAgg peraggstate)
{
	Oid			inputTypes[FUNC_MAX_ARGS];
	int			numArguments;
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	Oid			aggtranstype;
	AclResult	aclresult;
	Oid			transfn_oid,
				finalfn_oid;
	Expr	   *transfnexpr,
			   *finalfnexpr;
	Datum		textInitVal;
	int			i;
	ListCell   *lc;

	numArguments = list_length(wfunc->args);

	i = 0;
	foreach(lc, wfunc->args)
	{
		inputTypes[i++] = exprType((Node *) lfirst(lc));
	}

	aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(wfunc->winfnoid));
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u",
			 wfunc->winfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

	/*
	 * ExecInitWindowAgg already checked permission to call aggregate function
	 * ... but we still need to check the component functions
	 */

	peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
	peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

	/* Check that aggregate owner has permission to call component fns */
	{
		HeapTuple	procTuple;
		Oid			aggOwner;

		procTuple = SearchSysCache1(PROCOID,
									ObjectIdGetDatum(wfunc->winfnoid));
		if (!HeapTupleIsValid(procTuple))
			elog(ERROR, "cache lookup failed for function %u",
				 wfunc->winfnoid);
		aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
		ReleaseSysCache(procTuple);

		aclresult = pg_proc_aclcheck(transfn_oid, aggOwner,
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(transfn_oid));
		if (OidIsValid(finalfn_oid))
		{
			aclresult = pg_proc_aclcheck(finalfn_oid, aggOwner,
										 ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_PROC,
							   get_func_name(finalfn_oid));
		}
	}

	/* resolve actual type of transition state, if polymorphic */
	aggtranstype = aggform->aggtranstype;
	if (IsPolymorphicType(aggtranstype))
	{
		/* have to fetch the agg's declared input types... */
		Oid		   *declaredArgTypes;
		int			agg_nargs;

		get_func_signature(wfunc->winfnoid,
						   &declaredArgTypes, &agg_nargs);
		Assert(agg_nargs == numArguments);
		aggtranstype = enforce_generic_type_consistency(inputTypes,
														declaredArgTypes,
														agg_nargs,
														aggtranstype,
														false);
		pfree(declaredArgTypes);
	}

	/* build expression trees using actual argument & result types */
	build_aggregate_fnexprs(inputTypes,
							numArguments,
							aggtranstype,
							wfunc->wintype,
							wfunc->inputcollid,
							transfn_oid,
							finalfn_oid,
							&transfnexpr,
							&finalfnexpr);

	fmgr_info(transfn_oid, &peraggstate->transfn);
	fmgr_info_set_expr((Node *) transfnexpr, &peraggstate->transfn);

	if (OidIsValid(finalfn_oid))
	{
		fmgr_info(finalfn_oid, &peraggstate->finalfn);
		fmgr_info_set_expr((Node *) finalfnexpr, &peraggstate->finalfn);
	}

	get_typlenbyval(wfunc->wintype,
					&peraggstate->resulttypeLen,
					&peraggstate->resulttypeByVal);
	get_typlenbyval(aggtranstype,
					&peraggstate->transtypeLen,
					&peraggstate->transtypeByVal);

	/*
	 * initval is potentially null, so don't try to access it as a struct
	 * field. Must do it the hard way with SysCacheGetAttr.
	 */
	textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
								  Anum_pg_aggregate_agginitval,
								  &peraggstate->initValueIsNull);

	if (peraggstate->initValueIsNull)
		peraggstate->initValue = (Datum) 0;
	else
		peraggstate->initValue = GetAggInitVal(textInitVal,
											   aggtranstype);

	/*
	 * If the transfn is strict and the initval is NULL, make sure input type
	 * and transtype are the same (or at least binary-compatible), so that
	 * it's OK to use the first input value as the initial transValue.  This
	 * should have been checked at agg definition time, but just in case...
	 */
	if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
	{
		if (numArguments < 1 ||
			!IsBinaryCoercible(inputTypes[0], aggtranstype))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate %u needs to have compatible input type and transition type",
							wfunc->winfnoid)));
	}

	ReleaseSysCache(aggTuple);

	return peraggstate;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

/*
 * are_peers
 * compare two rows to see if they are equal according to the ORDER BY clause
 *
 * NB: this does not consider the window frame mode.
 */
static bool
are_peers(WindowAggState *winstate, TupleTableSlot *slot1,
		  TupleTableSlot *slot2)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;

	/* If no ORDER BY, all rows are peers with each other */
	if (node->ordNumCols == 0)
		return true;

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		/*
		 * When not in memory, the compare of two slots is for opt tuple.
		 */
		if(!tuplestore_in_memory(winstate->buffer)){
			return execTuplesMatch(slot1, slot2,
								   node->ordNumCols, node->opt_ordColIdx,
								   winstate->ordEqfunctions,
								   winstate->tmpcontext->ecxt_per_tuple_memory);
		}
	}
//#endif

	return execTuplesMatch(slot1, slot2,
						   node->ordNumCols, node->ordColIdx,
						   winstate->ordEqfunctions,
						   winstate->tmpcontext->ecxt_per_tuple_memory);
}

/*
 * window_gettupleslot
 *	Fetch the pos'th tuple of the current partition into the slot,
 *	using the winobj's read pointer
 *
 * Returns true if successful, false if no such row
 */
static bool
window_gettupleslot(WindowObject winobj, int64 pos, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	MemoryContext oldcontext;

	/* Don't allow passing -1 to spool_tuples here */
	if (pos < 0)
		return false;

	/* If necessary, fetch the tuple into the spool */
	spool_tuples(winstate, pos);

	if (pos >= winstate->spooled_rows)
		return false;

	if (pos < winobj->markpos)
		elog(ERROR, "cannot fetch row before WindowObject's mark position");

	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);

	/*
	 * There's no API to refetch the tuple at the current position. We have to
	 * move one tuple forward, and then one backward.  (We don't do it the
	 * other way because we might try to fetch the row before our mark, which
	 * isn't allowed.)  XXX this case could stand to be optimized.
	 */
	if (winobj->seekpos == pos)
	{
		tuplestore_advance(winstate->buffer, true);
		winobj->seekpos++;
	}

	while (winobj->seekpos > pos)
	{
		if (!tuplestore_gettupleslot(winstate->buffer, false, true, slot))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos--;
	}

	while (winobj->seekpos < pos)
	{
		if (!tuplestore_gettupleslot(winstate->buffer, true, true, slot))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos++;
	}

	MemoryContextSwitchTo(oldcontext);

	return true;
}


/***********************************************************************
 * API exposed to window functions
 ***********************************************************************/


/*
 * WinGetPartitionLocalMemory
 *		Get working memory that lives till end of partition processing
 *
 * On first call within a given partition, this allocates and zeroes the
 * requested amount of space.  Subsequent calls just return the same chunk.
 *
 * Memory obtained this way is normally used to hold state that should be
 * automatically reset for each new partition.	If a window function wants
 * to hold state across the whole query, fcinfo->fn_extra can be used in the
 * usual way for that.
 */
void *
WinGetPartitionLocalMemory(WindowObject winobj, Size sz)
{
	Assert(WindowObjectIsValid(winobj));
	if (winobj->localmem == NULL)
		winobj->localmem =
			MemoryContextAllocZero(winobj->winstate->partcontext, sz);
	return winobj->localmem;
}

/*
 * WinGetCurrentPosition
 *		Return the current row's position (counting from 0) within the current
 *		partition.
 */
int64
WinGetCurrentPosition(WindowObject winobj)
{
	Assert(WindowObjectIsValid(winobj));
	return winobj->winstate->currentpos;
}

/*
 * WinGetPartitionRowCount
 *		Return total number of rows contained in the current partition.
 *
 * Note: this is a relatively expensive operation because it forces the
 * whole partition to be "spooled" into the tuplestore at once.  Once
 * executed, however, additional calls within the same partition are cheap.
 */
int64
WinGetPartitionRowCount(WindowObject winobj)
{
	Assert(WindowObjectIsValid(winobj));
	spool_tuples(winobj->winstate, -1);
	return winobj->winstate->spooled_rows;
}

/*
 * WinSetMarkPosition
 *		Set the "mark" position for the window object, which is the oldest row
 *		number (counting from 0) it is allowed to fetch during all subsequent
 *		operations within the current partition.
 *
 * Window functions do not have to call this, but are encouraged to move the
 * mark forward when possible to keep the tuplestore size down and prevent
 * having to spill rows to disk.
 */
void
WinSetMarkPosition(WindowObject winobj, int64 markpos)
{
	WindowAggState *winstate;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		/*
		 * if the buffer is not in memory, which means the following part of the partition is on tape,
		 * so there is no need to keep the mark position any more.
		 *
		 * NOTE: this may cause logical error, need to check!
		 */
		if(!tuplestore_in_memory(winstate->buffer))
			return;
	}
//#endif

	/* as this function may call tuplestore_advance, we record the time of setting the mark position during the partition */
	//InstrStartNode(winstate->instr_setmarkpos_part);

	if (markpos < winobj->markpos)
		elog(ERROR, "cannot move WindowObject's mark position backward");
	tuplestore_select_read_pointer(winstate->buffer, winobj->markptr);
	while (markpos > winobj->markpos)
	{
		tuplestore_advance(winstate->buffer, true);
		winobj->markpos++;
	}
	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	while (markpos > winobj->seekpos)
	{
		tuplestore_advance(winstate->buffer, true);
		winobj->seekpos++;
	}

	//InstrStopNode(winstate->instr_setmarkpos_part, 0);
}

/*
 * WinRowsArePeers
 *		Compare two rows (specified by absolute position in window) to see
 *		if they are equal according to the ORDER BY clause.
 *
 * NB: this does not consider the window frame mode.
 */
bool
WinRowsArePeers(WindowObject winobj, int64 pos1, int64 pos2)
{
	WindowAggState *winstate;
	WindowAgg  *node;
	TupleTableSlot *slot1;
	TupleTableSlot *slot2;
	bool		res;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	node = (WindowAgg *) winstate->ss.ps.plan;

	/* If no ORDER BY, all rows are peers; don't bother to fetch them */
	if (node->ordNumCols == 0)
		return true;

	slot1 = winstate->temp_slot_1;
	slot2 = winstate->temp_slot_2;

	if (!window_gettupleslot(winobj, pos1, slot1))
		elog(ERROR, "specified position is out of window: " INT64_FORMAT,
			 pos1);
	if (!window_gettupleslot(winobj, pos2, slot2))
		elog(ERROR, "specified position is out of window: " INT64_FORMAT,
			 pos2);

	res = are_peers(winstate, slot1, slot2);

	ExecClearTuple(slot1);
	ExecClearTuple(slot2);

	return res;
}

/*
 * WinGetFuncArgInPartition
 *		Evaluate a window function's argument expression on a specified
 *		row of the partition.  The row is identified in lseek(2) style,
 *		i.e. relative to the current, first, or last row.
 *
 * argno: argument number to evaluate (counted from 0)
 * relpos: signed rowcount offset from the seek position
 * seektype: WINDOW_SEEK_CURRENT, WINDOW_SEEK_HEAD, or WINDOW_SEEK_TAIL
 * set_mark: If the row is found and set_mark is true, the mark is moved to
 *		the row as a side-effect.
 * isnull: output argument, receives isnull status of result
 * isout: output argument, set to indicate whether target row position
 *		is out of partition (can pass NULL if caller doesn't care about this)
 *
 * Specifying a nonexistent row is not an error, it just causes a null result
 * (plus setting *isout true, if isout isn't NULL).
 */
Datum
WinGetFuncArgInPartition(WindowObject winobj, int argno,
						 int relpos, int seektype, bool set_mark,
						 bool *isnull, bool *isout)
{
	WindowAggState *winstate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	bool		gottuple;
	int64		abs_pos;

//#ifdef WIN_FUN_OPT
	TupleTableSlot *opt_slot = winobj->winstate->opt_temp_slot_1;
//#endif

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	econtext = winstate->ss.ps.ps_ExprContext;
	slot = winstate->temp_slot_1;

	switch (seektype)
	{
		case WINDOW_SEEK_CURRENT:
			abs_pos = winstate->currentpos + relpos;
			break;
		case WINDOW_SEEK_HEAD:
			abs_pos = relpos;
			break;
		case WINDOW_SEEK_TAIL:
			spool_tuples(winstate, -1);
			abs_pos = winstate->spooled_rows - 1 + relpos;
			break;
		default:
			elog(ERROR, "unrecognized window seek type: %d", seektype);
			abs_pos = 0;		/* keep compiler quiet */
			break;
	}

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt)
		gottuple = opt_window_gettupleslot(winobj, abs_pos, slot, opt_slot);
//#else
	else
		gottuple = window_gettupleslot(winobj, abs_pos, slot);
//#endif

	if (!gottuple)
	{
		if (isout)
			*isout = true;
		*isnull = true;
		return (Datum) 0;
	}
	else
	{
		if (isout)
			*isout = false;
		if (set_mark)
		{
			int			frameOptions = winstate->frameOptions;
			int64		mark_pos = abs_pos;

			/*
			 * In RANGE mode with a moving frame head, we must not let the
			 * mark advance past frameheadpos, since that row has to be
			 * fetchable during future update_frameheadpos calls.
			 *
			 * XXX it is very ugly to pollute window functions' marks with
			 * this consideration; it could for instance mask a logic bug that
			 * lets a window function fetch rows before what it had claimed
			 * was its mark.  Perhaps use a separate mark for frame head
			 * probes?
			 */
			if ((frameOptions & FRAMEOPTION_RANGE) &&
				!(frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING))
			{
//#ifdef WIN_FUN_OPT
				if(enable_winfunopt)
					opt_update_frameheadpos(winobj, winstate->temp_slot_2, winstate->opt_temp_slot_2);
//#else
				else
					update_frameheadpos(winobj, winstate->temp_slot_2);
//#endif
				if (mark_pos > winstate->frameheadpos)
					mark_pos = winstate->frameheadpos;
			}
			WinSetMarkPosition(winobj, mark_pos);
		}

//#ifdef WIN_FUN_OPT
		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			/*
			 * use which?
			 *
			 * currently, we still use ecxt_outertuple
			 * if it fails, we will use the alternative opt_exct_slot
			 */
			econtext->ecxt_outertuple = opt_slot;
			//econtext->opt_exct_slot = opt_slot;
			return ExecEvalExpr((ExprState *) list_nth(winobj->opt_argstates, argno),
								econtext, isnull, NULL);
		}else{
//#endif
			econtext->ecxt_outertuple = slot;
			return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
								econtext, isnull, NULL);
//#ifdef WIN_FUN_OPT
		}
//#endif
	}
}

/*
 * WinGetFuncArgInFrame
 *		Evaluate a window function's argument expression on a specified
 *		row of the window frame.  The row is identified in lseek(2) style,
 *		i.e. relative to the current, first, or last row.
 *
 * argno: argument number to evaluate (counted from 0)
 * relpos: signed rowcount offset from the seek position
 * seektype: WINDOW_SEEK_CURRENT, WINDOW_SEEK_HEAD, or WINDOW_SEEK_TAIL
 * set_mark: If the row is found and set_mark is true, the mark is moved to
 *		the row as a side-effect.
 * isnull: output argument, receives isnull status of result
 * isout: output argument, set to indicate whether target row position
 *		is out of frame (can pass NULL if caller doesn't care about this)
 *
 * Specifying a nonexistent row is not an error, it just causes a null result
 * (plus setting *isout true, if isout isn't NULL).
 */
Datum
WinGetFuncArgInFrame(WindowObject winobj, int argno,
					 int relpos, int seektype, bool set_mark,
					 bool *isnull, bool *isout)
{
	WindowAggState *winstate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	bool		gottuple;
	int64		abs_pos;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	econtext = winstate->ss.ps.ps_ExprContext;
	slot = winstate->temp_slot_1;

	switch (seektype)
	{
		case WINDOW_SEEK_CURRENT:
			abs_pos = winstate->currentpos + relpos;
			break;
		case WINDOW_SEEK_HEAD:
//#ifdef WIN_FUN_OPT
			if(enable_winfunopt)
				opt_update_frameheadpos(winobj, slot, winstate->opt_temp_slot_1);
//#else
			else
				update_frameheadpos(winobj, slot);
//#endif
			abs_pos = winstate->frameheadpos + relpos;
			break;
		case WINDOW_SEEK_TAIL:
//#ifdef WIN_FUN_OPT
			if(enable_winfunopt)
				opt_update_frametailpos(winobj, slot, winstate->opt_temp_slot_1);
//#else
			else
				update_frametailpos(winobj, slot);
//#endif
			abs_pos = winstate->frametailpos + relpos;
			break;
		default:
			elog(ERROR, "unrecognized window seek type: %d", seektype);
			abs_pos = 0;		/* keep compiler quiet */
			break;
	}

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		gottuple = opt_window_gettupleslot(winobj, abs_pos, slot, winstate->opt_temp_slot_1);
		if(gottuple){
			if(tuplestore_in_memory(winstate->buffer))
				gottuple = row_is_in_frame(winstate, abs_pos, slot);
			else
				gottuple = row_is_in_frame(winstate, abs_pos, winstate->opt_temp_slot_1);
		}
	}else{
//#else
		gottuple = window_gettupleslot(winobj, abs_pos, slot);
		if (gottuple){
			gottuple = row_is_in_frame(winstate, abs_pos, slot);
		}
	}
//#endif

	if (!gottuple)
	{
		if (isout)
			*isout = true;
		*isnull = true;
		return (Datum) 0;
	}
	else
	{
		if (isout)
			*isout = false;
		if (set_mark)
		{
			int			frameOptions = winstate->frameOptions;
			int64		mark_pos = abs_pos;

			/*
			 * In RANGE mode with a moving frame head, we must not let the
			 * mark advance past frameheadpos, since that row has to be
			 * fetchable during future update_frameheadpos calls.
			 *
			 * XXX it is very ugly to pollute window functions' marks with
			 * this consideration; it could for instance mask a logic bug that
			 * lets a window function fetch rows before what it had claimed
			 * was its mark.  Perhaps use a separate mark for frame head
			 * probes?
			 */
			if ((frameOptions & FRAMEOPTION_RANGE) &&
				!(frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING))
			{
//#ifdef WIN_FUN_OPT
				if(enable_winfunopt)
					opt_update_frameheadpos(winobj, winstate->temp_slot_2, winstate->opt_temp_slot_2);
//#else
				else
					update_frameheadpos(winobj, winstate->temp_slot_2);
//#endif
				if (mark_pos > winstate->frameheadpos)
					mark_pos = winstate->frameheadpos;
			}
			WinSetMarkPosition(winobj, mark_pos);
		}

		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			econtext->ecxt_outertuple = winstate->opt_temp_slot_1;
			return ExecEvalExpr((ExprState *) list_nth(winobj->opt_argstates, argno),
								econtext, isnull, NULL);
		}else{
			econtext->ecxt_outertuple = slot;
			return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
								econtext, isnull, NULL);
		}
	}
}

/*
 * WinGetFuncArgCurrent
 *		Evaluate a window function's argument expression on the current row.
 *
 * argno: argument number to evaluate (counted from 0)
 * isnull: output argument, receives isnull status of result
 *
 * Note: this isn't quite equivalent to WinGetFuncArgInPartition or
 * WinGetFuncArgInFrame targeting the current row, because it will succeed
 * even if the WindowObject's mark has been set beyond the current row.
 * This should generally be used for "ordinary" arguments of a window
 * function, such as the offset argument of lead() or lag().
 */
Datum
WinGetFuncArgCurrent(WindowObject winobj, int argno, bool *isnull)
{
	WindowAggState *winstate;
	ExprContext *econtext;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	econtext = winstate->ss.ps.ps_ExprContext;

	/* by cywang */
	/*
	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
		econtext->ecxt_outertuple = winstate->opt_scantupslot;
		return ExecEvalExpr((ExprState *) list_nth(winobj->opt_argstates, argno),
							econtext, isnull, NULL);
	}else{
		econtext->ecxt_outertuple = winstate->ss.ss_ScanTupleSlot;
		return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
							econtext, isnull, NULL);
	}
	*/
	/*
	 * by cywang
	 *
	 * as ss_ScanTupleSlot is always available, we don't change this function here
	 */
	econtext->ecxt_outertuple = winstate->ss.ss_ScanTupleSlot;
	return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
						econtext, isnull, NULL);
}

//#ifdef WIN_FUN_OPT
/*
 * add a attribute to useful2init
 * elements in useful2init are unique
 * return the init2useful[num]
 */
static AttrNumber addNumber(AttrNumber *array, AttrNumber num){
	AttrNumber i;
	for(i=1; i<=array[0]; i++){
		if(array[i] == num)
			return i;
	}
	array[0]++;
	array[array[0]] = num;
	return array[0];
}
/*
 * as the parameters of window_gettupleslot has one slot,
 * while the slot type can be determined during the function,
 * we have to let the caller to determine which slot to fetch.
 */
bool opt_window_gettupleslot(WindowObject winobj, int64 pos, TupleTableSlot *slot, TupleTableSlot *opt_slot){
	WindowAggState *winstate = winobj->winstate;
	MemoryContext oldcontext;

	/* Don't allow passing -1 to spool_tuples here */
	if (pos < 0)
		return false;

	/* If necessary, fetch the tuple into the spool */
	spool_tuples(winstate, pos);

	if (pos >= winstate->spooled_rows)
		return false;

	if (pos < winobj->markpos)
		elog(ERROR, "cannot fetch row before WindowObject's mark position");

	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

//#ifdef WIN_FUN_OPT
	/*
	 * When not in memory, we use the alternative read pointers
	 *
	 * OK
	 */
	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
		opt_tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	}else{
//endif
		tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
//#ifdef WIN_FUN_OPT
	}
//#endif


	/*
	 * There's no API to refetch the tuple at the current position. We have to
	 * move one tuple forward, and then one backward.  (We don't do it the
	 * other way because we might try to fetch the row before our mark, which
	 * isn't allowed.)  XXX this case could stand to be optimized.
	 */
	if (winobj->seekpos == pos)
	{
//#ifdef WIN_FUN_OPT
		if(enable_winfunopt)
			opt_tuplestore_advance(winstate->buffer, true);
//#else
		else
			tuplestore_advance(winstate->buffer, true);
//#endif
		winobj->seekpos++;
	}


	while (winobj->seekpos > pos)
	{
		/*
		 * by cywang
		 *
		 * here we can use seekpos-pos>=2 as a condition to add to locate time
		 */
//#ifdef WIN_FUN_OPT
		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			if(!opt_tuplestore_gettupleslot(winstate->buffer, false, true, opt_slot))
				elog(ERROR, "unexpected end of tuplestore");
		}else{
//endif
			if (!tuplestore_gettupleslot(winstate->buffer, false, true, slot))
				elog(ERROR, "unexpected end of tuplestore");
//#ifdef WIN_FUN_OPT
		}
//#endif

		winobj->seekpos--;
	}

	while (winobj->seekpos < pos)
	{
//#ifdef WIN_FUN_OPT
		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			if(!opt_tuplestore_gettupleslot(winstate->buffer, true, true, opt_slot))
				elog(ERROR, "unexpected end of tuplestore");
		}else{
//#endif
			if(!tuplestore_gettupleslot(winstate->buffer, true, true, slot))
				elog(ERROR, "unexpected end of tuplestore");
//#ifdef WIN_FUN_OPT
		}
//#endif
		winobj->seekpos++;
	}

	MemoryContextSwitchTo(oldcontext);

	return true;
}

static void opt_update_frameheadpos(WindowObject winobj, TupleTableSlot *slot, TupleTableSlot *opt_slot){
	WindowAggState *winstate = winobj->winstate;
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;

	if (winstate->framehead_valid)
		return;					/* already known for current row */

	if (frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
	{
		/* In UNBOUNDED PRECEDING mode, frame head is always row 0 */
		winstate->frameheadpos = 0;
		winstate->framehead_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_START_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, frame head is the same as current */
			winstate->frameheadpos = winstate->currentpos;
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			int64		fhprev;

			/* If no ORDER BY, all rows are peers with each other */
			if (node->ordNumCols == 0)
			{
				winstate->frameheadpos = 0;
				winstate->framehead_valid = true;
				return;
			}

			/*
			 * In RANGE START_CURRENT mode, frame head is the first row that
			 * is a peer of current row.  We search backwards from current,
			 * which could be a bit inefficient if peer sets are large. Might
			 * be better to have a separate read pointer that moves forward
			 * tracking the frame head.
			 */
			fhprev = winstate->currentpos - 1;
			for (;;)
			{
				/* assume the frame head can't go backwards */
				if (fhprev < winstate->frameheadpos)
					break;
//#ifdef WIN_FUN_OPT
				if(enable_winfunopt){
					if (!opt_window_gettupleslot(winobj, fhprev, slot, opt_slot))
						break;		/* start of partition */
					/*
					 * opt_window_gettupleslot may change status,
					 * slot or opt_slot stores the result depending on the status
					 */
					if(tuplestore_in_memory(winstate->buffer)){
						if (!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
							break; /* not peer of current row */
					}else{
						if (!are_peers(winstate, opt_slot, winstate->opt_scantupslot))
							break;
					}
				}else{
//#else
					if (!window_gettupleslot(winobj, fhprev, slot))
						break;		/* start of partition */
					if (!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
						break;		/* not peer of current row */
				}
//#endif
				fhprev--;
			}
			winstate->frameheadpos = fhprev + 1;
			winstate->framehead_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_START_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, bound is physically n before/after current */
			int64		offset = DatumGetInt64(winstate->startOffsetValue);

			if (frameOptions & FRAMEOPTION_START_VALUE_PRECEDING)
				offset = -offset;

			winstate->frameheadpos = winstate->currentpos + offset;
			/* frame head can't go before first row */
			if (winstate->frameheadpos < 0)
				winstate->frameheadpos = 0;
			else if (winstate->frameheadpos > winstate->currentpos)
			{
				/* make sure frameheadpos is not past end of partition */
				spool_tuples(winstate, winstate->frameheadpos - 1);
				if (winstate->frameheadpos > winstate->spooled_rows)
					winstate->frameheadpos = winstate->spooled_rows;
			}
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}
	else
		Assert(false);
}
static void opt_update_frametailpos(WindowObject winobj, TupleTableSlot *slot, TupleTableSlot *opt_slot){
	WindowAggState *winstate = winobj->winstate;
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;

	if (winstate->frametail_valid)
		return;					/* already known for current row */

	if (frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING)
	{
		/* In UNBOUNDED FOLLOWING mode, all partition rows are in frame */
		spool_tuples(winstate, -1);
		winstate->frametailpos = winstate->spooled_rows - 1;
		winstate->frametail_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_END_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, exactly the rows up to current are in frame */
			winstate->frametailpos = winstate->currentpos;
			winstate->frametail_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			int64		ftnext;

			/* If no ORDER BY, all rows are peers with each other */
			if (node->ordNumCols == 0)
			{
				spool_tuples(winstate, -1);
				winstate->frametailpos = winstate->spooled_rows - 1;
				winstate->frametail_valid = true;
				return;
			}

			/*
			 * Else we have to search for the first non-peer of the current
			 * row.  We assume the current value of frametailpos is a lower
			 * bound on the possible frame tail location, ie, frame tail never
			 * goes backward, and that currentpos is also a lower bound, ie,
			 * frame end always >= current row.
			 */
			ftnext = Max(winstate->frametailpos, winstate->currentpos) + 1;
			for (;;)
			{
//#ifdef WIN_FUN_OPT
				if(enable_winfunopt){
					if(!opt_window_gettupleslot(winobj, ftnext, slot, opt_slot))
						break; /* end of partition */
					if(tuplestore_in_memory(winstate->buffer)){
						if(!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
							break; /* not peer of current row */
					}else{
						if(!are_peers(winstate, opt_slot, winstate->opt_scantupslot))
							break;
					}
				}else{
//#else
					if (!window_gettupleslot(winobj, ftnext, slot))
						break;		/* end of partition */
					if (!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
						break;		/* not peer of current row */
				}
//#endif

				ftnext++;
			}
			winstate->frametailpos = ftnext - 1;
			winstate->frametail_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_END_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, bound is physically n before/after current */
			int64		offset = DatumGetInt64(winstate->endOffsetValue);

			if (frameOptions & FRAMEOPTION_END_VALUE_PRECEDING)
				offset = -offset;

			winstate->frametailpos = winstate->currentpos + offset;
			/* smallest allowable value of frametailpos is -1 */
			if (winstate->frametailpos < 0)
				winstate->frametailpos = -1;
			else if (winstate->frametailpos > winstate->currentpos)
			{
				/* make sure frametailpos is not past last row of partition */
				spool_tuples(winstate, winstate->frametailpos);
				if (winstate->frametailpos >= winstate->spooled_rows)
					winstate->frametailpos = winstate->spooled_rows - 1;
			}
			winstate->frametail_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}
	else
		Assert(false);
}

bool
opt_WinRowsArePeers(WindowObject winobj, int64 pos1, int64 pos2)
{
	WindowAggState *winstate;
	WindowAgg  *node;
	TupleTableSlot *slot1;
	TupleTableSlot *slot2;
	bool		res;

//#ifdef WIN_FUN_OPT
	TupleTableSlot *opt_slot1;
	TupleTableSlot *opt_slot2;
//#endif

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	node = (WindowAgg *) winstate->ss.ps.plan;

	/* If no ORDER BY, all rows are peers; don't bother to fetch them */
	if (node->ordNumCols == 0)
		return true;

	slot1 = winstate->temp_slot_1;
	slot2 = winstate->temp_slot_2;

//#ifdef WIN_FUN_OPT
	if(enable_winfunopt){
		opt_slot1 = winstate->opt_temp_slot_1;
		opt_slot2 = winstate->opt_temp_slot_2;
		/*
		 * currently, the caller of this function only compare the slot as position of current and current-1,
		 * so the result will in the same status.
		 */
		if (!opt_window_gettupleslot(winobj, pos1, slot1, opt_slot1))
			elog(ERROR, "specified position is out of window: " INT64_FORMAT,
				 pos1);
		if (!opt_window_gettupleslot(winobj, pos2, slot2, opt_slot2))
			elog(ERROR, "specified position is out of window: " INT64_FORMAT,
				 pos2);

		if(tuplestore_in_memory(winstate->buffer)){
			res = are_peers(winstate, slot1, slot2);
			ExecClearTuple(slot1);
			ExecClearTuple(slot2);
		}else{
			res = are_peers(winstate, opt_slot1, opt_slot2);
			ExecClearTuple(opt_slot1);
			ExecClearTuple(opt_slot2);
		}
	}else{
//#else
		if (!window_gettupleslot(winobj, pos1, slot1))
			elog(ERROR, "specified position is out of window: " INT64_FORMAT,
				 pos1);
		if (!window_gettupleslot(winobj, pos2, slot2))
			elog(ERROR, "specified position is out of window: " INT64_FORMAT,
				 pos2);

		res = are_peers(winstate, slot1, slot2);

		ExecClearTuple(slot1);
		ExecClearTuple(slot2);
	}
//#endif

	return res;
}

void opt_update_frameheadptr(WindowObject winobj, int64 frameheadpos){
	WindowAggState *winstate;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer))
		opt_tuplestore_select_read_pointer(winstate->buffer, winobj->opt_frameheadptr);
	else
		tuplestore_select_read_pointer(winstate->buffer, winobj->opt_frameheadptr);

	/*
	 * NOTE: frameheadptr is not used to fetch tuple,
	 * so we use frameheadpos-1 instead of frameheadpos here.
	 */
	while (frameheadpos-1 > winobj->opt_frameheadpos)
	{
		if(enable_winfunopt)
			opt_tuplestore_advance(winstate->buffer, true);
		else
			tuplestore_advance(winstate->buffer, true);
		winobj->opt_frameheadpos++;
	}
}

void opt_copy_frameheadptr(WindowObject winobj){
	WindowAggState *winstate;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	/*
	 * Do we really need to call tuplestore_select_read_pointer() here?
	 *
	 * In memory, the read pointer's current is updated when getting a tuple slot, there is no need.
	 * However, when on tape, we need to save the status of the pre-active pointer,
	 * that's what tuplestore_select_read_pointer can do.
	 */
	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer))
		opt_tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	else
		tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	winobj->seekpos = winobj->opt_frameheadpos;
	opt_tuplestore_copy_ptr(winstate->buffer, winobj->readptr, winobj->opt_frameheadptr);

	/*
	 * as we have called tuplestore_select_read_pointer,
	 * we must seek directly to make the new readptr work properly.
	 */
	opt_tuplestore_seek_ptr(winstate->buffer, winobj->readptr);
}

/*
 * Compute the temporary transition value
 */
void opt_temp_advance_windowaggregate(WindowAggState *winstate,
						WindowStatePerFunc perfuncstate,
						WindowStatePerAgg peraggstate,
						int target)
{
	WindowFuncExprState *wfuncstate = perfuncstate->wfuncstate;
	int			numArguments = perfuncstate->numArguments;
	FunctionCallInfoData fcinfodata;
	FunctionCallInfo fcinfo = &fcinfodata;
	Datum		newVal;
	ListCell   *arg;
	int			i;
	MemoryContext oldContext;
	ExprContext *econtext = winstate->tmpcontext;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* We start from 1, since the 0th arg will be the transition value */
	i = 1;
	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
		foreach(arg, wfuncstate->opt_args){
			ExprState  *argstate = (ExprState *) lfirst(arg);

			fcinfo->arg[i] = ExecEvalExpr(argstate, econtext,
										  &fcinfo->argnull[i], NULL);
			i++;
		}
	}else{
		foreach(arg, wfuncstate->args)
		{
			ExprState  *argstate = (ExprState *) lfirst(arg);

			fcinfo->arg[i] = ExecEvalExpr(argstate, econtext,
										  &fcinfo->argnull[i], NULL);
			i++;
		}
	}

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->argnull[i])
			{
				MemoryContextSwitchTo(oldContext);
				return;
			}
		}
		//if (peraggstate->noTransValue)
		if(peraggstate->opt_temp_noTransValue[target])
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into aggcontext if it is pass-by-ref. We
			 * do not need to pfree the old transValue, since it's NULL.
			 */
			/*
			MemoryContextSwitchTo(winstate->aggcontext);
			peraggstate->transValue = datumCopy(fcinfo->arg[1],
												peraggstate->transtypeByVal,
												peraggstate->transtypeLen);
			peraggstate->transValueIsNull = false;
			peraggstate->noTransValue = false;
			*/

			/*
			 * remember to switch, that's why bug appears!!!!!!!!!!!!!!!!!!
			 */
			MemoryContextSwitchTo(winstate->aggcontext);
			peraggstate->opt_temp_transValue[target] = datumCopy(fcinfo->arg[1],
													peraggstate->transtypeByVal,
													peraggstate->transtypeLen);
			peraggstate->opt_temp_transValueIsNull[target] = false;
			peraggstate->opt_temp_noTransValue[target] = false;

			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (peraggstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			MemoryContextSwitchTo(oldContext);
			return;
		}
	}

	/*
	 * OK to call the transition function
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn),
							 numArguments + 1,
							 perfuncstate->winCollation,
							 (void *) winstate, NULL);

	fcinfo->arg[0] = peraggstate->opt_temp_transValue[target];
	fcinfo->argnull[0] = peraggstate->opt_temp_transValueIsNull[target];

	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.	But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(peraggstate->opt_temp_transValue[target]))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(winstate->aggcontext);
			newVal = datumCopy(newVal,
							   peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		if (!peraggstate->opt_temp_transValueIsNull[target])
			pfree(DatumGetPointer(peraggstate->opt_temp_transValue[target]));
	}

	MemoryContextSwitchTo(oldContext);
	peraggstate->opt_temp_transValue[target] = newVal;
	peraggstate->opt_temp_transValueIsNull[target] = fcinfo->isnull;
}

/*
 * Combine two transition values
 *
 * Similar to advance_windowaggregate() except that the opt_temp_transValue will be the only argument
 *
 * NOTE: not used now, because different function has different combine strategy
 */
static void opt_combine_tranValues(WindowAggState *winstate,
		WindowStatePerFunc perfuncstate,
		WindowStatePerAgg peraggstate)
{
	//int			numArguments = perfuncstate->numArguments;
	int			numArguments = 1;
	FunctionCallInfoData fcinfodata;
	FunctionCallInfo fcinfo = &fcinfodata;
	Datum		newVal;
	int			i;
	MemoryContext oldContext;
	ExprContext *econtext = winstate->tmpcontext;

	/* if the temporary transition value is null, we do nothing*/
	//if (peraggstate->transfn.fn_strict && peraggstate->opt_temp_transValueIsNull)
	//	return;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* We start from 1, since the 0th arg will be the transition value */
	fcinfo->arg[1] = peraggstate->opt_temp_transValue[0];
	fcinfo->argnull[1] = peraggstate->opt_temp_transValueIsNull[0];

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->argnull[i])
			{
				MemoryContextSwitchTo(oldContext);
				return;
			}
		}
		if (peraggstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into aggcontext if it is pass-by-ref. We
			 * do not need to pfree the old transValue, since it's NULL.
			 */
			MemoryContextSwitchTo(winstate->aggcontext);
			peraggstate->transValue = datumCopy(fcinfo->arg[1],
												peraggstate->transtypeByVal,
												peraggstate->transtypeLen);
			peraggstate->transValueIsNull = false;
			peraggstate->noTransValue = false;
			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (peraggstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			MemoryContextSwitchTo(oldContext);
			return;
		}
	}

	/*
	 * OK to call the transition function
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn),
							 numArguments + 1,
							 perfuncstate->winCollation,
							 (void *) winstate, NULL);
	fcinfo->arg[0] = peraggstate->transValue;
	fcinfo->argnull[0] = peraggstate->transValueIsNull;
	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.	But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(peraggstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(winstate->aggcontext);
			newVal = datumCopy(newVal,
							   peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		if (!peraggstate->transValueIsNull)
			pfree(DatumGetPointer(peraggstate->transValue));
	}

	MemoryContextSwitchTo(oldContext);
	peraggstate->transValue = newVal;
	peraggstate->transValueIsNull = fcinfo->isnull;
}
/*
 * Initial the temporary transition value
 */
void opt_temp_initialize_windowaggregate(WindowAggState *winstate,
						   WindowStatePerFunc perfuncstate,
						   WindowStatePerAgg peraggstate,
						   int target)
{
	MemoryContext oldContext;

	if(!enable_recompute)
		return;	//just for sake

	if (peraggstate->initValueIsNull)
		peraggstate->opt_temp_transValue[target] = peraggstate->initValue;
	else
	{
		oldContext = MemoryContextSwitchTo(winstate->aggcontext);
		peraggstate->opt_temp_transValue[target] = datumCopy(peraggstate->initValue,
											peraggstate->transtypeByVal,
											peraggstate->transtypeLen);
		MemoryContextSwitchTo(oldContext);
	}
	peraggstate->opt_temp_transValueIsNull[target] = peraggstate->initValueIsNull;
	peraggstate->opt_temp_noTransValue[target] = peraggstate->initValueIsNull;
	//peraggstate->resultValueIsNull = true;
}

void opt_copy_tempTransEndPtr(WindowObject winobj, int target){
	WindowAggState *winstate;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	/*
	 * Do we really need to call tuplestore_select_read_pointer() here?
	 *
	 * In memory, the read pointer's current is updated when getting a tuple slot, there is no need.
	 * However, when on tape, we need to save the status of the pre-active pointer,
	 * that's what tuplestore_select_read_pointer can do.
	 */
	if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer))
		opt_tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	else
		tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	winobj->seekpos = winobj->opt_tempTransEndPos[target];
	opt_tuplestore_copy_ptr(winstate->buffer, winobj->readptr, winobj->opt_tempTransEndPtr[target]);

	/*
	 * as we have called tuplestore_select_read_pointer,
	 * we must seek directly to make the new readptr work properly.
	 */
	opt_tuplestore_seek_ptr(winstate->buffer, winobj->readptr);
}

static void opt_eval_tempTransEnd(WindowAggState *winstate){
	WindowStatePerAgg peraggstate;
	int			wfuncno,
				numaggs;
	int			i;

	WindowObject agg_winobj;
	TupleTableSlot *agg_row_slot;

	TupleTableSlot	*opt_agg_row_slot = winstate->opt_agg_row_slot;

	int target;

	numaggs = winstate->numaggs;
	if (numaggs == 0)
		return;					/* nothing to do */

	/* final output execution is in ps_ExprContext */
	agg_winobj = winstate->agg_winobj;
	agg_row_slot = winstate->agg_row_slot;

	for (;;)
	{
		if(enable_winfunopt){
			/*
			 * opt_window_gettupleslot may change the status
			 */
			if(!tuplestore_in_memory(winstate->buffer)){
				if (TupIsNull(opt_agg_row_slot)){
					if (!opt_window_gettupleslot(agg_winobj, winstate->aggregatedupto, agg_row_slot, opt_agg_row_slot))
						break;			/* must be end of partition */
				}
			}else{
				if (TupIsNull(agg_row_slot))
				{
					if (!opt_window_gettupleslot(agg_winobj, winstate->aggregatedupto, agg_row_slot, opt_agg_row_slot))
						break;			/* must be end of partition */
				}
				/*
				 * this is the situation that the buffer status switch from memory to tape,
				 * as the follow-up computing will for opt, we need to converted it to an opt tuple.
				 */
				if(!tuplestore_in_memory(winstate->buffer)){
					//tuplestore_convert_to_opt(winstate->buffer, agg_row_slot, opt_agg_row_slot);
					/* the initial tuple slot is will not be used in the computation of current row */
					if(agg_row_slot)
						ExecClearTuple(agg_row_slot);
				}
			}
		}else{
			/* Fetch next row if we didn't already */
			if (TupIsNull(agg_row_slot))
			{
				if (!window_gettupleslot(agg_winobj, winstate->aggregatedupto,
										 agg_row_slot))
					break;			/* must be end of partition */
			}
		}


		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			if (!row_is_in_frame(winstate, winstate->aggregatedupto, opt_agg_row_slot)){
				break;
			}
			/* Set tuple context for evaluation of aggregate arguments */
			winstate->tmpcontext->ecxt_outertuple = opt_agg_row_slot;
		}else{
			if (!row_is_in_frame(winstate, winstate->aggregatedupto, agg_row_slot)){
				break;
			}
			/* Set tuple context for evaluation of aggregate arguments */
			winstate->tmpcontext->ecxt_outertuple = agg_row_slot;
		}

		/* for currentpos's use */
		for (i = 0; i < numaggs; i++)
		{
			peraggstate = &winstate->peragg[i];
			wfuncno = peraggstate->wfuncno;
			advance_windowaggregate(winstate,
									&winstate->perfunc[wfuncno],
									peraggstate);
		}
		/* for future use */
		for(target=winstate->opt_active_tempTransValue; target<winstate->opt_tempTransValue_num; target++){
			/* only the temporary transition values that are still useful need to update */
			if(agg_winobj->opt_tempTransEndPos[target]<0)
				break;
			for (i = 0; i < numaggs; i++)
			{
				peraggstate = &winstate->peragg[i];
				wfuncno = peraggstate->wfuncno;
				opt_temp_advance_windowaggregate(winstate,
										&winstate->perfunc[wfuncno],
										peraggstate,
										target);
			}
			/*
			 * save opt_tempTransEndPtr
			 *
			 * we can't do this out this loop,
			 * because at that time the position is not the end position for temporary transition value.
			 */
			opt_tuplestore_copy_ptr(winstate->buffer, agg_winobj->opt_tempTransEndPtr[target], agg_winobj->readptr);
			agg_winobj->opt_tempTransEndPos[target] = agg_winobj->seekpos; /* we must copy the seek position */
			/*
			 * if not in memory, we need to set the bufFile's offset to opt_tempTransEndPtr.
			 *
			 * NOTE: not just for on tape, but also for memory, or the answer will contains error
			 */
			opt_tuplestore_tell_ptr(winstate->buffer, agg_winobj->opt_tempTransEndPtr[target]);
		}

		/* Reset per-input-tuple context after each tuple */
		ResetExprContext(winstate->tmpcontext);

		/* And advance the aggregated-row state */
		winstate->aggregatedupto++;

		if(enable_winfunopt && !tuplestore_in_memory(winstate->buffer)){
			ExecClearTuple(opt_agg_row_slot);
		}else{
			ExecClearTuple(agg_row_slot);
		}
	}
}

void opt_copy_transValue_from_tempTransValue(WindowAggState *winstate,
		   WindowStatePerFunc perfuncstate,
		   WindowStatePerAgg peraggstate,
		   int target)
{
	MemoryContext oldContext;

	if(!enable_recompute)
		return;	//just for sake


	oldContext = MemoryContextSwitchTo(winstate->aggcontext);

	/*
	 * here doesn't call MemoryContextResetAndDeleteChildren,
	 * we need to free the transValue manually first
	 *
	 * NOTE: only when not transtypeByVal, we can free.
	 */
	if(!peraggstate->transtypeByVal && !peraggstate->transValueIsNull){
		pfree(DatumGetPointer(peraggstate->transValue));
		peraggstate->transValueIsNull = peraggstate->initValueIsNull;
	}

	peraggstate->transValue = datumCopy(peraggstate->opt_temp_transValue[target],
										peraggstate->transtypeByVal,
										peraggstate->transtypeLen);

	MemoryContextSwitchTo(oldContext);

	peraggstate->transValueIsNull = peraggstate->opt_temp_transValueIsNull[target];
	peraggstate->noTransValue = peraggstate->opt_temp_transValueIsNull[target];
	peraggstate->resultValueIsNull = true;
}

static bool
reuse_window_gettupleslot(WindowObject winobj, int64 pos, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	MemoryContext oldcontext;

	/* Don't allow passing -1 to spool_tuples here */
	if (pos < 0)
		return false;

	/* If necessary, fetch the tuple into the spool */
	spool_tuples(winstate, pos);

	if (pos >= winstate->spooled_rows)
		return false;

	if (pos < winobj->markpos)
		elog(ERROR, "cannot fetch row before WindowObject's mark position");

	/*
	 * by cywang, for reusing buffer
	 *
	 * check if we can get the tuple slot from the buffer directly when not in memory
	 */
	if(enable_reusebuffer && !tuplestore_in_memory(winstate->buffer)){
		if(reuse_tuplestore_gettupleslot_direct(winstate->buffer, pos, slot))
			return true;
	}

	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);

	/*
	 * There's no API to refetch the tuple at the current position. We have to
	 * move one tuple forward, and then one backward.  (We don't do it the
	 * other way because we might try to fetch the row before our mark, which
	 * isn't allowed.)  XXX this case could stand to be optimized.
	 */
	if (winobj->seekpos == pos)
	{
		tuplestore_advance(winstate->buffer, true);
		winobj->seekpos++;
	}

	while (winobj->seekpos > pos)
	{
		//if (!tuplestore_gettupleslot(winstate->buffer, false, true, slot))
		if (!reuse_tuplestore_gettupleslot(winstate->buffer, false, true, slot, pos))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos--;
	}

	while (winobj->seekpos < pos)
	{
		//if (!tuplestore_gettupleslot(winstate->buffer, true, true, slot))
		if (!reuse_tuplestore_gettupleslot(winstate->buffer, true, true, slot, pos))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos++;
	}

	MemoryContextSwitchTo(oldcontext);

	return true;
}

void copy_reuseptr(WindowObject winobj){
	WindowAggState *winstate;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	winobj->seekpos = reuse_tuplestore_copy_reuseptr(winstate->buffer);

	/*
	 * as we have called tuplestore_select_read_pointer,
	 * we must seek directly to make the new readptr work properly.
	 */
	opt_tuplestore_seek_ptr(winstate->buffer, winobj->readptr);
}

static void
reuse_update_frameheadpos(WindowObject winobj, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;

	if (winstate->framehead_valid)
		return;					/* already known for current row */

	if (frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
	{
		/* In UNBOUNDED PRECEDING mode, frame head is always row 0 */
		winstate->frameheadpos = 0;
		winstate->framehead_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_START_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, frame head is the same as current */
			winstate->frameheadpos = winstate->currentpos;
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			int64		fhprev;

			/* If no ORDER BY, all rows are peers with each other */
			if (node->ordNumCols == 0)
			{
				winstate->frameheadpos = 0;
				winstate->framehead_valid = true;
				return;
			}

			/*
			 * In RANGE START_CURRENT mode, frame head is the first row that
			 * is a peer of current row.  We search backwards from current,
			 * which could be a bit inefficient if peer sets are large. Might
			 * be better to have a separate read pointer that moves forward
			 * tracking the frame head.
			 */
			fhprev = winstate->currentpos - 1;
			for (;;)
			{
				/* assume the frame head can't go backwards */
				if (fhprev < winstate->frameheadpos)
					break;
				if(!tuplestore_in_memory(winstate->buffer)){
					if (!reuse_window_gettupleslot(winobj, fhprev, slot))
						break;		/* start of partition */
				}else{
					if (!window_gettupleslot(winobj, fhprev, slot))
						break;		/* start of partition */
				}
				if (!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
					break;		/* not peer of current row */
				fhprev--;
			}
			winstate->frameheadpos = fhprev + 1;
			winstate->framehead_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_START_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, bound is physically n before/after current */
			int64		offset = DatumGetInt64(winstate->startOffsetValue);

			if (frameOptions & FRAMEOPTION_START_VALUE_PRECEDING)
				offset = -offset;

			winstate->frameheadpos = winstate->currentpos + offset;
			/* frame head can't go before first row */
			if (winstate->frameheadpos < 0)
				winstate->frameheadpos = 0;
			else if (winstate->frameheadpos > winstate->currentpos)
			{
				/* make sure frameheadpos is not past end of partition */
				spool_tuples(winstate, winstate->frameheadpos - 1);
				if (winstate->frameheadpos > winstate->spooled_rows)
					winstate->frameheadpos = winstate->spooled_rows;
			}
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}
	else
		Assert(false);
}
//#endif
