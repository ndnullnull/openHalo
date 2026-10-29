mysm_sources = files(
  'aggregates.c',
  'bpchar.c',
  'common_funcs.c',
  'date_func.c',
  'operators.c',
  'partListColumns.c',
  'strfuncs.c',
  'systemVar.c',
  'time_func.c',
  'timestamp_func.c',
  'user.c',
  'user_var.c',
  'uuid_short.c',
)
mysm = shared_module('mysm', mysm_sources, kwargs: pg_mod_args)
backend_targets += mysm
