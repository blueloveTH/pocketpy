#ifdef OPCODE

OPCODE(NO_OP)
OPCODE(IMPORT_NAME)
OPCODE(PRINT_EXPR)
OPCODE(POP_TOP)
OPCODE(DUP_TOP)
OPCODE(CALL)
OPCODE(RETURN_VALUE)

OPCODE(BINARY_OP)
OPCODE(COMPARE_OP)
OPCODE(BITWISE_OP)
OPCODE(IS_OP)
OPCODE(CONTAINS_OP)

OPCODE(UNARY_NEGATIVE)
OPCODE(UNARY_NOT)

OPCODE(BUILD_LIST)
OPCODE(BUILD_MAP)
OPCODE(BUILD_SET)
OPCODE(BUILD_SLICE)

OPCODE(LIST_APPEND)

OPCODE(GET_ITER)
OPCODE(FOR_ITER)

OPCODE(WITH_ENTER)
OPCODE(WITH_EXIT)
OPCODE(LOOP_BREAK)
OPCODE(LOOP_CONTINUE)

OPCODE(POP_JUMP_IF_FALSE)
OPCODE(JUMP_ABSOLUTE)
OPCODE(SAFE_JUMP_ABSOLUTE)
OPCODE(JUMP_IF_TRUE_OR_POP)
OPCODE(JUMP_IF_FALSE_OR_POP)

OPCODE(LOAD_CONST)
OPCODE(LOAD_NONE)
OPCODE(LOAD_TRUE)
OPCODE(LOAD_FALSE)
OPCODE(LOAD_EVAL_FN)
OPCODE(LOAD_LAMBDA)
OPCODE(LOAD_ELLIPSIS)
OPCODE(LOAD_NAME)
OPCODE(LOAD_NAME_REF)

OPCODE(ASSERT)
OPCODE(RAISE_ERROR)

OPCODE(STORE_FUNCTION)
OPCODE(BUILD_CLASS)
OPCODE(BUILD_ATTR_REF)
OPCODE(BUILD_INDEX_REF)
OPCODE(STORE_NAME_REF)
OPCODE(STORE_REF)
OPCODE(DELETE_REF)

OPCODE(BUILD_SMART_TUPLE)
OPCODE(BUILD_STRING)

OPCODE(GOTO)

#endif