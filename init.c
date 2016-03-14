#include "postgres.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "utils/tqual.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "storage/bufmgr.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_operator.h"
#include "commands/explain.h"
#include "commands/defrem.h"
#include "funcapi.h"

#include "access/htup_details.h"
#include "access/heapam.h"

#include "point.h"
#include "crossmatch.h"

extern void _PG_init(void);

static set_join_pathlist_hook_type set_join_pathlist_next;

typedef struct
{
	CustomPath		cpath;

	JoinType		jointype;

	Path		   *outer_path;
	Oid				outer_idx;
	Oid				outer_rel;

	Path		   *inner_path;
	Oid				inner_idx;
	Oid				inner_rel;

	List		   *joinrestrictinfo;

	float8			threshold;
} CrossmatchJoinPath;

typedef struct
{
	CustomScanState css;

	Datum		   *values;
	bool		   *nulls;
	HeapTuple		stored_tuple;

	List		   *scan_tlist;

	Index			outer_relid;
	Oid				outer_idx;
	Oid				outer_rel;
	Relation		outer;

	Index			inner_relid;
	Oid				inner_idx;
	Oid				inner_rel;
	Relation		inner;

	float8			threshold;

	CrossmatchContext *ctx;
} CrossmatchScanState;

static CustomPathMethods	crossmatch_path_methods;
static CustomScanMethods	crossmatch_plan_methods;
static CustomExecMethods	crossmatch_exec_methods;


#define IsVarSpointDist(arg, dist_func_oid) 				\
	(														\
		IsA(arg, FuncExpr) &&								\
		((FuncExpr *) (arg))->funcid == (dist_func_oid) &&	\
		IsA(linitial(((FuncExpr *) (arg))->args), Var) &&	\
		IsA(lsecond(((FuncExpr *) (arg))->args), Var)		\
	)


static inline int64
get_index_size(Oid idx)
{
	Datum size = DirectFunctionCall2(pg_relation_size,
									 ObjectIdGetDatum(idx),
									 PointerGetDatum(cstring_to_text("main")));
	return DatumGetInt64(size);
}

static inline Oid
get_dist_func()
{
	text *dist_func_name =
		cstring_to_text("public.dist(public.spoint, public.spoint)");

	return DatumGetObjectId(DirectFunctionCall1(to_regprocedure,
												PointerGetDatum(dist_func_name)));
}

static float8
cstring_to_float8(char *str)
{
	return DatumGetFloat8(DirectFunctionCall1(float8in,
											  CStringGetDatum(str)));
}

static char *
float8_to_cstring(float8 val)
{
	return DatumGetCString(DirectFunctionCall1(float8out,
											   Float8GetDatum(val)));
}

static float8
get_const_val(Const *node)
{
	FmgrInfo	finfo;
	Oid			cast;

	Assert(IsA(node, Const));

	if (node->consttype == FLOAT8OID)
		return DatumGetFloat8(node->constvalue);

	cast = get_cast_oid(node->consttype, FLOAT8OID, false);
	fmgr_info(cast, &finfo);

	return DatumGetFloat8(FunctionCall1(&finfo, node->constvalue));
}

/*
 * TODO: check for the predicates & decide
 * whether some partial indices may suffice
 */
static Oid
pick_suitable_index(Oid relation, AttrNumber column)
{
	Oid				found_index = InvalidOid;
	int64			found_index_size = 0;
	HeapTuple		htup;
	SysScanDesc		scan;
	Relation		pg_index;
	List		   *spoint2_opclass_name;
	Oid				spoint2_opclass;
	ScanKeyData		key[3];

	spoint2_opclass_name = stringToQualifiedNameList("public.spoint2");
	spoint2_opclass = get_opclass_oid(GIST_AM_OID, spoint2_opclass_name, false);

	ScanKeyInit(&key[0],
				Anum_pg_index_indrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relation));

	pg_index = heap_open(IndexRelationId, AccessShareLock);
	scan = systable_beginscan(pg_index, InvalidOid, false, NULL, 1, key);

	while (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		Form_pg_index	pg_ind = (Form_pg_index) GETSTRUCT(htup);
		Relation		index;
		Oid				index_am;

		index = index_open(pg_ind->indexrelid, AccessShareLock);
		index_am = index->rd_rel->relam;
		index_close(index, AccessShareLock);

		/* check if this is a valid GIST index with no predicates */
		if (index_am == GIST_AM_OID && pg_ind->indisvalid &&
			heap_attisnull(htup, Anum_pg_index_indpred))
		{
			int i;

			for (i = 0; i < pg_ind->indkey.dim1; i++)
			{
				int64 cur_index_size = 0;

				if (pg_ind->indkey.values[i] == column)
				{
					cur_index_size = get_index_size(pg_ind->indexrelid);

					if (found_index == InvalidOid || cur_index_size < found_index_size)
					{
						bool		is_null;
						Datum		indclass = heap_getattr(htup, Anum_pg_index_indclass,
															pg_index->rd_att, &is_null);
						oidvector  *indclasses = (oidvector *) DatumGetPointer(indclass);

						/* column must use 'spoint2' opclass */
						if (!is_null && indclasses->values[i] == spoint2_opclass)
							found_index = pg_ind->indexrelid;
					}

					break;	/* no need to scan 'indkey' further */
				}
			}
		}
	}

	systable_endscan(scan);
	heap_close(pg_index, AccessShareLock);

	return found_index;
}

