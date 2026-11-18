#include <erl_nif.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

static ERL_NIF_TERM atom_ok;
static ERL_NIF_TERM atom_error;

static int on_load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info) {
  (void)priv_data;
  (void)load_info;
  atom_ok = enif_make_atom(env, "ok");
  atom_error = enif_make_atom(env, "error");
  return 0;
}

static ERL_NIF_TERM set_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  (void)argc;
#ifndef _WIN32
  (void)argv;
  return atom_ok;
#else
  char id[512];
  if (enif_get_string(env, argv[0], id, sizeof(id), ERL_NIF_LATIN1) <= 0) {
    ErlNifBinary bin;
    if (!enif_inspect_binary(env, argv[0], &bin) || bin.size >= sizeof(id)) {
      return enif_make_badarg(env);
    }
    memcpy(id, bin.data, bin.size);
    id[bin.size] = '\0';
  }
  if (id[0] == '\0') return atom_ok;

  int n = MultiByteToWideChar(CP_UTF8, 0, id, -1, NULL, 0);
  if (n <= 0) return enif_make_tuple2(env, atom_error, enif_make_atom(env, "utf8"));
  WCHAR *wide = (WCHAR *)enif_alloc(sizeof(WCHAR) * (size_t)n);
  if (!wide) return enif_make_tuple2(env, atom_error, enif_make_atom(env, "alloc"));
  MultiByteToWideChar(CP_UTF8, 0, id, -1, wide, n);
  HRESULT hr = SetCurrentProcessExplicitAppUserModelID(wide);
  enif_free(wide);
  if (FAILED(hr)) {
    return enif_make_tuple2(env, atom_error, enif_make_int(env, (int)hr));
  }
  return atom_ok;
#endif
}

static ErlNifFunc nif_funcs[] = {{"set_nif", 1, set_nif}};

ERL_NIF_INIT(Elixir.DesktopWebview.AppUserModelId, nif_funcs, on_load, NULL, NULL, NULL)
