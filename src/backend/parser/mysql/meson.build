# OpenHalo MySQL parser, built separately for generated grammar headers.
mysql_parser_sources = files(
  'mys_analyze.c', 'mys_parse_agg.c', 'mys_parse_clause.c',
  'mys_parse_expr.c', 'mys_parse_func.c', 'mys_parse_oper.c',
  'mys_parse_utilcmd.c', 'mys_keywords.c', 'mys_parser.c',
)
mysql_kwlist = custom_target('mys_kwlist',
  input: files('../../../include/parser/mysql/mys_kwlist.h'),
  output: 'mys_kwlist_d.h',
  depend_files: gen_kwlist_deps,
  command: [gen_kwlist_cmd, '--extern', '--varname=MysScanKeywords'])
mysql_scanner = custom_target('mys_scan',
  input: 'mys_scan.l',
  output: 'mys_scan.c',
  command: [flex_cmd, '--no-backup', '--', '-CF', '-p', '-p'],
)
mysql_grammar = custom_target('mys_gram',
  input: 'mys_gram.y',
  kwargs: bison_kw,
)
generated_sources += [mysql_kwlist, mysql_scanner] + mysql_grammar.to_list()
mysql_parser_sources += [mysql_kwlist, mysql_scanner, mysql_grammar]
mysql_parser = static_library('mysql_parser',
  mysql_parser_sources,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)
backend_link_with += mysql_parser
