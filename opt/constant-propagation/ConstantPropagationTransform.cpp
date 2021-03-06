/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagationTransform.h"

#include "Transform.h"

namespace constant_propagation {

/*
 * Replace an instruction that has a single destination register with a `const`
 * load. `env` holds the state of the registers after `insn` has been
 * evaluated. So, `env.get(dest)` holds the _new_ value of the destination
 * register.
 */
void Transform::replace_with_const(const ConstantEnvironment& env,
                                   IRList::iterator it) {
  auto* insn = it->insn;
  auto cst = env.get_primitive(insn->dest()).constant_domain().get_constant();
  if (!cst) {
    return;
  }
  IRInstruction* replacement = new IRInstruction(
      insn->dest_is_wide() ? OPCODE_CONST_WIDE : OPCODE_CONST);
  replacement->set_literal(*cst);
  replacement->set_dest(insn->dest());

  TRACE(CONSTP, 5, "Replacing %s with %s\n", SHOW(insn), SHOW(replacement));
  if (opcode::is_move_result_pseudo(insn->opcode())) {
    m_replacements.emplace_back(std::prev(it)->insn, replacement);
  } else {
    m_replacements.emplace_back(insn, replacement);
  }
  ++m_stats.materialized_consts;
}

void Transform::simplify_instruction(const ConstantEnvironment& env,
                                     const WholeProgramState& wps,
                                     IRList::iterator it) {
  auto* insn = it->insn;
  switch (insn->opcode()) {
  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
    if (m_config.replace_moves_with_consts) {
      replace_with_const(env, it);
    }
    break;
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
    auto* primary_insn = ir_list::primary_instruction_of_move_result_pseudo(it);
    auto op = primary_insn->opcode();
    if (is_sget(op) || is_aget(op)) {
      replace_with_const(env, it);
    }
    break;
  }
  // We currently don't replace move-result opcodes with consts because it's
  // unlikely that we can get a more compact encoding (move-result can address
  // 8-bit register operands while taking up just 1 code unit). However it can
  // be a net win if we can remove the invoke opcodes as well -- we need a
  // purity analysis for that though.
  /*
  case OPCODE_MOVE_RESULT:
  case OPCODE_MOVE_RESULT_WIDE:
  case OPCODE_MOVE_RESULT_OBJECT: {
    replace_with_const(it, env);
    break;
  }
  */
  case OPCODE_SPUT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_SHORT:
  case OPCODE_SPUT_WIDE: {
    auto* field = resolve_field(insn->get_field());
    auto cst = wps.get_field_value(field).constant_domain().get_constant();
    if (cst) {
      // This field is known to be constant and must already hold this value.
      // We don't need to write to it again.
      m_deletes.push_back(it);
    }
    break;
  }
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_ADD_INT_LIT8: {
    replace_with_const(env, it);
    break;
  }

  default: {}
  }
}

/*
 * If the last instruction in a basic block is an if-* instruction, determine
 * whether it is dead (i.e. whether the branch always taken or never taken).
 * If it is, we can replace it with either a nop or a goto.
 */
void Transform::eliminate_dead_branch(
    const intraprocedural::FixpointIterator& intra_cp,
    const ConstantEnvironment& env,
    cfg::Block* block) {
  auto insn_it = transform::find_last_instruction(block);
  if (insn_it == block->end()) {
    return;
  }
  auto* insn = insn_it->insn;
  if (!is_conditional_branch(insn->opcode())) {
    return;
  }
  always_assert_log(block->succs().size() == 2, "actually %d\n%s",
                    block->succs().size(), SHOW(InstructionIterable(*block)));
  for (auto& edge : block->succs()) {
    // Check if the fixpoint analysis has determined the successors to be
    // unreachable
    if (intra_cp.analyze_edge(edge, env).is_bottom()) {
      auto is_fallthrough = edge->type() == cfg::EDGE_GOTO;
      TRACE(CONSTP, 2, "Changed conditional branch %s as it is always %s\n",
            SHOW(insn), is_fallthrough ? "true" : "false");
      ++m_stats.branches_removed;
      if (is_fallthrough) {
        m_replacements.emplace_back(insn, new IRInstruction(OPCODE_GOTO));
      } else {
        m_deletes.emplace_back(insn_it);
      }
      // Assuming :block is reachable, then at least one of its successors must
      // be reachable, so we can break after finding one that's unreachable
      break;
    }
  }
}

void Transform::apply_changes(IRCode* code) {
  for (auto const& p : m_replacements) {
    IRInstruction* old_op = p.first;
    IRInstruction* new_op = p.second;
    TRACE(CONSTP, 4, "Replacing instruction %s -> %s\n", SHOW(old_op),
          SHOW(new_op));
    if (is_branch(old_op->opcode())) {
      code->replace_branch(old_op, new_op);
    } else {
      code->replace_opcode(old_op, new_op);
    }
  }
  for (auto it : m_deletes) {
    TRACE(CONSTP, 4, "Removing instruction %s\n", SHOW(it->insn));
    code->remove_opcode(it);
  }
}

Transform::Stats Transform::apply(
    const intraprocedural::FixpointIterator& intra_cp,
    const WholeProgramState& wps,
    IRCode* code) {
  auto& cfg = code->cfg();
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    // This block is unreachable, no point mutating its instructions -- DCE
    // will be removing it anyway
    if (env.is_bottom()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      intra_cp.analyze_instruction(mie.insn, &env);
      simplify_instruction(env, wps, code->iterator_to(mie));
    }
    eliminate_dead_branch(intra_cp, env, block);
  }
  apply_changes(code);
  return m_stats;
}

} // namespace constant_propagation