static void
get_spoint_attnums(FuncExpr *fexpr, RelOptInfo *outer, RelOptInfo *inner,
				   AttrNumber *outer_spoint, AttrNumber *inner_spoint)
{
	ListCell *dist_arg;

	Assert(outer->relid != 0 && inner->relid != 0);

	foreach(dist_arg, fexpr->args)
	{
		Var *arg = (Var *) lfirst(dist_arg);

		if (arg->varno == outer->relid)
			*outer_spoint = arg->varoattno;

		if (arg->varno == inner->relid)
			*inner_spoint = arg->varoattno;
	}
}

static Path *
crossmatch_find_cheapest_path(PlannerInfo *root,
							  RelOptInfo *joinrel,
							  RelOptInfo *inputrel)
{
	Path	   *input_path = inputrel->cheapest_total_path;
	Relids		other_relids;
	ListCell   *lc;

	other_relids = bms_difference(joinrel->relids, inputrel->relids);
	if (bms_overlap(PATH_REQ_OUTER(input_path), other_relids))
	{
		input_path = NULL;
		foreach (lc, inputrel->pathlist)
		{
			Path   *curr_path = lfirst(lc);

			if (bms_overlap(PATH_REQ_OUTER(curr_path), other_relids))
				continue;
			if (input_path == NULL ||
				input_path->total_cost > curr_path->total_cost)
				input_path = curr_path;
		}
	}
	bms_free(other_relids);

	return input_path;
}

static void
create_crossmatch_path(PlannerInfo *root,
					   RelOptInfo *joinrel,
					   Path *outer_path,
					   Path *inner_path,
					   ParamPathInfo *param_info,
					   List *restrict_clauses,
					   Relids required_outer,
					   float8 threshold,
					   AttrNumber outer_spoint,
					   AttrNumber inner_spoint)
{
	CrossmatchJoinPath *result;

	Oid outer_rel = root->simple_rte_array[outer_path->parent->relid]->relid;
	Oid inner_rel = root->simple_rte_array[inner_path->parent->relid]->relid;
	Oid outer_idx;
	Oid inner_idx;

	if (outer_rel == inner_rel)
		return;

	if ((outer_idx = pick_suitable_index(outer_rel, outer_spoint)) == InvalidOid ||
		(inner_idx = pick_suitable_index(inner_rel, inner_spoint)) == InvalidOid)
	{
		return;
	}

	result = palloc0(sizeof(CrossmatchJoinPath));
	NodeSetTag(result, T_CustomPath);

	result->cpath.path.pathtype = T_CustomScan;
	result->cpath.path.parent = joinrel;
	result->cpath.path.param_info = param_info;
	result->cpath.path.pathkeys = NIL;
	result->cpath.path.pathtarget = &joinrel->reltarget;
	result->cpath.path.rows = joinrel->rows;
	result->cpath.flags = 0;
	result->cpath.methods = &crossmatch_path_methods;
	result->outer_path = outer_path;
	result->outer_idx = outer_idx;
	result->outer_rel = outer_rel;
	result->inner_path = inner_path;
	result->inner_idx = inner_idx;
	result->inner_rel = inner_rel;
	result->threshold = threshold;
	result->joinrestrictinfo = restrict_clauses;

	/* TODO: real costs */
	result->cpath.path.startup_cost = 0;
	result->cpath.path.total_cost = 1;

	add_path(joinrel, &result->cpath.path);
}

