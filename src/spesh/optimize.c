#include "moar.h"

/* This is where the main optimization work on a spesh graph takes place,
 * using facts discovered during analysis. */

/* Obtains facts for an operand. */
static MVMSpeshFacts * get_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand o) {
    return &g->facts[o.reg.orig][o.reg.i];
}

/* Obtains a string constant. */
static MVMString * get_string(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand o) {
    return g->sf->body.cu->body.strings[o.lit_str_idx];
}

/* Adds a value into a spesh slot and returns its index. */
static MVMint16 add_spesh_slot(MVMThreadContext *tc, MVMSpeshGraph *g, MVMCollectable *c) {
    if (g->num_spesh_slots >= g->alloc_spesh_slots) {
        g->alloc_spesh_slots += 8;
        if (g->spesh_slots)
            g->spesh_slots = realloc(g->spesh_slots,
                g->alloc_spesh_slots * sizeof(MVMCollectable *));
        else
            g->spesh_slots = malloc(g->alloc_spesh_slots * sizeof(MVMCollectable *));
    }
    g->spesh_slots[g->num_spesh_slots] = c;
    return g->num_spesh_slots++;
}

/* Performs optimization on a method lookup. If we know the type that we'll
 * be dispatching on, resolve it right off. If not, add a cache. */
static void optimize_method_lookup(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    /* See if we can resolve the method right off due to knowing the type. */
    MVMSpeshFacts *obj_facts = get_facts(tc, g, ins->operands[1]);
    MVMint32 resolved = 0;
    if (obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) {
        /* Try to resolve. */
        MVMString *name = get_string(tc, g, ins->operands[2]);
        MVMObject *meth = MVM_6model_find_method_cache_only(tc, obj_facts->type, name);
        if (meth) {
            /* Could compile-time resolve the method. Add it in a spesh slot
             * and tweak instruction to grab it from there. */
            MVMint16 ss = add_spesh_slot(tc, g, (MVMCollectable *)meth);
            get_facts(tc, g, ins->operands[1])->usages--;
            ins->info = MVM_op_get_op(MVM_OP_sp_getspeshslot);
            ins->operands[1].lit_i16 = ss;
            resolved = 1;
        }
    }

    /* If not, add space to cache a single type/method pair, to save hash
     * lookups in the (common) monomorphic case, and rewrite to caching
     * version of the instruction. */
    if (!resolved) {
        MVMSpeshOperand *orig_o = ins->operands;
        ins->info = MVM_op_get_op(MVM_OP_sp_findmeth);
        ins->operands = MVM_spesh_alloc(tc, g, 4 * sizeof(MVMSpeshOperand));
        memcpy(ins->operands, orig_o, 3 * sizeof(MVMSpeshOperand));
        ins->operands[3].lit_i16 = add_spesh_slot(tc, g, NULL);
        add_spesh_slot(tc, g, NULL);
    }
}

/* Sees if we can resolve an istype at compile time. */
static void optimize_istype(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *obj_facts  = get_facts(tc, g, ins->operands[1]);
    MVMSpeshFacts *type_facts = get_facts(tc, g, ins->operands[2]);
    MVMSpeshFacts *result_facts;

    if (type_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE &&
         obj_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) {
        MVMint32 result;
        if (!MVM_6model_try_cache_type_check(tc, obj_facts->type, type_facts->type, &result))
            return;
        ins->info = MVM_op_get_op(MVM_OP_const_i64);
        result_facts = get_facts(tc, g, ins->operands[0]);
        result_facts->flags |= MVM_SPESH_FACT_KNOWN_VALUE;
        ins->operands[1].lit_i64 = result;
        result_facts->value.i64  = result;
        obj_facts->usages--;
        type_facts->usages--;
    }
}

/* using the set op with a register we know the value of should
 * propagate that knowledge */
static void optimize_set(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *dst_facts = get_facts(tc, g, ins->operands[0]);
    MVMSpeshFacts *src_facts = get_facts(tc, g, ins->operands[1]);

    if (src_facts->flags & MVM_SPESH_FACT_KNOWN_TYPE) {
        dst_facts->flags |= MVM_SPESH_FACT_KNOWN_TYPE;
        dst_facts->type = src_facts->type;
    }
    if (src_facts->flags & MVM_SPESH_FACT_KNOWN_VALUE) {
        dst_facts->flags |= MVM_SPESH_FACT_KNOWN_VALUE;
        dst_facts->value = src_facts->value;
    }
}

/* iffy ops that operate on a known value register can turn into goto
 * or be dropped. */
static void optimize_iffy(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins, MVMSpeshBB *bb) {
    MVMSpeshFacts *flag_facts = get_facts(tc, g, ins->operands[0]);
    MVMuint8 negated_op;
    MVMuint8 truthvalue;

    switch (ins->info->opcode) {
        case MVM_OP_if_i:
        case MVM_OP_if_s:
        case MVM_OP_if_n:
        case MVM_OP_if_o:
            negated_op = 0;
            break;
        case MVM_OP_unless_i:
        case MVM_OP_unless_s:
        case MVM_OP_unless_n:
        case MVM_OP_unless_o:
            negated_op = 1;
            break;
        default:
            return;
    }

    if (flag_facts->flags & MVM_SPESH_FACT_KNOWN_VALUE) {
        switch (ins->info->opcode) {
            case MVM_OP_if_i:
            case MVM_OP_unless_i:
                truthvalue = flag_facts->value.i64;
                break;
            default:
                return;
        }
    } else {
        return;
    }

    if (truthvalue != negated_op) {
        /* this conditional can be turned into an unconditional jump */
        ins->info = MVM_op_get_op(MVM_OP_goto);
        ins->operands[0] = ins->operands[1];

        /* since we have an unconditional jump now, we can remove the successor
         * that's in the linear_next */
        MVM_spesh_manipulate_remove_successor(tc, bb, bb->linear_next);
    } else {
        /* this conditional can be dropped completely */
        MVM_spesh_manipulate_remove_successor(tc, bb, ins->operands[1].ins_bb);
        MVM_spesh_manipulate_delete_ins(tc, bb, ins);
    }
}

