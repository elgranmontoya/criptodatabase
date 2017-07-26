#include <postgres.h>

#include <utils/rel.h>
#include <utils/rls.h>
#include <utils/lsyscache.h>
#include <utils/guc.h>
#include <access/xact.h>
#include <miscadmin.h>

#include "errors.h"
#include "chunk_insert_state.h"

/*
 * Create a new RangeTblEntry for the chunk in the executor's range table and
 * return the index.
 */
static inline Index
create_chunk_range_table_entry(EState *estate, Relation rel)
{
	RangeTblEntry *rte;
	int			length = list_length(estate->es_range_table);

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->requiredPerms = ACL_INSERT;

	if (length == 1)
		estate->es_range_table = list_copy(estate->es_range_table);

	estate->es_range_table = lappend(estate->es_range_table, rte);

	return length + 1;
}

/*
 * Create a new ResultRelInfo for a chunk.
 *
 * The ResultRelInfo holds the executor state (e.g., open relation, indexes, and
 * options) for the result relation where tuples will be stored.
 *
 * The first ResultRelInfo in the executor's array (corresponding to the main
 * table's) is used as a template for the chunk's new ResultRelInfo.
 */
static inline ResultRelInfo *
create_chunk_result_relation_info(EState *estate, Relation rel, Index rti)
{
	ResultRelInfo *rri,
			   *rri_orig;

	rri = palloc(sizeof(ResultRelInfo));
	MemSet(rri, 0, sizeof(ResultRelInfo));
	NodeSetTag(rri, T_ResultRelInfo);

	InitResultRelInfo(rri, rel, rti, 0);

	/*
	 * Copy options from the first result relation. This should be safe,
	 * because initially there should be one result relation info for the main
	 * table, and the following ones are for chunks that we append based on
	 * the info in the main table.
	 */
	rri_orig = &estate->es_result_relations[0];
	rri->ri_WithCheckOptions = rri_orig->ri_WithCheckOptions;
	rri->ri_WithCheckOptionExprs = rri_orig->ri_WithCheckOptionExprs;
	rri->ri_junkFilter = rri_orig->ri_junkFilter;
	rri->ri_projectReturning = rri_orig->ri_projectReturning;
	rri->ri_onConflictSetProj = rri_orig->ri_onConflictSetProj;
	rri->ri_onConflictSetWhere = rri_orig->ri_onConflictSetWhere;

	return rri;
}

/*
 * Create new insert chunk state.
 *
 * This is essentially a ResultRelInfo for a chunk. Initialization of the
 * ResultRelInfo should be similar to ExecInitModifyTable().
 */
extern ChunkInsertState *
chunk_insert_state_create(Chunk *chunk, EState *estate)
{
	ChunkInsertState *state;
	Relation	rel;
	Index		rti;
	MemoryContext old_mcxt;

	/* permissions NOT checked here; were checked at hypertable level */
	if (check_enable_rls(chunk->table_id, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Hypertables don't support row-level security")));

	/* Switch to executor's per-query context */
	old_mcxt = MemoryContextSwitchTo(estate->es_query_cxt);

	rel = heap_open(chunk->table_id, RowExclusiveLock);
	CheckValidResultRel(rel, CMD_INSERT);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "insert is not on a table");

	rti = create_chunk_range_table_entry(estate, rel);

	state = palloc0(sizeof(ChunkInsertState));
	state->chunk = chunk;
	state->rel = rel;
	state->result_relation_info = create_chunk_result_relation_info(estate, rel, rti);

	if (state->result_relation_info->ri_RelationDesc->rd_rel->relhasindex &&
		state->result_relation_info->ri_IndexRelationDescs == NULL)
		ExecOpenIndices(state->result_relation_info,
					 false /* mtstate->mt_onconflict != ONCONFLICT_NONE */ );

	if (state->result_relation_info->ri_TrigDesc != NULL)
	{
		if (state->result_relation_info->ri_TrigDesc->trig_insert_instead_row ||
			state->result_relation_info->ri_TrigDesc->trig_insert_after_statement ||
			state->result_relation_info->ri_TrigDesc->trig_insert_before_statement)
			elog(ERROR, "Insert trigger on chunk table not supported");
	}

	MemoryContextSwitchTo(old_mcxt);

	return state;
}

extern void
chunk_insert_state_destroy(ChunkInsertState *state)
{
	if (state == NULL)
		return;

	ExecCloseIndices(state->result_relation_info);
	heap_close(state->rel, NoLock);
}