static void
try_crossmatch_path(RestrictInfo *restrInfo,
					FuncExpr *distFuncExpr,
					Const *thresholdConst,
					PlannerInfo *root,
					RelOptInfo *joinrel,
					RelOptInfo *outerrel,
					RelOptInfo *innerrel,
					JoinPathExtraData *extra)
{
	AttrNumber		outer_spoint,
					inner_spoint;
	List		   *restrict_clauses;
	Path		   *outer_path,
				   *inner_path;
	Relids			required_outer;
	ParamPathInfo  *param_info;

	/* Remove current RestrictInfo from restrict clauses */
	restrict_clauses = list_delete_ptr(list_copy(extra->restrictlist), restrInfo);
	restrict_clauses = list_concat_unique(restrict_clauses,
										  outerrel->baserestrictinfo);
	restrict_clauses = list_concat_unique(restrict_clauses,
										  innerrel->baserestrictinfo);

	outer_path = crossmatch_find_cheapest_path(root, joinrel, outerrel);
	inner_path = crossmatch_find_cheapest_path(root, joinrel, innerrel);

	required_outer = calc_nestloop_required_outer(outer_path, inner_path);

	param_info = get_joinrel_parampathinfo(root,
										   joinrel,
										   outer_path,
										   inner_path,
										   extra->sjinfo,
										   required_outer,
										   &restrict_clauses);

	get_spoint_attnums(distFuncExpr, outerrel, innerrel,
					   &outer_spoint, &inner_spoint);

	create_crossmatch_path(root, joinrel, outer_path, inner_path,
						   param_info, restrict_clauses, required_outer,
						   get_const_val(thresholdConst),
						   outer_spoint, inner_spoint);
}

static void
join_pathlist_hook(PlannerInfo *root,
				   RelOptInfo *joinrel,
				   RelOptInfo *outerrel,
				   RelOptInfo *innerrel,
				   JoinType jointype,
				   JoinPathExtraData *extra)
{
	ListCell   *restr;
	Oid			dist_func;
	Relids		required_relids = NULL;

	if (set_join_pathlist_next)
		set_join_pathlist_next(root, joinrel, outerrel,
							   innerrel, jointype, extra);

	/* Get oid of the dist(spoint, spoint) function */
	if ((dist_func = get_dist_func()) == InvalidOid)
		return;

	if (outerrel->reloptkind == RELOPT_BASEREL &&
		innerrel->reloptkind == RELOPT_BASEREL)
	{
		required_relids = bms_add_member(required_relids, outerrel->relid);
		required_relids = bms_add_member(required_relids, innerrel->relid);
	}
	else return; /* one of relations can't have index */

	foreach(restr, extra->restrictlist)
	{
		RestrictInfo *restrInfo = (RestrictInfo *) lfirst(restr);

		/* Skip irrelevant JOIN case */
		if (!bms_equal(required_relids, restrInfo->required_relids))
			continue;

		if (IsA(restrInfo->clause, OpExpr))
		{
			OpExpr	   *opExpr = (OpExpr *) restrInfo->clause;
			int			nargs = list_length(opExpr->args);
			Node	   *arg1;
			Node	   *arg2;

			if (nargs != 2)
				continue;

			arg1 = linitial(opExpr->args);
			arg2 = lsecond(opExpr->args);

			if (opExpr->opno == Float8LessOperator &&
				IsVarSpointDist(arg1, dist_func) && IsA(arg2, Const))
			{
				try_crossmatch_path(restrInfo, (FuncExpr *) arg1, (Const *) arg2,
									root, joinrel, outerrel, innerrel, extra);
				break;
			}
			else if (opExpr->opno == get_commutator(Float8LessOperator) &&
					 IsA(arg1, Const) && IsVarSpointDist(arg2, dist_func))
			{
				try_crossmatch_path(restrInfo, (FuncExpr *) arg2, (Const *) arg1,
									root, joinrel, outerrel, innerrel, extra);
				break;
			}
		}
	}
}

static Plan *
create_crossmatch_plan(PlannerInfo *root,
					   RelOptInfo *rel,
					   CustomPath *best_path,
					   List *tlist,
					   List *clauses,
					   List *custom_plans)
{
	CrossmatchJoinPath	   *gpath = (CrossmatchJoinPath *) best_path;
	List				   *joinrestrictclauses = gpath->joinrestrictinfo;
	List				   *joinclauses;
	CustomScan			   *cscan;

	Assert(!IS_OUTER_JOIN(gpath->jointype));
	joinclauses = extract_actual_clauses(joinrestrictclauses, false);

	cscan = makeNode(CustomScan);
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = joinclauses;
	cscan->scan.scanrelid = 0;
	cscan->custom_scan_tlist = tlist;	/* tlist of the 'virtual' join rel
										   we'll have to build and scan */

	cscan->flags = best_path->flags;
	cscan->methods = &crossmatch_plan_methods;

	cscan->custom_private = list_make1(list_make4_oid(gpath->outer_idx,
													  gpath->outer_rel,
													  gpath->inner_idx,
													  gpath->inner_rel));

	/* store threshold as cstring */
	cscan->custom_private = lappend(cscan->custom_private,
									makeString(float8_to_cstring(gpath->threshold)));

	cscan->custom_private = lappend(cscan->custom_private,
									makeInteger(gpath->outer_path->parent->relid));

	cscan->custom_private = lappend(cscan->custom_private,
									makeInteger(gpath->inner_path->parent->relid));

	return &cscan->scan.plan;
}

