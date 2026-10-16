# Quick Settings — alvos de desenvolvimento e validação.
# analyze: análise estática profunda (GCC -fanalyzer) com as flags do build oficial.
# cppcheck: opcional, roda se estiver no PATH.
# smoke: build + verificação de dlopen + roteiro manual.
# CI futuro sugerido: job com -fsanitize=address,undefined -fno-sanitize-recover=all.

PKGS = gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3
CFLAGS_QS = -Wall -Wextra -Wconversion -g -std=gnu11 -fPIC -O2 -shared -D_FORTIFY_SOURCE=3
SRCS = $(wildcard src/*.c)

.PHONY: build analyze cppcheck smoke

build:
	sh install/common/build.sh

analyze:
	gcc -fanalyzer $(CFLAGS_QS) -o /tmp/quicksettings-analyze.so $(SRCS) \
	  $(shell pkg-config --cflags $(PKGS)) $(shell pkg-config --libs $(PKGS)) -lsqlite3

cppcheck:
	@command -v cppcheck >/dev/null 2>&1 && \
	  cppcheck --enable=warning,performance,portability --std=c11 -q src/ || \
	  echo "cppcheck não encontrado no PATH — alvo pulado"

smoke: build
	sh scripts/smoke.sh
