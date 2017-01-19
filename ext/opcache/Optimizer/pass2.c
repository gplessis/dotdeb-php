/*
   +----------------------------------------------------------------------+
   | Zend OPcache                                                         |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2017 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@zend.com>                                |
   |          Zeev Suraski <zeev@zend.com>                                |
   |          Stanislav Malyshev <stas@zend.com>                          |
   |          Dmitry Stogov <dmitry@zend.com>                             |
   +----------------------------------------------------------------------+
*/

/* pass 2:
 * - convert non-numeric constants to numeric constants in numeric operators
 * - optimize constant conditional JMPs
 * - optimize static BRKs and CONTs
 */

#include "php.h"
#include "Optimizer/zend_optimizer.h"
#include "Optimizer/zend_optimizer_internal.h"
#include "zend_API.h"
#include "zend_constants.h"
#include "zend_execute.h"
#include "zend_vm.h"

void zend_optimizer_pass2(zend_op_array *op_array)
{
	zend_op *opline;
	zend_op *end = op_array->opcodes + op_array->last;

	opline = op_array->opcodes;
	while (opline < end) {
		switch (opline->opcode) {
			case ZEND_ADD:
			case ZEND_SUB:
			case ZEND_MUL:
			case ZEND_DIV:
			case ZEND_POW:
				if (ZEND_OP1_TYPE(opline) == IS_CONST) {
					if (Z_TYPE(ZEND_OP1_LITERAL(opline)) == IS_STRING) {
						convert_scalar_to_number(&ZEND_OP1_LITERAL(opline));
					}
				}
				/* break missing *intentionally* - the assign_op's may only optimize op2 */
			case ZEND_ASSIGN_ADD:
			case ZEND_ASSIGN_SUB:
			case ZEND_ASSIGN_MUL:
			case ZEND_ASSIGN_DIV:
			case ZEND_ASSIGN_POW:
				if (opline->extended_value != 0) {
					/* object tristate op - don't attempt to optimize it! */
					break;
				}
				if (ZEND_OP2_TYPE(opline) == IS_CONST) {
					if (Z_TYPE(ZEND_OP2_LITERAL(opline)) == IS_STRING) {
						convert_scalar_to_number(&ZEND_OP2_LITERAL(opline));
					}
				}
				break;

			case ZEND_MOD:
			case ZEND_SL:
			case ZEND_SR:
				if (ZEND_OP1_TYPE(opline) == IS_CONST) {
					if (Z_TYPE(ZEND_OP1_LITERAL(opline)) != IS_LONG) {
						convert_to_long(&ZEND_OP1_LITERAL(opline));
					}
				}
				/* break missing *intentionally - the assign_op's may only optimize op2 */
			case ZEND_ASSIGN_MOD:
			case ZEND_ASSIGN_SL:
			case ZEND_ASSIGN_SR:
				if (opline->extended_value != 0) {
					/* object tristate op - don't attempt to optimize it! */
					break;
				}
				if (ZEND_OP2_TYPE(opline) == IS_CONST) {
					if (Z_TYPE(ZEND_OP2_LITERAL(opline)) != IS_LONG) {
						convert_to_long(&ZEND_OP2_LITERAL(opline));
					}
				}
				break;

			case ZEND_CONCAT:
			case ZEND_FAST_CONCAT:
				if (ZEND_OP1_TYPE(opline) == IS_CONST) {
					if (Z_TYPE(ZEND_OP1_LITERAL(opline)) != IS_STRING) {
						convert_to_string(&ZEND_OP1_LITERAL(opline));
					}
				}
				/* break missing *intentionally - the assign_op's may only optimize op2 */
			case ZEND_ASSIGN_CONCAT:
				if (opline->extended_value != 0) {
					/* object tristate op - don't attempt to optimize it! */
					break;
				}
				if (ZEND_OP2_TYPE(opline) == IS_CONST) {
					if (Z_TYPE(ZEND_OP2_LITERAL(opline)) != IS_STRING) {
						convert_to_string(&ZEND_OP2_LITERAL(opline));
					}
				}
				break;

			case ZEND_JMPZ_EX:
			case ZEND_JMPNZ_EX:
				/* convert Ti = JMPZ_EX(Ti, L) to JMPZ(Ti, L) */
				if (0 && /* FIXME: temporary disable unsafe pattern */
				    ZEND_OP1_TYPE(opline) == IS_TMP_VAR &&
				    ZEND_RESULT_TYPE(opline) == IS_TMP_VAR &&
				    ZEND_OP1(opline).var == ZEND_RESULT(opline).var) {
					opline->opcode -= 3;
				/* convert Ti = JMPZ_EX(C, L) => Ti = QM_ASSIGN(C)
				   in case we know it wouldn't jump */
				} else if (ZEND_OP1_TYPE(opline) == IS_CONST) {
					int should_jmp = zend_is_true(&ZEND_OP1_LITERAL(opline));
					if (opline->opcode == ZEND_JMPZ_EX) {
						should_jmp = !should_jmp;
					}
					if (!should_jmp) {
						opline->opcode = ZEND_QM_ASSIGN;
						SET_UNUSED(opline->op2);
					}
				}
				break;

			case ZEND_JMPZ:
			case ZEND_JMPNZ:
				if (ZEND_OP1_TYPE(opline) == IS_CONST) {
					int should_jmp = zend_is_true(&ZEND_OP1_LITERAL(opline));

					if (opline->opcode == ZEND_JMPZ) {
						should_jmp = !should_jmp;
					}
					literal_dtor(&ZEND_OP1_LITERAL(opline));
					ZEND_OP1_TYPE(opline) = IS_UNUSED;
					if (should_jmp) {
						opline->opcode = ZEND_JMP;
						COPY_NODE(opline->op1, opline->op2);
					} else {
						MAKE_NOP(opline);
					}
					break;
				}
				if ((opline + 1)->opcode == ZEND_JMP) {
					/* JMPZ(X, L1), JMP(L2) => JMPZNZ(X, L1, L2) */
					/* JMPNZ(X, L1), JMP(L2) => JMPZNZ(X, L2, L1) */
					if (ZEND_OP2(opline).opline_num == ZEND_OP1(opline + 1).opline_num) {
						/* JMPZ(X, L1), JMP(L1) => NOP, JMP(L1) */
						MAKE_NOP(opline);
					} else {
						if (opline->opcode == ZEND_JMPZ) {
							opline->extended_value = ZEND_OP1(opline + 1).opline_num;
						} else {
							opline->extended_value = ZEND_OP2(opline).opline_num;
							COPY_NODE(opline->op2, (opline + 1)->op1);
						}
						opline->opcode = ZEND_JMPZNZ;
					}
				}
				break;

			case ZEND_JMPZNZ:
				if (ZEND_OP1_TYPE(opline) == IS_CONST) {
					int opline_num;
					if (zend_is_true(&ZEND_OP1_LITERAL(opline))) {
						opline_num = opline->extended_value; /* JMPNZ */
					} else {
						opline_num = ZEND_OP2(opline).opline_num; /* JMPZ */
					}
					literal_dtor(&ZEND_OP1_LITERAL(opline));
					ZEND_OP1(opline).opline_num = opline_num;
					ZEND_OP1_TYPE(opline) = IS_UNUSED;
					opline->opcode = ZEND_JMP;
				}
				break;
		}
		opline++;
	}
}