static Node *
crossmatch_create_scan_state(CustomScan *node)
{
	CrossmatchScanState	*scan_state = palloc0(sizeof(CrossmatchScanState));

	NodeSetTag(scan_state, T_CustomScanState);
	scan_state->css.flags = node->flags;
	scan_state->css.methods = &crossmatch_exec_methods;

	/* Save scan tlist for join relation */
	scan_state->scan_tlist = node->custom_scan_tlist;

	scan_state->outer_idx = linitial_oid(linitial(node->custom_private));
	scan_state->outer_rel = lsecond_oid(linitial(node->custom_private));
	scan_state->inner_idx = lthird_oid(linitial(node->custom_private));
	scan_state->inner_rel = lfourth_oid(linitial(node->custom_private));

	scan_state->threshold = cstring_to_float8(strVal(lsecond(node->custom_private)));

	scan_state->outer_relid = intVal(lthird(node->custom_private));
	scan_state->inner_relid = intVal(lfourth(node->custom_private));

	return (Node *) scan_state;
}

static void
crossmatch_begin(CustomScanState *node, EState *estate, int eflags)
{
	CrossmatchScanState	   *scan_state = (CrossmatchScanState *) node;
	CrossmatchContext	   *ctx = (CrossmatchContext *) palloc0(sizeof(CrossmatchContext));
	int						nlist = list_length(scan_state->scan_tlist);

	scan_state->ctx = ctx;
	setupFirstcall(ctx, scan_state->outer_idx,
				   scan_state->inner_idx, scan_state->threshold);

	scan_state->outer = heap_open(scan_state->outer_rel, AccessShareLock);
	scan_state->inner = heap_open(scan_state->inner_rel, AccessShareLock);

	scan_state->values = palloc(sizeof(Datum) * nlist);
	scan_state->nulls = palloc(sizeof(bool) * nlist);

	/* Store blank tuple in case scan tlist is empty */
	if (scan_state->scan_tlist == NIL)
	{
		TupleDesc tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
		scan_state->stored_tuple = heap_form_tuple(tupdesc, NULL, NULL);
	}
	else
		scan_state->stored_tuple = NULL;
}