/* Turns a decont into a set, if we know it's not needed. */
static void optimize_decont(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *obj_facts = get_facts(tc, g, ins->operands[1]);
    if (obj_facts->flags & (MVM_SPESH_FACT_DECONTED | MVM_SPESH_FACT_TYPEOBJ))
        ins->info = MVM_op_get_op(MVM_OP_set);
}

/* Visits the blocks in dominator tree order, recursively. */
static void optimize_bb(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb) {
    MVMint32 i;

    /* Look for instructions that are interesting to optimize. */
    MVMSpeshIns *ins = bb->first_ins;
    while (ins) {
        switch (ins->info->opcode) {
        case MVM_OP_findmeth:
            optimize_method_lookup(tc, g, ins);
            break;
        case MVM_OP_decont:
            optimize_decont(tc, g, ins);
            break;
        case MVM_OP_istype:
            optimize_istype(tc, g, ins);
            break;
        case MVM_OP_set:
            optimize_set(tc, g, ins);
            break;
        case MVM_OP_if_i:
        case MVM_OP_unless_i:
            optimize_iffy(tc, g, ins, bb);
            break;
        }
        ins = ins->next;
    }

    /* Visit children. */
    for (i = 0; i < bb->num_children; i++)
        optimize_bb(tc, g, bb->children[i]);

    /* Now walk backwards through the instructions, eliminating any that are
     * pure and unused. */
    ins = bb->last_ins;
    while (ins) {
        MVMSpeshIns *prev = ins->prev;
        if (ins->info->opcode == MVM_SSA_PHI) {
            MVMSpeshFacts *facts = get_facts(tc, g, ins->operands[0]);
            if (facts->usages == 0) {
                /* Propagate non-usage. */
                for (i = 1; i < ins->info->num_operands; i++)
                    get_facts(tc, g, ins->operands[i])->usages--;

                /* Remove this phi. */
                MVM_spesh_manipulate_delete_ins(tc, bb, ins);
            }
        }
        else if (ins->info->pure) {
            /* Sanity check to make sure it's a write reg as first operand. */
            if ((ins->info->operands[0] & MVM_operand_rw_mask) == MVM_operand_write_reg) {
                MVMSpeshFacts *facts = get_facts(tc, g, ins->operands[0]);
                if (facts->usages == 0) {
                    /* Propagate non-usage. */
                    for (i = 1; i < ins->info->num_operands; i++)
                        if ((ins->info->operands[i] & MVM_operand_rw_mask) == MVM_operand_read_reg)
                            get_facts(tc, g, ins->operands[i])->usages--;

                    /* Remove this instruction. */
                    MVM_spesh_manipulate_delete_ins(tc, bb, ins);
                }
            }
        }
        ins = prev;
    }
}

/* Eliminates any unreachable basic blocks (that is, dead code). Not having
 * to consider them any further simplifies all that follows. */
static void eliminate_dead(MVMThreadContext *tc, MVMSpeshGraph *g) {
    /* Iterate to fixed point. */
    MVMint8  *seen     = malloc(g->num_bbs);
    MVMint32  orig_bbs = g->num_bbs;
    MVMint8   death    = 1;
    while (death) {
        /* First pass: mark every basic block that is the entry point or the
         * successor of some other block. */
        MVMSpeshBB *cur_bb = g->entry;
        memset(seen, 0, g->num_bbs);
        seen[0] = 1;
        while (cur_bb) {
            MVMuint16 i;
            for (i = 0; i < cur_bb->num_succ; i++)
                seen[cur_bb->succ[i]->idx] = 1;
            cur_bb = cur_bb->linear_next;
        }

        /* Second pass: eliminate dead BBs from consideration. */
        death = 0;
        cur_bb = g->entry;
        while (cur_bb->linear_next) {
            if (!seen[cur_bb->linear_next->idx]) {
                cur_bb->linear_next = cur_bb->linear_next->linear_next;
                g->num_bbs--;
                death = 1;
            }
            cur_bb = cur_bb->linear_next;
        }
    }
    free(seen);

    if (g->num_bbs != orig_bbs) {
        MVMint32    new_idx  = 0;
        MVMSpeshBB *cur_bb   = g->entry;
        while (cur_bb) {
            cur_bb->idx = new_idx;
            new_idx++;
            cur_bb = cur_bb->linear_next;
        }
    }
}

/* Drives the overall optimization work taking place on a spesh graph. */
void MVM_spesh_optimize(MVMThreadContext *tc, MVMSpeshGraph *g) {
    optimize_bb(tc, g, g->entry);
    eliminate_dead(tc, g);
}