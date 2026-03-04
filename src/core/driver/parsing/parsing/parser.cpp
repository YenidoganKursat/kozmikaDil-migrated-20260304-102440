// Parser modülerizasyonu:
// - 00: shared utility + error + assignment/trim helpers
// - 01: parser core + block traversal
// - 02: statement dispatch and clause parsing
// - 03: expression entry + precedence chain
// - 04: unary/primary/postfix expression parsing

#include "parser_parts/00a_parser_text_utils.cpp"
#include "parser_parts/00b_parser_state_utils.cpp"
#include "parser_parts/01_parser_core.cpp"
#include "parser_parts/02a_parser_statement_dispatch.cpp"
#include "parser_parts/02b_parser_control_statements.cpp"
#include "parser_parts/02c_parser_definition_statements.cpp"
#include "parser_parts/03_parser_expressions.cpp"
#include "parser_parts/04a_parser_expression_unary_primary.cpp"
#include "parser_parts/04b_parser_expression_suffix.cpp"