static TupleTableSlot *
crossmatch_exec(CustomScanState *node)
{
	CrossmatchScanState	   *scan_state = (CrossmatchScanState *) node;
	TupleTableSlot		   *scanSlot = node->ss.ss_ScanTupleSlot;
	HeapTuple				htup = scan_state->stored_tuple;

	for(;;)
	{
		/* Fetch next tid pair */
		if (!node->ss.ps.ps_TupFromTlist)
		{
			Datum			   *values = scan_state->values;
			bool			   *nulls = scan_state->nulls;

			ItemPointerData		p_tids[2] = { 0 };
			HeapTupleData		htup_outer;
			HeapTupleData		htup_inner;
			Buffer				buf1;
			Buffer				buf2;

			crossmatch(scan_state->ctx, p_tids);

			if (!ItemPointerIsValid(&p_tids[0]) || !ItemPointerIsValid(&p_tids[1]))
			{
				return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
			}

			/* We don't have to fetch tuples if scan tlist is empty */
			if (scan_state->scan_tlist != NIL)
			{
				TupleDesc	tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
				bool		htup_outer_ready = false;
				bool		htup_inner_ready = false;
				int			col_index = 0;
				ListCell   *l;

				htup_outer.t_self = p_tids[0];
				htup_inner.t_self = p_tids[1];

				foreach(l, scan_state->scan_tlist)
				{
					TargetEntry *target = (TargetEntry *) lfirst(l);
					Var *var = (Var *) target->expr;

					if (var->varno == scan_state->outer_relid)
					{
						if (!htup_outer_ready)
						{
							htup_outer_ready = true;
							heap_fetch(scan_state->outer, SnapshotSelf,
									   &htup_outer, &buf1, false, NULL);
						}

						values[col_index] = heap_getattr(&htup_outer, var->varattno,
														 scan_state->outer->rd_att,
														 &nulls[col_index]);
					}

					if (var->varno == scan_state->inner_relid)
					{
						if (!htup_inner_ready)
						{
							htup_inner_ready = true;
							heap_fetch(scan_state->inner, SnapshotSelf,
									   &htup_inner, &buf2, false, NULL);
						}

						values[col_index] = heap_getattr(&htup_inner, var->varattno,
														 scan_state->outer->rd_att,
														 &nulls[col_index]);
					}

					col_index++;
				}

				if (htup_outer_ready)
					ReleaseBuffer(buf1);
				if (htup_inner_ready)
					ReleaseBuffer(buf2);

				htup = heap_form_tuple(tupdesc, values, nulls);
				scan_state->stored_tuple = htup;

				/* Fill scanSlot with a new tuple */
				ExecStoreTuple(htup, scanSlot, InvalidBuffer, false);
			}
		}

		if (node->ss.ps.ps_ProjInfo)
		{
			ExprDoneCond isDone;
			TupleTableSlot *resultSlot;

			ResetExprContext(node->ss.ps.ps_ProjInfo->pi_exprContext);

			node->ss.ps.ps_ProjInfo->pi_exprContext->ecxt_scantuple = scanSlot;
			resultSlot = ExecProject(node->ss.ps.ps_ProjInfo, &isDone);

			if (isDone != ExprEndResult)
			{
				node->ss.ps.ps_TupFromTlist = (isDone == ExprMultipleResult);

				/* Check join conditions */
				node->ss.ps.ps_ExprContext->ecxt_scantuple = scanSlot;
				if (ExecQual(node->ss.ps.qual, node->ss.ps.ps_ExprContext, false))
					return resultSlot;
				else
					InstrCountFiltered1(node, 1);
			}
			else
				node->ss.ps.ps_TupFromTlist = false;
		}
		else
		{
			/* Check join conditions */
			node->ss.ps.ps_ExprContext->ecxt_scantuple = scanSlot;
			if (ExecQual(node->ss.ps.qual, node->ss.ps.ps_ExprContext, false))
				return scanSlot;
			else
				InstrCountFiltered1(node, 1);
		}
	}
}

static void
crossmatch_end(CustomScanState *node)
{
	CrossmatchScanState *scan_state = (CrossmatchScanState *) node;

	heap_close(scan_state->outer, AccessShareLock);
	heap_close(scan_state->inner, AccessShareLock);

	endCall(scan_state->ctx);
}

static void
crossmatch_rescan(CustomScanState *node)
{
	/* NOTE: nothing to do here? */
}

static void
crossmatch_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	CrossmatchScanState	   *scan_state = (CrossmatchScanState *) node;
	StringInfoData			str;

	initStringInfo(&str);

	appendStringInfo(&str, "%s",
					 get_rel_name(scan_state->outer_idx));
	ExplainPropertyText("Outer index", str.data, es);

	resetStringInfo(&str);

	appendStringInfo(&str, "%s",
					 get_rel_name(scan_state->inner_idx));
	ExplainPropertyText("Inner index", str.data, es);

	resetStringInfo(&str);

	appendStringInfo(&str, "%s",
					 float8_to_cstring(scan_state->threshold));
	ExplainPropertyText("Threshold", str.data, es);
}

void
_PG_init(void)
{
	elog(LOG, "loading pg_sphere");

	set_join_pathlist_next = set_join_pathlist_hook;
	set_join_pathlist_hook = join_pathlist_hook;

	crossmatch_path_methods.CustomName				= "CrossmatchJoin";
	crossmatch_path_methods.PlanCustomPath			= create_crossmatch_plan;

	crossmatch_plan_methods.CustomName 				= "CrossmatchJoin";
	crossmatch_plan_methods.CreateCustomScanState	= crossmatch_create_scan_state;

	crossmatch_exec_methods.CustomName				= "CrossmatchJoin";
	crossmatch_exec_methods.BeginCustomScan			= crossmatch_begin;
	crossmatch_exec_methods.ExecCustomScan			= crossmatch_exec;
	crossmatch_exec_methods.EndCustomScan			= crossmatch_end;
	crossmatch_exec_methods.ReScanCustomScan		= crossmatch_rescan;
	crossmatch_exec_methods.MarkPosCustomScan		= NULL;
	crossmatch_exec_methods.RestrPosCustomScan		= NULL;
	crossmatch_exec_methods.ExplainCustomScan		= crossmatch_explain;
}