# Windows-only AppUserModelID NIF. Non-Windows: no-op (Elixir stub returns :ok).
PRIV_DIR = $(MIX_APP_PATH)/priv/native/windows
NIF_NAME = aumid_nif

ifeq ($(OS),Windows_NT)
  NIF = $(PRIV_DIR)/$(NIF_NAME).dll
  CC ?= gcc
  CFLAGS ?= -O2 -shared -fPIC
  ERTS_INCLUDE ?= $(ERTS_INCLUDE_DIR)
  all: $(NIF)
  $(NIF): c_src/aumid_nif.c
	@mkdir -p "$(PRIV_DIR)"
	$(CC) $(CFLAGS) -I"$(ERTS_INCLUDE)" -o "$@" c_src/aumid_nif.c -lshell32 -lole32
else
  all:
	@mkdir -p "$(PRIV_DIR)"
	@echo "aumid_nif: skipped (non-Windows)"
endif

clean:
	rm -f "$(PRIV_DIR)/$(NIF_NAME).dll" "$(PRIV_DIR)/$(NIF_NAME).so"
